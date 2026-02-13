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
 * Uses async SPI with double buffering: CPU scales frame N+1 into one
 * buffer while SPI DMA sends frame N from the other. This overlaps
 * CPU and SPI work, achieving the SPI bus speed limit (~50 FPS).
 *
 * Inspired by zardam's SPI display concept.
 * Based on drivers/gpu/drm/tiny/repaper.c skeleton pattern.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
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
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define DRIVER_NAME	"drm-spifb"
#define DRIVER_DESC	"NumWorks SPI framebuffer display"

/*
 * SPI frame size. The BCM2835 DMA engine supports transfers up to 1GB,
 * so we send the entire frame in a single transfer to minimize overhead.
 * Previous 32KB chunking added ~2ms DMA setup overhead per frame.
 */
#define FRAME_SIZE	(320 * 240 * 2)	/* 153,600 bytes RGB565 */
#define MAX_SPI_XFERS	1

struct nw_spifb {
	struct drm_device drm;
	struct spi_device *spi;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	u32 width;		/* Physical SPI display width (320) */
	u32 height;		/* Physical SPI display height (240) */
	u32 vwidth;		/* Virtual (compositor) width */
	u32 vheight;		/* Virtual (compositor) height */

	/* Double-buffered async SPI */
	void *tx_buf[2];
	int tx_write;			/* Buffer index CPU writes to next */
	struct spi_message tx_msg;
	struct spi_transfer tx_xfers[MAX_SPI_XFERS];
	struct completion tx_done;	/* Signals SPI transfer complete */
};

static inline struct nw_spifb *drm_to_nw(struct drm_device *drm)
{
	return container_of(drm, struct nw_spifb, drm);
}

/*
 * Downscale XRGB8888 framebuffer to big-endian RGB565 with nearest-neighbor.
 * Source is vwidth x vheight XRGB8888, output is width x height RGB565 BE.
 */
static void nw_spifb_scale_xrgb8888(struct nw_spifb *nw,
				     const struct iosys_map *src,
				     const struct drm_framebuffer *fb,
				     u16 *tx)
{
	u32 src_pitch = fb->pitches[0];
	const u8 *src_base = src->vaddr;
	u32 w = nw->width, h = nw->height;
	u32 vw = nw->vwidth, vh = nw->vheight;
	u32 x, y;

	for (y = 0; y < h; y++) {
		u32 sy = y * vh / h;
		const u32 *src_row = (const u32 *)(src_base + sy * src_pitch);
		for (x = 0; x < w; x++) {
			u32 sx = x * vw / w;
			u32 pix = src_row[sx];
			u16 r = (pix >> 16) & 0xff;
			u16 g = (pix >> 8) & 0xff;
			u16 b = pix & 0xff;
			u16 rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			tx[y * w + x] = cpu_to_be16(rgb565);
		}
	}
}

/*
 * Downscale RGB565 framebuffer to big-endian RGB565 with nearest-neighbor.
 * Source is vwidth x vheight RGB565, output is width x height RGB565 BE.
 */
static void nw_spifb_scale_rgb565(struct nw_spifb *nw,
				   const struct iosys_map *src,
				   const struct drm_framebuffer *fb,
				   u16 *tx)
{
	u32 src_pitch = fb->pitches[0];
	const u8 *src_base = src->vaddr;
	u32 w = nw->width, h = nw->height;
	u32 vw = nw->vwidth, vh = nw->vheight;
	u32 x, y;

	for (y = 0; y < h; y++) {
		u32 sy = y * vh / h;
		const u16 *src_row = (const u16 *)(src_base + sy * src_pitch);
		for (x = 0; x < w; x++) {
			u32 sx = x * vw / w;
			tx[y * w + x] = cpu_to_be16(src_row[sx]);
		}
	}
}

/*
 * Prepare a frame: scale/convert the compositor framebuffer into the
 * current write-side TX buffer. CPU work only, no SPI.
 */
static void nw_spifb_prepare_frame(struct nw_spifb *nw,
				    const struct iosys_map *src,
				    const struct drm_framebuffer *fb,
				    struct drm_format_conv_state *fmtcnv_state)
{
	u16 *tx = nw->tx_buf[nw->tx_write];

	if (nw->vwidth != nw->width || nw->vheight != nw->height) {
		/* Scaled mode: downscale + format convert */
		switch (fb->format->format) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			nw_spifb_scale_xrgb8888(nw, src, fb, tx);
			break;
		case DRM_FORMAT_RGB565:
			nw_spifb_scale_rgb565(nw, src, fb, tx);
			break;
		default:
			return;
		}
	} else {
		/* 1:1 mode: use DRM format helpers */
		struct iosys_map dst;
		struct drm_rect clip = DRM_RECT_INIT(0, 0, nw->width, nw->height);

		iosys_map_set_vaddr(&dst, tx);

		switch (fb->format->format) {
		case DRM_FORMAT_RGB565:
			drm_fb_swab(&dst, NULL, src, fb, &clip, false,
				    fmtcnv_state);
			break;
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			drm_fb_xrgb8888_to_rgb565(&dst, NULL, src, fb, &clip,
						  fmtcnv_state, true);
			break;
		default:
			return;
		}
	}
}

/* SPI async completion callback — runs in interrupt context */
static void nw_spifb_spi_complete(void *context)
{
	struct nw_spifb *nw = context;

	complete(&nw->tx_done);
}

/*
 * Submit the current write buffer via async SPI, then flip to the
 * other buffer for the next prepare. Waits for any in-flight transfer
 * to complete first.
 */
static void nw_spifb_submit_frame(struct nw_spifb *nw)
{
	size_t frame_size = nw->width * nw->height * 2; /* RGB565 output */
	void *buf = nw->tx_buf[nw->tx_write];

	/* Wait for previous async transfer to finish */
	wait_for_completion(&nw->tx_done);
	reinit_completion(&nw->tx_done);

	/* Single SPI transfer for entire frame — no chunking overhead */
	spi_message_init(&nw->tx_msg);
	memset(&nw->tx_xfers[0], 0, sizeof(nw->tx_xfers[0]));

	nw->tx_xfers[0].tx_buf = buf;
	nw->tx_xfers[0].len = frame_size;
	spi_message_add_tail(&nw->tx_xfers[0], &nw->tx_msg);

	nw->tx_msg.complete = nw_spifb_spi_complete;
	nw->tx_msg.context = nw;

	spi_async(nw->spi, &nw->tx_msg);

	/* Flip to the other buffer for next frame's CPU work */
	nw->tx_write ^= 1;
}

/* --- DRM simple display pipe callbacks --- */

static enum drm_mode_status
nw_spifb_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			 const struct drm_display_mode *mode)
{
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);

	if (mode->hdisplay != nw->vwidth || mode->vdisplay != nw->vheight)
		return MODE_BAD;

	return MODE_OK;
}

static void nw_spifb_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);
	struct drm_shadow_plane_state *shadow =
		to_drm_shadow_plane_state(plane_state);

	/* Send initial frame */
	nw_spifb_prepare_frame(nw, &shadow->data[0], plane_state->fb,
			       &shadow->fmtcnv_state);
	nw_spifb_submit_frame(nw);
}

static void nw_spifb_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);

	/* Wait for any in-flight SPI transfer (1s timeout to avoid hanging shutdown) */
	if (!wait_for_completion_timeout(&nw->tx_done, HZ))
		dev_warn(&nw->spi->dev, "SPI transfer timeout on disable\n");
}

static void nw_spifb_pipe_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow =
		to_drm_shadow_plane_state(state);
	struct drm_rect rect;
	struct nw_spifb *nw = drm_to_nw(pipe->crtc.dev);

	if (!pipe->crtc.state->active)
		return;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect)) {
		/*
		 * We always send the full frame because the STM32 SPI slave
		 * has no partial update mechanism — it expects a complete
		 * 320x240 frame per CS assertion.
		 */
		nw_spifb_prepare_frame(nw, &shadow->data[0], state->fb,
				       &shadow->fmtcnv_state);
		nw_spifb_submit_frame(nw);
	}
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

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->hdisplay = nw->vwidth;
	mode->vdisplay = nw->vheight;

	/*
	 * Timings are meaningless for SPI — we just need valid values.
	 * SPI refresh is limited by bus speed, not pixel clock.
	 */
	mode->htotal = nw->vwidth + 1;
	mode->hsync_start = nw->vwidth + 1;
	mode->hsync_end = nw->vwidth + 1;
	mode->vtotal = nw->vheight + 1;
	mode->vsync_start = nw->vheight + 1;
	mode->vsync_end = nw->vheight + 1;
	mode->clock = mode->htotal * mode->vtotal * 60 / 1000;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
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

DEFINE_DRM_GEM_DMA_FOPS(nw_spifb_fops);

static const struct drm_driver nw_spifb_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &nw_spifb_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= 1,
	.minor			= 2,
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
	int ret;

	nw = devm_drm_dev_alloc(dev, &nw_spifb_drm_driver,
				struct nw_spifb, drm);
	if (IS_ERR(nw))
		return PTR_ERR(nw);

	drm = &nw->drm;
	nw->spi = spi;

	/* Read display properties from DT */
	if (of_property_read_u32(dev->of_node, "width", &nw->width))
		nw->width = 320;
	if (of_property_read_u32(dev->of_node, "height", &nw->height))
		nw->height = 240;
	if (of_property_read_u32(dev->of_node, "vwidth", &nw->vwidth))
		nw->vwidth = 480;	/* Default: 1.5x (480x360) */
	if (of_property_read_u32(dev->of_node, "vheight", &nw->vheight))
		nw->vheight = 360;
	if (nw->vwidth < nw->width)
		nw->vwidth = nw->width;
	if (nw->vheight < nw->height)
		nw->vheight = nw->height;

	/* Allocate two TX buffers for double buffering (cached for fast CPU writes) */
	nw->tx_buf[0] = devm_kzalloc(dev, nw->width * nw->height * 2,
				      GFP_KERNEL);
	if (!nw->tx_buf[0])
		return -ENOMEM;

	nw->tx_buf[1] = devm_kzalloc(dev, nw->width * nw->height * 2,
				      GFP_KERNEL);
	if (!nw->tx_buf[1])
		return -ENOMEM;

	nw->tx_write = 0;

	/* Start with completion signaled (no transfer in flight) */
	init_completion(&nw->tx_done);
	complete(&nw->tx_done);

	/* DRM mode config */
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.funcs = &nw_spifb_mode_config_funcs;
	drm->mode_config.min_width = nw->vwidth;
	drm->mode_config.max_width = nw->vwidth;
	drm->mode_config.min_height = nw->vheight;
	drm->mode_config.max_height = nw->vheight;

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

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	/* fbdev emulation — provides /dev/fb0 for legacy console/apps */
	drm_fbdev_dma_setup(drm, 16);

	dev_info(dev, "NumWorks SPI display: %ux%u (virtual %ux%u) @ SPI max %u Hz\n",
		 nw->width, nw->height, nw->vwidth, nw->vheight,
		 spi->max_speed_hz);

	return 0;
}

static void nw_spifb_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);
	struct nw_spifb *nw = drm_to_nw(drm);

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
