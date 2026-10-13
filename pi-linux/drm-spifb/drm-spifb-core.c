// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for NumWorks SPI framebuffer display
 *
 * The NumWorks calculator's STM32 acts as an SPI slave that receives
 * raw RGB565 pixels and DMA-transfers them directly to the LCD.
 * No init commands, no MIPI DBI protocol — just raw pixel data.
 *
 * This is a DRM/KMS driver using drm_simple_display_pipe, which means:
 *   - KMS composites directly to this display (no fbcp needed)
 *   - Works with Wayland, X11, and fbcon
 *   - Proper modern Linux graphics stack
 *
 * Uses async SPI with double buffering: CPU builds frame N+1 into one
 * buffer while SPI DMA sends frame N from the other. This overlaps
 * CPU and SPI work, achieving the SPI bus speed limit (~48 FPS).
 *
 * Framebuffers are allocated cached (map_noncoherent). Nothing DMAs from
 * them — frames are always converted into tx_buf first — so there is no
 * coherency cost, and both the compositor and this driver read them at
 * full cache speed instead of through a write-combined mapping.
 *
 * The compositor draws at 1.5x, 2x (default), 2.5x or 3x the panel size,
 * offered as display modes it can switch between at runtime.
 *
 * Downscaling (virtual -> physical resolution) uses an area filter by
 * default: every output pixel is the coverage-weighted average of the
 * source pixels under it, so thin lines and small text survive instead
 * of being dropped by nearest-neighbour sampling (exact 2:1 has a packed
 * 2x2 fast path). Output is ordered-dithered to hide RGB565 banding. Only
 * the damaged region is rebuilt; the rest is copied from the previous frame.
 *
 * Protocol 2 (firmware that answers "DSP:2" on the UART; nwpid negotiates it
 * and sets the protocol parameter): each update is an 8-word header frame
 * (memory access mode + window), a CS-high gap, then only the damaged
 * rectangle in the panel's native scan order; separate damaged areas (a
 * clock and a cursor) go as up to NW_MAX_RECTS such pairs in one message. The calculator times every
 * update against the LCD's tearing-effect line and adjusts the panel refresh
 * rate so the scan never crosses the write: no tearing, and small updates
 * (cursor, typing, a moving window's edges) cost only their own pixels.
 *
 * Inspired by zardam's SPI display concept.
 * Based on drivers/gpu/drm/tiny/repaper.c skeleton pattern.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#if __has_include(<drm/clients/drm_client_setup.h>)
#include <drm/clients/drm_client_setup.h>	/* 6.15+ */
#else
#include <drm/drm_client_setup.h>
#endif
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#if IS_ENABLED(CONFIG_KERNEL_MODE_NEON)
#include <linux/fpu.h>
#include "drm-spifb-neon.h"
#endif

#define DRIVER_NAME	"drm-spifb"
#define DRIVER_DESC	"NumWorks SPI framebuffer display"

/* Physical panel limits */
#define MAX_WIDTH	320
#define MAX_HEIGHT	240

/*
 * Desktop sizes offered to the compositor, in half-steps of the panel size:
 * 1.5x, 2x (the default), 2.5x and 3x. Each is drawn at that size and
 * downscaled onto the panel; three filter taps per axis cover exactly these
 * ratios (an arbitrary ratio up to 3x could need four).
 */
static const u8 nw_mode_halves[] = { 3, 4, 5, 6 };
#define NW_MAX_SCALE	3

/* Row cache slots for horizontally filtered source rows (power of two, >= 3) */
#define HROW_SLOTS	4

static bool area_filter = true;
module_param(area_filter, bool, 0644);
MODULE_PARM_DESC(area_filter,
		 "Downscale with an area filter (1, default) or nearest-neighbour (0)");

static bool dither = true;
module_param(dither, bool, 0644);
MODULE_PARM_DESC(dither, "Ordered dithering to hide RGB565 colour banding (default on)");

#if IS_ENABLED(CONFIG_KERNEL_MODE_NEON)
static bool simd = true;
module_param(simd, bool, 0644);
MODULE_PARM_DESC(simd, "Use NEON for the 2x downscale when the CPU has it (default on; same output)");
#endif

static unsigned int pixel_order;
module_param(pixel_order, uint, 0644);
MODULE_PARM_DESC(pixel_order,
		 "SPI pixel order: 0 = landscape rows (default, stock bridge firmware), "
		 "1 = panel-native columns right to left, 2 = panel-native columns left to right");

static unsigned int protocol;
module_param(protocol, uint, 0644);
MODULE_PARM_DESC(protocol,
		 "Bridge protocol: 0 = full landscape frames (default), "
		 "2 = header + damaged rectangle in panel order (set by nwpid)");

static unsigned int header_gap_us = 40;
module_param(header_gap_us, uint, 0644);
MODULE_PARM_DESC(header_gap_us,
		 "Protocol 2: CS-high time between header and pixels, for the "
		 "calculator to program the LCD window (default 40, max 60)");

static unsigned int spi_hz;
module_param(spi_hz, uint, 0644);
MODULE_PARM_DESC(spi_hz,
		 "SPI clock, 0 = the DT's spi-max-frequency (default). Lower it "
		 "to keep a marginal link working: a degraded data wire first "
		 "fails at the high rates");

/* Protocol 2 header: magic, version, MADCTL, column window, page window, check */
#define NW_HDR_WORDS	8
#define NW_HDR_MAGIC	0x4e57
#define NW_HDR_VERSION	2
#define NW_MADCTL_NATIVE	0x00	/* pages along the scan, column = y */
/*
 * The SPI core spins for delays up to 10 us and sleeps above, waking tens
 * of us late under load. Longer CS-high gaps are chained from 10 us steps
 * so they stay exact: the calculator's tearing model assumes the pixels
 * start right after the header gap.
 */
#define NW_GAP_STEP_US	10
#define NW_MAX_GAP_US	60
/* Minimum CS-high time after a frame, for the calculator to re-arm */
#define NW_FRAME_GAP_US	10
/* Damage clips looked at per update; more are folded into the last one */
#define NW_MAX_CLIPS	32
/* Protocol 2: damaged rectangles sent separately in one update, at most */
#define NW_MAX_RECTS	8
/* Header, gap steps after its own, pixels */
#define NW_XFERS_PER_RECT	(1 + NW_MAX_GAP_US / NW_GAP_STEP_US)
/*
 * What one more rectangle costs on the wire, in pixels: its header, the
 * gaps around it and the calculator's interrupts, about 25 us at ~0.24 us
 * per pixel, plus the SPI core's per-transfer setup on the Pi.
 */
#define NW_RECT_COST	256

/*
 * Filter taps for one output pixel along one axis: source index of the
 * first tap and up to three 8-bit weights summing to 256 (enough for every
 * ratio in nw_mode_halves).
 */
struct nw_taps {
	u16 start;
	u16 w[3];
};

/* Output (panel landscape) rectangle [x0, x1) x [y0, y1) */
struct nw_rect {
	u32 x0, x1, y0, y1;
};

struct nw_spifb {
	struct drm_device drm;
	struct spi_device *spi;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	u32 width;		/* Physical SPI display width (320) */
	u32 height;		/* Physical SPI display height (240) */
	u32 vwidth;		/* Virtual (compositor) width, the current mode */
	u32 vheight;		/* Virtual (compositor) height */
	u32 pref_vwidth;	/* Preferred mode, from DT */
	u32 pref_vheight;

	struct nw_taps xtaps[MAX_WIDTH];
	struct nw_taps ytaps[MAX_HEIGHT];
	bool taps_area;		/* Filter the taps were built for */

	u32 *src_row;		/* RGB565 source row expanded to XRGB8888 */
	u32 *hrow[HROW_SLOTS];	/* Horizontally filtered rows, physical width */
	int hrow_tag[HROW_SLOTS];	/* Source row held by each slot, -1 = none */

	bool full_update;	/* Rebuild the whole frame next time */
	u32 last_format;
	unsigned int last_order;
	bool last_dither;
	unsigned int last_protocol;

	/* Protocol 2: whole screen in panel order; tx_buf gets the packed rects */
	u16 *canvas;
	u16 *tx_hdr[2];			/* NW_MAX_RECTS headers each */
	u32 tx_rect_len[NW_MAX_RECTS];	/* Pixel bytes of each packed rect */
	int tx_nrects;
	struct nw_rect clips[NW_MAX_CLIPS];	/* This update's damage */

	struct mutex lock;		/* Building and submitting a frame */
	struct work_struct repaint_work;	/* Resend the whole screen */

	/* Double-buffered async SPI */
	void *tx_buf[2];
	int tx_write;			/* Buffer index CPU writes to next */
	u32 tx_len;			/* Bytes of tx_buf[tx_write] to send */
	bool tx_v2;			/* Send tx_hdr[tx_write] first */
	struct spi_message tx_msg;
	struct spi_transfer tx_xfer[NW_MAX_RECTS * NW_XFERS_PER_RECT];
	struct completion tx_done;	/* Signals SPI transfer complete */
	ktime_t tx_end;			/* When the last transfer completed */

	/* Cumulative, read from debugfs (dri/N/spifb_stats) */
	struct {
		u64 updates, rects, bytes;
		u64 prep_ns, prep_max_ns;	/* Filtering and packing */
		u64 wait_ns, wait_max_ns;	/* Blocked on the previous transfer */
	} stats;
};

static inline struct nw_spifb *drm_to_nw(struct drm_device *drm)
{
	return container_of(drm, struct nw_spifb, drm);
}

/*
 * Build per-output-pixel taps mapping @src source pixels onto @dst outputs.
 * Area: weights are the overlap of each source pixel with the output
 * pixel's footprint. Nearest: one tap at the footprint's centre.
 * All three taps are always read, so start is pulled back where needed
 * to keep start + 2 inside the row (the extra taps carry zero weight).
 */
static void nw_build_taps(struct nw_taps *t, u32 dst, u32 src, bool area)
{
	u32 i, j, k;

	for (i = 0; i < dst; i++) {
		/* Footprint [lo, hi) in units of 1/dst source pixels */
		u32 lo = i * src, hi = (i + 1) * src;
		u32 sum = 0, big = 0, shift;

		memset(&t[i], 0, sizeof(t[i]));

		if (!area || src == dst) {
			t[i].start = (lo + hi) / (2 * dst);
			t[i].w[0] = 256;
		} else {
			j = lo / dst;
			t[i].start = j;
			for (k = 0; k < 3 && j * dst < hi; k++, j++) {
				u32 a = max(lo, j * dst), b = min(hi, (j + 1) * dst);

				t[i].w[k] = (b - a) * 256 / src;
				sum += t[i].w[k];
				if (t[i].w[k] > t[i].w[big])
					big = k;
			}
			t[i].w[big] += 256 - sum;	/* rounding remainder */
		}

		if (t[i].start + 3 > src) {
			shift = t[i].start + 3 - src;
			t[i].start -= shift;
			for (k = 3; k-- > shift; )
				t[i].w[k] = t[i].w[k - shift];
			for (k = 0; k < shift; k++)
				t[i].w[k] = 0;
		}
	}
}

/*
 * Source row @sy as XRGB8888, valid over [x0, x1). XRGB framebuffers are
 * cached memory and read in place; RGB565 (fbcon) is expanded into src_row.
 */
static const u32 *nw_src_row(struct nw_spifb *nw, const u8 *src, u32 pitch,
			     u32 format, u32 sy, u32 x0, u32 x1)
{
	const u8 *row = src + sy * pitch;
	u32 *dst = nw->src_row;
	u32 x;

	if (format != DRM_FORMAT_RGB565)
		return (const u32 *)row;

	for (x = x0; x < x1; x++) {
		u32 p = ((const u16 *)row)[x];
		u32 r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;

		dst[x] = ((r << 3 | r >> 2) << 16) |
			 ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
	}
	return dst;
}

/*
 * Weighted sum of three XRGB8888 pixels, weights summing to 256. Red and
 * blue share one 32-bit accumulator (16-bit lanes cannot overflow:
 * 255 * 256 + 128 < 65536), green gets its own.
 */
static inline u32 nw_blend3(u32 a, u32 b, u32 c, u32 wa, u32 wb, u32 wc)
{
	u32 rb = (a & 0xff00ff) * wa + (b & 0xff00ff) * wb + (c & 0xff00ff) * wc;
	u32 g = (a & 0xff00) * wa + (b & 0xff00) * wb + (c & 0xff00) * wc;

	return (((rb + 0x800080) >> 8) & 0xff00ff) | (((g + 0x8000) >> 8) & 0xff00);
}

/* Return source row @sy filtered horizontally over outputs [x0, x1) */
static const u32 *nw_get_hrow(struct nw_spifb *nw, const u8 *src, u32 pitch,
			      u32 format, u32 sy, u32 x0, u32 x1)
{
	int slot = sy & (HROW_SLOTS - 1);
	const u32 *row;
	u32 *out = nw->hrow[slot];
	u32 x;

	if (nw->hrow_tag[slot] == sy)
		return out;

	row = nw_src_row(nw, src, pitch, format, sy, nw->xtaps[x0].start,
			 nw->xtaps[x1 - 1].start + 3);
	for (x = x0; x < x1; x++) {
		const struct nw_taps *t = &nw->xtaps[x];
		const u32 *p = row + t->start;

		out[x] = nw_blend3(p[0], p[1], p[2], t->w[0], t->w[1], t->w[2]);
	}
	nw->hrow_tag[slot] = sy;

	return out;
}

/*
 * 4x4 Bayer thresholds pre-scaled for RGB565 truncation, packed as the
 * value to add to an XRGB8888 pixel: 0..7 to red and blue (3 bits lost),
 * 0..3 to green (2 bits lost).
 */
static const u8 nw_bayer4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

static u32 nw_dither[4][4];

static void nw_init_dither(void)
{
	int x, y;

	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			u32 t = nw_bayer4[y][x];

			nw_dither[y][x] = (t >> 1) << 16 | (t >> 2) << 8 | (t >> 1);
		}
}

/*
 * XRGB8888 -> big-endian RGB565, adding @d (a nw_dither entry or 0) before
 * truncating. Channels are first scaled by 31/32 (63/64 for green) so that
 * truncation approximates v * 31 / 255 instead of v / 8, which reads ~2.5%
 * too bright in the midtones; it also caps lanes at 248 (252) so adding
 * the dither can never carry into the next channel.
 */
static inline u16 nw_pack(u32 c, u32 d)
{
	u32 rb = (c & 0xff00ff) - ((c >> 5) & 0x070007) + (d & 0xff00ff);
	u32 g = (c & 0xff00) - ((c >> 6) & 0x0300) + (d & 0xff00);

	return swab16(((rb >> 8) & 0xf800) | ((g >> 5) & 0x07e0) | ((rb >> 3) & 0x001f));
}

/* Index of output pixel (0, y) in tx and the step per x, for @order */
static inline void nw_order_base(struct nw_spifb *nw, unsigned int order, u32 y,
				 int *base, int *step)
{
	switch (order) {
	case 1:		/* native: column x at page w-1-x */
		*base = (nw->width - 1) * nw->height + y;
		*step = -(int)nw->height;
		break;
	case 2:		/* native: column x at page x */
		*base = y;
		*step = nw->height;
		break;
	default:	/* landscape rows */
		*base = y * nw->width;
		*step = 1;
		break;
	}
}

/* Output rows per kernel_fpu_begin(), which holds off softirqs: ~50 us */
#define NW_SIMD_ROWS	16

/*
 * Exact 2:1 area filter for XRGB8888: plain 2x2 box average with packed
 * adds (four 8-bit values fit a 16-bit lane), each source pixel read once.
 * With NEON, whole groups of 8 output pixels go through nw_neon_2x_row()
 * (same result) and only the remainder takes the C loop.
 */
static void nw_build_rect_2x(struct nw_spifb *nw, const u8 *src, u32 pitch,
			     u16 *tx, unsigned int order, bool dither,
			     u32 x0, u32 x1, u32 y0, u32 y1)
{
	u32 x, y;
#if IS_ENABLED(CONFIG_KERNEL_MODE_NEON)
	u32 n8 = simd && kernel_fpu_available() ? (x1 - x0) / 8 : 0;
	u16 row[MAX_WIDTH];
	u8 drb[8], dg[8];
#endif

	for (y = y0; y < y1; y++) {
		const u32 *r0 = (const u32 *)(src + 2 * y * pitch);
		const u32 *r1 = (const u32 *)(src + (2 * y + 1) * pitch);
		const u32 *d = nw_dither[y & 3];
		int base, step;

		nw_order_base(nw, order, y, &base, &step);
		x = x0;

#if IS_ENABLED(CONFIG_KERNEL_MODE_NEON)
		if (n8) {
			/* Landscape rows are contiguous; columns go via @row */
			u16 *out = step == 1 ? tx + base + x0 : row;
			u32 i;

			for (i = 0; i < 8; i++) {
				u32 t = dither ? nw_bayer4[y & 3][(x0 + i) & 3] : 0;

				drb[i] = t >> 1;
				dg[i] = t >> 2;
			}
			if ((y - y0) % NW_SIMD_ROWS == 0)
				kernel_fpu_begin();
			nw_neon_2x_row((const u8 *)(r0 + 2 * x0), (const u8 *)(r1 + 2 * x0),
				       out, n8, drb, dg);
			if ((y - y0) % NW_SIMD_ROWS == NW_SIMD_ROWS - 1 || y == y1 - 1)
				kernel_fpu_end();
			if (step != 1)
				for (i = 0; i < 8 * n8; i++)
					tx[base + (int)(x0 + i) * step] = row[i];
			x = x0 + 8 * n8;
		}
#endif

		for (; x < x1; x++) {
			u32 a = r0[2 * x], b = r0[2 * x + 1];
			u32 c = r1[2 * x], e = r1[2 * x + 1];
			u32 rb = (a & 0xff00ff) + (b & 0xff00ff) +
				 (c & 0xff00ff) + (e & 0xff00ff) + 0x20002;
			u32 g = (a & 0xff00) + (b & 0xff00) +
				(c & 0xff00) + (e & 0xff00) + 0x200;
			u32 avg = ((rb >> 2) & 0xff00ff) | ((g >> 2) & 0xff00);

			tx[base + (int)x * step] = nw_pack(avg, dither ? d[x & 3] : 0);
		}
	}
}

/*
 * Rebuild output rect [x0, x1) x [y0, y1) of @tx from the framebuffer,
 * writing pixels in the SPI order selected by @order.
 */
static void nw_build_rect(struct nw_spifb *nw, const u8 *src, u32 pitch,
			  u32 format, u16 *tx, unsigned int order, bool dither,
			  u32 x0, u32 x1, u32 y0, u32 y1)
{
	u32 x, y;
	int i;

	if (nw->taps_area && format == DRM_FORMAT_XRGB8888 &&
	    nw->vwidth == 2 * nw->width && nw->vheight == 2 * nw->height) {
		nw_build_rect_2x(nw, src, pitch, tx, order, dither, x0, x1, y0, y1);
		return;
	}

	for (i = 0; i < HROW_SLOTS; i++)
		nw->hrow_tag[i] = -1;

	for (y = y0; y < y1; y++) {
		const struct nw_taps *t = &nw->ytaps[y];
		const u32 *d = nw_dither[y & 3];
		const u32 *r[3];
		int base, step;

		/* Zero-weight taps (any position, after edge shifts) borrow a used row */
		for (i = 0; i < 3; i++)
			r[i] = t->w[i] ? nw_get_hrow(nw, src, pitch, format,
						     t->start + i, x0, x1) : NULL;
		for (i = 0; i < 3; i++)
			if (!r[i])
				r[i] = r[0] ? r[0] : r[1] ? r[1] : r[2];

		nw_order_base(nw, order, y, &base, &step);

		for (x = x0; x < x1; x++) {
			u32 c = t->w[0] == 256 ? r[0][x] :
				nw_blend3(r[0][x], r[1][x], r[2][x],
					  t->w[0], t->w[1], t->w[2]);

			tx[base + (int)x * step] = nw_pack(c, dither ? d[x & 3] : 0);
		}
	}
}

/*
 * Protocol 2: pack output rect @r from the canvas at @pk, and its header at
 * @hdr. Canvas order 1 puts output column x at panel page w-1-x, with y as
 * the panel column. Returns the packed bytes.
 */
static u32 nw_pack_rect(struct nw_spifb *nw, const struct nw_rect *r,
			u16 *pk, u16 *hdr)
{
	u32 cols = r->y1 - r->y0, p0 = nw->width - r->x1, p1 = nw->width - 1 - r->x0;
	u16 w[NW_HDR_WORDS], check = 0xffff;
	u32 p, i;

	for (p = p0; p <= p1; p++, pk += cols)
		memcpy(pk, nw->canvas + p * nw->height + r->y0, cols * 2);

	w[0] = NW_HDR_MAGIC;
	w[1] = NW_HDR_VERSION;
	w[2] = NW_MADCTL_NATIVE;
	w[3] = r->y0;
	w[4] = r->y1 - 1;
	w[5] = p0;
	w[6] = p1;
	for (i = 0; i < NW_HDR_WORDS - 1; i++)
		check ^= w[i];
	w[NW_HDR_WORDS - 1] = check;
	for (i = 0; i < NW_HDR_WORDS; i++)
		hdr[i] = cpu_to_be16(w[i]);

	return cols * (p1 - p0 + 1) * 2;
}

/*
 * The output rect a damage clip (virtual coordinates) touches, widened by
 * one output pixel for the filter footprint. False if it is empty.
 */
static bool nw_damage_to_rect(const struct nw_spifb *nw,
			      const struct drm_rect *d, struct nw_rect *r)
{
	u32 x0 = (u32)max(0, d->x1) * nw->width / nw->vwidth;
	u32 x1 = DIV_ROUND_UP((u32)max(0, d->x2) * nw->width, nw->vwidth);
	u32 y0 = (u32)max(0, d->y1) * nw->height / nw->vheight;
	u32 y1 = DIV_ROUND_UP((u32)max(0, d->y2) * nw->height, nw->vheight);

	r->x0 = x0 ? x0 - 1 : 0;
	r->y0 = y0 ? y0 - 1 : 0;
	r->x1 = min(x1 + 1, nw->width);
	r->y1 = min(y1 + 1, nw->height);
	return r->x0 < r->x1 && r->y0 < r->y1;
}

static void nw_rect_union(struct nw_rect *a, const struct nw_rect *b)
{
	a->x0 = min(a->x0, b->x0);
	a->x1 = max(a->x1, b->x1);
	a->y0 = min(a->y0, b->y0);
	a->y1 = max(a->y1, b->y1);
}

static inline u32 nw_rect_area(const struct nw_rect *r)
{
	return (r->x1 - r->x0) * (r->y1 - r->y0);
}

/*
 * Merge overlapping rects of r[0..n) until they are disjoint, so no pixel
 * goes out twice and the packed rects always fit one frame. Falls back to
 * the bounding box when more than @max remain, or when it is no bigger on
 * the wire counting NW_RECT_COST pixels per rect. Returns the rect count.
 */
static int nw_merge_rects(struct nw_rect *r, int n, int max)
{
	struct nw_rect box = r[0];
	u32 cost = 0;
	int i, j;

	for (i = 1; i < n; i++)
		nw_rect_union(&box, &r[i]);
restart:
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (r[i].x0 < r[j].x1 && r[j].x0 < r[i].x1 &&
			    r[i].y0 < r[j].y1 && r[j].y0 < r[i].y1) {
				/* The union may now overlap rects already passed */
				nw_rect_union(&r[i], &r[j]);
				r[j] = r[--n];
				goto restart;
			}

	for (i = 0; i < n; i++)
		cost += nw_rect_area(&r[i]) + NW_RECT_COST;
	if (n > max || cost >= nw_rect_area(&box) + NW_RECT_COST) {
		r[0] = box;
		return 1;
	}
	return n;
}

/*
 * Prepare a frame in the current write-side TX buffer, rebuilding only the
 * output rects r[0..n) (from nw_damage_to_rect; n = 0 rebuilds everything).
 * @r has room for at least one rect and is rewritten. Protocol 0 sends the
 * whole frame, unchanged pixels copied from the one sent last; protocol 2
 * sends just the merged rects. Returns false if there is nothing to send.
 */
static bool nw_spifb_prepare_frame(struct nw_spifb *nw,
				    const struct iosys_map *map,
				    const struct drm_framebuffer *fb,
				    struct nw_rect *r, int n)
{
	unsigned int proto = protocol == 2 ? 2 : 0;
	u16 *tx = proto ? nw->canvas : nw->tx_buf[nw->tx_write];
	const u8 *src = (const u8 *)map->vaddr + fb->offsets[0];
	u32 format = fb->format->format;
	unsigned int order = proto ? 1 : pixel_order > 2 ? 0 : pixel_order;
	bool area = area_filter;
	bool dith = dither;
	int i;

	if (format == DRM_FORMAT_ARGB8888)
		format = DRM_FORMAT_XRGB8888;
	if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_RGB565)
		return false;
	/* Only reachable mid mode switch; never read past a smaller buffer */
	if (fb->width < nw->vwidth || fb->height < nw->vheight)
		return false;

	if (area != nw->taps_area) {
		nw_build_taps(nw->xtaps, nw->width, nw->vwidth, area);
		nw_build_taps(nw->ytaps, nw->height, nw->vheight, area);
		nw->taps_area = area;
		nw->full_update = true;
	}
	if (format != nw->last_format || order != nw->last_order ||
	    dith != nw->last_dither || proto != nw->last_protocol) {
		nw->last_format = format;
		nw->last_order = order;
		nw->last_dither = dith;
		nw->last_protocol = proto;
		nw->full_update = true;
	}

	if (nw->full_update || !n) {
		r[0] = (struct nw_rect){ 0, nw->width, 0, nw->height };
		n = 1;
		nw->full_update = false;
	} else if (proto) {
		n = nw_merge_rects(r, n, NW_MAX_RECTS);
	} else {
		for (i = 1; i < n; i++)
			nw_rect_union(&r[0], &r[i]);
		n = 1;
		/* Unchanged pixels come from the frame sent last */
		if (nw_rect_area(&r[0]) < nw->width * nw->height)
			memcpy(tx, nw->tx_buf[nw->tx_write ^ 1],
			       nw->width * nw->height * 2);
	}

	for (i = 0; i < n; i++)
		nw_build_rect(nw, src, fb->pitches[0], format, tx, order, dith,
			      r[i].x0, r[i].x1, r[i].y0, r[i].y1);

	if (proto) {
		u16 *pk = nw->tx_buf[nw->tx_write];

		nw->tx_len = 0;
		for (i = 0; i < n; i++) {
			u32 len = nw_pack_rect(nw, &r[i], pk,
					       nw->tx_hdr[nw->tx_write] + i * NW_HDR_WORDS);

			nw->tx_rect_len[i] = len;
			nw->tx_len += len;
			pk += len / 2;
		}
		nw->tx_nrects = n;
		nw->tx_v2 = true;
	} else {
		nw->tx_len = nw->width * nw->height * 2;
		nw->tx_nrects = 1;
		nw->tx_v2 = false;
	}
	return true;
}

/* SPI async completion callback — runs in interrupt context */
static void nw_spifb_spi_complete(void *context)
{
	struct nw_spifb *nw = context;

	nw->tx_end = ktime_get();
	complete(&nw->tx_done);
}

/*
 * Submit the current write buffer via async SPI, then flip to the
 * other buffer for the next prepare. Waits for any in-flight transfer
 * to complete first.
 */
/* Next free transfer of the message being built, appended to it */
static struct spi_transfer *nw_xfer(struct nw_spifb *nw, int *k)
{
	struct spi_transfer *t = &nw->tx_xfer[(*k)++];

	spi_message_add_tail(t, &nw->tx_msg);
	return t;
}

static void nw_usecs(struct spi_delay *d, u32 us)
{
	d->value = us;
	d->unit = SPI_DELAY_UNIT_USECS;
}

static void nw_spifb_submit_frame(struct nw_spifb *nw)
{
	const u8 *px = nw->tx_buf[nw->tx_write];
	struct spi_transfer *t;
	ktime_t start = ktime_get();
	s64 idle, wait;
	int i, k = 0;

	/* Wait for previous async transfer to finish */
	wait_for_completion(&nw->tx_done);
	reinit_completion(&nw->tx_done);
	wait = ktime_to_ns(ktime_sub(ktime_get(), start));
	nw->stats.wait_ns += wait;
	nw->stats.wait_max_ns = max_t(u64, nw->stats.wait_max_ns, wait);

	/* The calculator re-arms on the previous frame's CS rise */
	idle = ktime_us_delta(ktime_get(), nw->tx_end);
	if (idle < NW_FRAME_GAP_US)
		udelay(NW_FRAME_GAP_US - idle);

	spi_message_init(&nw->tx_msg);
	memset(nw->tx_xfer, 0, sizeof(nw->tx_xfer));
	if (!nw->tx_v2) {
		/* One transfer for all the pixels — no chunking overhead */
		t = nw_xfer(nw, &k);
		t->tx_buf = px;
		t->len = nw->tx_len;
	}
	for (i = 0; nw->tx_v2 && i < nw->tx_nrects; i++) {
		u32 gap = min(header_gap_us, (unsigned int)NW_MAX_GAP_US);
		u32 left = gap > NW_GAP_STEP_US ? gap - NW_GAP_STEP_US : 0;

		/* Header, then CS high while the calculator programs the window */
		t = nw_xfer(nw, &k);
		t->tx_buf = nw->tx_hdr[nw->tx_write] + i * NW_HDR_WORDS;
		t->len = NW_HDR_WORDS * 2;
		t->cs_change = 1;
		nw_usecs(&t->cs_change_delay, min(gap, (u32)NW_GAP_STEP_US));
		/* The rest of the gap: empty transfers with CS left high */
		for (; left; left -= min(left, (u32)NW_GAP_STEP_US)) {
			t = nw_xfer(nw, &k);
			t->cs_off = 1;
			nw_usecs(&t->delay, min(left, (u32)NW_GAP_STEP_US));
		}

		t = nw_xfer(nw, &k);
		t->tx_buf = px;
		t->len = nw->tx_rect_len[i];
		px += t->len;
		if (i + 1 < nw->tx_nrects) {
			t->cs_change = 1;
			nw_usecs(&t->cs_change_delay, NW_FRAME_GAP_US);
		}
	}

	for (i = 0; i < k; i++)
		nw->tx_xfer[i].speed_hz = spi_hz;	/* 0: the device's max */
	nw->tx_msg.complete = nw_spifb_spi_complete;
	nw->tx_msg.context = nw;

	spi_async(nw->spi, &nw->tx_msg);

	/* Flip to the other buffer for next frame's CPU work */
	nw->tx_write ^= 1;
}

/*
 * Build and send a frame from @state: output rects r[0..n), or everything
 * when n = 0 (@r still needs room for one rect).
 */
static void nw_spifb_update(struct nw_spifb *nw, struct drm_plane_state *state,
			    struct nw_rect *r, int n)
{
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_framebuffer *fb = state->fb;
	ktime_t start;
	s64 prep;
	bool send;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;
	mutex_lock(&nw->lock);
	start = ktime_get();
	send = nw_spifb_prepare_frame(nw, &shadow->data[0], fb, r, n);
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	if (!send) {
		mutex_unlock(&nw->lock);
		return;
	}

	prep = ktime_to_ns(ktime_sub(ktime_get(), start));
	nw->stats.updates++;
	nw->stats.rects += nw->tx_nrects;
	nw->stats.bytes += nw->tx_len;
	nw->stats.prep_ns += prep;
	nw->stats.prep_max_ns = max_t(u64, nw->stats.prep_max_ns, prep);

	nw_spifb_submit_frame(nw);
	mutex_unlock(&nw->lock);
}

/*
 * The calculator drew over the panel (its own screens while the Pi app was
 * closed): resend everything from the current framebuffer. The modeset
 * locks keep that state and its mapping alive meanwhile.
 */
static void nw_spifb_repaint_work(struct work_struct *work)
{
	struct nw_spifb *nw = container_of(work, struct nw_spifb, repaint_work);
	struct drm_device *drm = &nw->drm;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_plane_state *state;
	struct nw_rect r;
	int ret, idx;

	if (!drm_dev_enter(drm, &idx))
		return;
	DRM_MODESET_LOCK_ALL_BEGIN(drm, ctx, 0, ret);
	state = nw->pipe.plane.state;
	if (nw->pipe.crtc.state && nw->pipe.crtc.state->active && state && state->fb)
		nw_spifb_update(nw, state, &r, 0);
	DRM_MODESET_LOCK_ALL_END(drm, ctx, ret);
	drm_dev_exit(idx);
}

/* The panel the repaint parameter acts on (there is only ever one) */
static struct nw_spifb *nw_device;
static DEFINE_MUTEX(nw_device_lock);

static int nw_repaint_set(const char *val, const struct kernel_param *kp)
{
	mutex_lock(&nw_device_lock);
	if (nw_device)
		schedule_work(&nw_device->repaint_work);
	mutex_unlock(&nw_device_lock);
	return 0;
}

static const struct kernel_param_ops nw_repaint_ops = {
	.set = nw_repaint_set,
};
module_param_cb(repaint, &nw_repaint_ops, NULL, 0200);
MODULE_PARM_DESC(repaint, "Write 1 to resend the whole screen (nwpid does, "
		 "when the calculator reopens the Pi app)");

/* --- DRM simple display pipe callbacks --- */

static bool nw_mode_ok(const struct nw_spifb *nw, u32 w, u32 h)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nw_mode_halves); i++)
		if (w == nw->width * nw_mode_halves[i] / 2 &&
		    h == nw->height * nw_mode_halves[i] / 2)
			return true;
	return false;
}

static enum drm_mode_status
nw_spifb_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			 const struct drm_display_mode *mode)
{
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);

	return nw_mode_ok(nw, mode->hdisplay, mode->vdisplay) ? MODE_OK : MODE_BAD;
}

static void nw_spifb_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);
	const struct drm_display_mode *mode = &crtc_state->mode;

	/* A mode switch is always a full modeset, so it lands here */
	if (mode->hdisplay != nw->vwidth || mode->vdisplay != nw->vheight) {
		nw->vwidth = mode->hdisplay;
		nw->vheight = mode->vdisplay;
		nw_build_taps(nw->xtaps, nw->width, nw->vwidth, nw->taps_area);
		nw_build_taps(nw->ytaps, nw->height, nw->vheight, nw->taps_area);
	}
	nw_spifb_update(nw, plane_state, nw->clips, 0);
}

static void nw_spifb_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);

	/* Wait for any in-flight SPI transfer (1s timeout to avoid hanging shutdown) */
	if (!wait_for_completion_timeout(&nw->tx_done, HZ))
		dev_warn(&nw->spi->dev, "SPI transfer timeout on disable\n");
	else
		complete(&nw->tx_done);	/* keep "idle" state for the next enable */
}

static void nw_spifb_pipe_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);
	struct drm_atomic_helper_damage_iter iter;
	struct nw_rect r;
	struct drm_rect clip;
	int n = 0;

	if (!pipe->crtc.state->active)
		return;

	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		if (!nw_damage_to_rect(nw, &clip, &r))
			continue;
		if (n < NW_MAX_CLIPS)
			nw->clips[n++] = r;
		else
			nw_rect_union(&nw->clips[n - 1], &r);
	}
	if (n)
		nw_spifb_update(nw, state, nw->clips, n);
}

static int nw_spifb_stats_show(struct seq_file *m, void *arg)
{
	struct drm_debugfs_entry *entry = m->private;
	struct nw_spifb *nw = drm_to_nw(entry->dev);
	u64 updates = max_t(u64, nw->stats.updates, 1);

	seq_printf(m, "mode %ux%u\nprotocol %u\nupdates %llu\nrects %llu\nbytes %llu\n",
		   nw->vwidth, nw->vheight, nw->last_protocol, nw->stats.updates,
		   nw->stats.rects, nw->stats.bytes);
	seq_printf(m, "prep_avg_us %llu\nprep_max_us %llu\nwait_avg_us %llu\nwait_max_us %llu\n",
		   div64_u64(nw->stats.prep_ns, updates * 1000),
		   div_u64(nw->stats.prep_max_ns, 1000),
		   div64_u64(nw->stats.wait_ns, updates * 1000),
		   div_u64(nw->stats.wait_max_ns, 1000));
	return 0;
}

static const struct drm_simple_display_pipe_funcs nw_spifb_pipe_funcs = {
	.mode_valid	= nw_spifb_pipe_mode_valid,
	.enable		= nw_spifb_pipe_enable,
	.disable	= nw_spifb_pipe_disable,
	.update		= nw_spifb_pipe_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

/* --- Connector --- */

static int nw_spifb_connector_get_modes(struct drm_connector *connector)
{
	struct nw_spifb *nw = drm_to_nw(connector->dev);
	struct drm_display_mode *mode;
	int i, count = 0;

	for (i = 0; i < ARRAY_SIZE(nw_mode_halves); i++) {
		u32 w = nw->width * nw_mode_halves[i] / 2;
		u32 h = nw->height * nw_mode_halves[i] / 2;

		mode = drm_mode_create(connector->dev);
		if (!mode)
			break;

		mode->type = DRM_MODE_TYPE_DRIVER;
		if (w == nw->pref_vwidth && h == nw->pref_vheight)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		mode->hdisplay = w;
		mode->vdisplay = h;

		/*
		 * Timings are meaningless for SPI — we just need valid values.
		 * SPI refresh is limited by bus speed, not pixel clock.
		 */
		mode->htotal = w + 1;
		mode->hsync_start = w + 1;
		mode->hsync_end = w + 1;
		mode->vtotal = h + 1;
		mode->vsync_start = h + 1;
		mode->vsync_end = h + 1;
		mode->clock = mode->htotal * mode->vtotal * 60 / 1000;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
		count++;
	}

	return count;
}

static const struct drm_connector_helper_funcs nw_spifb_connector_hfuncs = {
	.get_modes = nw_spifb_connector_get_modes,
};

static const struct drm_connector_funcs nw_spifb_connector_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

/* --- Mode config --- */

static const struct drm_mode_config_funcs nw_spifb_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* --- DRM driver --- */

/*
 * Allocate GEM buffers cached. Only the CPU touches them (compositor
 * writes, this driver reads and converts into tx_buf), so write-combined
 * memory buys nothing and makes every read uncached.
 */
static struct drm_gem_object *
nw_spifb_gem_create_object(struct drm_device *drm, size_t size)
{
	struct drm_gem_dma_object *dma_obj;

	dma_obj = kzalloc(sizeof(*dma_obj), GFP_KERNEL);
	if (!dma_obj)
		return ERR_PTR(-ENOMEM);

	dma_obj->map_noncoherent = true;

	return &dma_obj->base;
}

DEFINE_DRM_GEM_DMA_FOPS(nw_spifb_fops);

static const struct drm_driver nw_spifb_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &nw_spifb_fops,
	.gem_create_object	= nw_spifb_gem_create_object,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= 1,
	.minor			= 7,
};

/* --- SPI probe/remove --- */

static const u32 nw_spifb_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static int nw_spifb_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct nw_spifb *nw;
	struct drm_device *drm;
	size_t frame_size;
	int ret, i;

	nw = devm_drm_dev_alloc(dev, &nw_spifb_drm_driver,
				struct nw_spifb, drm);
	if (IS_ERR(nw))
		return PTR_ERR(nw);

	drm = &nw->drm;
	nw->spi = spi;

	/* SPI devices get no DMA mask; GEM buffers are allocated against this device */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
	}

	/* Read display properties from DT */
	if (of_property_read_u32(dev->of_node, "width", &nw->width))
		nw->width = 320;
	if (of_property_read_u32(dev->of_node, "height", &nw->height))
		nw->height = 240;
	if (of_property_read_u32(dev->of_node, "vwidth", &nw->vwidth))
		nw->vwidth = 2 * nw->width;	/* Default: 2x (640x480) */
	if (of_property_read_u32(dev->of_node, "vheight", &nw->vheight))
		nw->vheight = 2 * nw->height;

	if (nw->width > MAX_WIDTH || nw->height > MAX_HEIGHT) {
		dev_err(dev, "display %ux%u exceeds %ux%u\n",
			nw->width, nw->height, MAX_WIDTH, MAX_HEIGHT);
		return -EINVAL;
	}
	if (!nw_mode_ok(nw, nw->vwidth, nw->vheight)) {
		dev_warn(dev, "virtual %ux%u is not 1.5x, 2x, 2.5x or 3x the panel; using 2x\n",
			 nw->vwidth, nw->vheight);
		nw->vwidth = 2 * nw->width;
		nw->vheight = 2 * nw->height;
	}
	nw->pref_vwidth = nw->vwidth;
	nw->pref_vheight = nw->vheight;

	/* Filter scratch: one expanded source row (any mode), filtered rows */
	nw->src_row = devm_kzalloc(dev, NW_MAX_SCALE * nw->width * 4, GFP_KERNEL);
	if (!nw->src_row)
		return -ENOMEM;
	for (i = 0; i < HROW_SLOTS; i++) {
		nw->hrow[i] = devm_kzalloc(dev, nw->width * 4, GFP_KERNEL);
		if (!nw->hrow[i])
			return -ENOMEM;
	}

	nw_build_taps(nw->xtaps, nw->width, nw->vwidth, area_filter);
	nw_build_taps(nw->ytaps, nw->height, nw->vheight, area_filter);
	nw->taps_area = area_filter;
	nw->full_update = true;
	nw_init_dither();

	/* Allocate two TX buffers for double buffering (cached for fast CPU writes) */
	frame_size = nw->width * nw->height * 2;
	nw->tx_buf[0] = devm_kzalloc(dev, frame_size, GFP_KERNEL);
	if (!nw->tx_buf[0])
		return -ENOMEM;

	nw->tx_buf[1] = devm_kzalloc(dev, frame_size, GFP_KERNEL);
	if (!nw->tx_buf[1])
		return -ENOMEM;

	nw->tx_write = 0;

	nw->canvas = devm_kzalloc(dev, frame_size, GFP_KERNEL);
	if (!nw->canvas)
		return -ENOMEM;
	for (i = 0; i < 2; i++) {
		nw->tx_hdr[i] = devm_kzalloc(dev, NW_MAX_RECTS * NW_HDR_WORDS * 2,
					     GFP_KERNEL);
		if (!nw->tx_hdr[i])
			return -ENOMEM;
	}

	mutex_init(&nw->lock);
	INIT_WORK(&nw->repaint_work, nw_spifb_repaint_work);

	/* Start with completion signaled (no transfer in flight) */
	init_completion(&nw->tx_done);
	complete(&nw->tx_done);

	/* DRM mode config */
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.funcs = &nw_spifb_mode_config_funcs;
	drm->mode_config.min_width = nw->width * nw_mode_halves[0] / 2;
	drm->mode_config.max_width = nw->width * NW_MAX_SCALE;
	drm->mode_config.min_height = nw->height * nw_mode_halves[0] / 2;
	drm->mode_config.max_height = nw->height * NW_MAX_SCALE;

	/* Connector */
	ret = drm_connector_init(drm, &nw->connector,
				 &nw_spifb_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	drm_connector_helper_add(&nw->connector, &nw_spifb_connector_hfuncs);

	/* Simple display pipe (CRTC + encoder + plane) */
	ret = drm_simple_display_pipe_init(drm, &nw->pipe,
					   &nw_spifb_pipe_funcs,
					   nw_spifb_formats,
					   ARRAY_SIZE(nw_spifb_formats),
					   NULL, /* no format modifiers */
					   &nw->connector);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&nw->pipe.plane);

	drm_mode_config_reset(drm);

	drm_debugfs_add_file(drm, "spifb_stats", nw_spifb_stats_show, NULL);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	mutex_lock(&nw_device_lock);
	nw_device = nw;
	mutex_unlock(&nw_device_lock);

	/* fbdev emulation — provides /dev/fb0 for legacy console/apps */
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);

	dev_info(dev, "NumWorks SPI display: %ux%u (virtual %ux%u, %s filter) @ SPI max %u Hz\n",
		 nw->width, nw->height, nw->vwidth, nw->vheight,
		 area_filter ? "area" : "nearest", spi->max_speed_hz);

	return 0;
}

static void nw_spifb_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);
	struct nw_spifb *nw = drm_to_nw(drm);

	mutex_lock(&nw_device_lock);
	nw_device = NULL;
	mutex_unlock(&nw_device_lock);
	cancel_work_sync(&nw->repaint_work);

	drm_dev_unplug(drm);

	/* Wait for any in-flight SPI transfer (1s timeout) */
	wait_for_completion_timeout(&nw->tx_done, HZ);
}

static void nw_spifb_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static const struct of_device_id nw_spifb_of_match[] = {
	{ .compatible = "numworks,spifb" },
	{ }
};
MODULE_DEVICE_TABLE(of, nw_spifb_of_match);

static const struct spi_device_id nw_spifb_id[] = {
	{ "spifb", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, nw_spifb_id);

static struct spi_driver nw_spifb_spi_driver = {
	.driver = {
		.name		= DRIVER_NAME,
		.of_match_table	= nw_spifb_of_match,
	},
	.id_table	= nw_spifb_id,
	.probe		= nw_spifb_probe,
	.remove		= nw_spifb_remove,
	.shutdown	= nw_spifb_shutdown,
};
module_spi_driver(nw_spifb_spi_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Martin");
MODULE_LICENSE("GPL");
