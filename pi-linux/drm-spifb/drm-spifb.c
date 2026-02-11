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
 * Inspired by zardam's SPI display concept.
 * Based on drivers/gpu/drm/tiny/repaper.c skeleton pattern.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
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
 * SPI transfer chunk size. BCM2835 DMA has a per-transfer limit.
 * 32768 bytes per chunk, matching the BCM2835 DMA limit.
 */
#define SPI_CHUNK_SIZE	32768

struct nw_spifb {
	struct drm_device drm;
	struct spi_device *spi;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	u32 width;		/* Physical SPI display width (320) */
	u32 height;		/* Physical SPI display height (240) */
	u32 vwidth;		/* Virtual (compositor) width */
	u32 vheight;		/* Virtual (compositor) height */

	/* DMA-coherent buffer for SPI transfers (byte-swapped copy) */
	void *tx_buf;
	dma_addr_t tx_dma;
	size_t tx_size;
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
				     const struct drm_framebuffer *fb)
{
	u16 *tx = nw->tx_buf;
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
				   const struct drm_framebuffer *fb)
{
	u16 *tx = nw->tx_buf;
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
 * Convert framebuffer data to big-endian RGB565 in the TX buffer,
 * then send over SPI in 32KB chunks.
 *
 * The STM32 SPI slave expects raw big-endian RGB565 pixels.
 * The compositor renders at (width*scale)x(height*scale) and we
 * downscale to width x height before sending.
 */
static void nw_spifb_send_frame(struct nw_spifb *nw,
				const struct iosys_map *src,
				const struct drm_framebuffer *fb,
				struct drm_format_conv_state *fmtcnv_state)
{
	size_t frame_size = nw->width * nw->height * 2; /* RGB565 output */
	struct spi_message msg;
	struct spi_transfer xfers[5];
	size_t remaining, offset;
	int n_xfers;

	if (nw->vwidth != nw->width || nw->vheight != nw->height) {
		/* Scaled mode: downscale + format convert */
		switch (fb->format->format) {
		case DRM_FORMAT_XRGB8888:
			nw_spifb_scale_xrgb8888(nw, src, fb);
			break;
		case DRM_FORMAT_RGB565:
			nw_spifb_scale_rgb565(nw, src, fb);
			break;
		default:
			return;
		}
	} else {
		/* 1:1 mode: use DRM format helpers */
		struct iosys_map dst;
		struct drm_rect clip = DRM_RECT_INIT(0, 0, nw->width, nw->height);

		iosys_map_set_vaddr(&dst, nw->tx_buf);

		switch (fb->format->format) {
		case DRM_FORMAT_RGB565:
			drm_fb_swab(&dst, NULL, src, fb, &clip, false,
				    fmtcnv_state);
			break;
		case DRM_FORMAT_XRGB8888:
			drm_fb_xrgb8888_to_rgb565(&dst, NULL, src, fb, &clip,
						  fmtcnv_state, true);
			break;
		default:
			return;
		}
	}

	/* Split into chunks for SPI DMA */
	spi_message_init(&msg);
	memset(xfers, 0, sizeof(xfers));

	remaining = frame_size;
	offset = 0;
	n_xfers = 0;

	while (remaining > 0 && n_xfers < ARRAY_SIZE(xfers)) {
		size_t len = min_t(size_t, remaining, SPI_CHUNK_SIZE);

		xfers[n_xfers].tx_buf = (u8 *)nw->tx_buf + offset;
		xfers[n_xfers].len = len;
		spi_message_add_tail(&xfers[n_xfers], &msg);

		offset += len;
		remaining -= len;
		n_xfers++;
	}

	spi_sync(nw->spi, &msg);
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
	nw_spifb_send_frame(nw, &shadow->data[0], plane_state->fb,
			    &shadow->fmtcnv_state);
}

static void nw_spifb_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	/* Nothing to do — the STM32 handles display power */
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
		nw_spifb_send_frame(nw, &shadow->data[0], state->fb,
				    &shadow->fmtcnv_state);
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
	.minor			= 0,
};

/* --- SPI probe/remove --- */

static const u32 nw_spifb_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888, /* KMS will convert to RGB565 for us */
};

static int nw_spifb_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct nw_spifb *nw;
	struct drm_device *drm;
	size_t buf_size;
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

	/* Allocate DMA-coherent TX buffer (sized for physical display) */
	buf_size = PAGE_ALIGN(nw->width * nw->height * 2);
	nw->tx_buf = dma_alloc_coherent(dev, buf_size, &nw->tx_dma, GFP_KERNEL);
	if (!nw->tx_buf)
		return -ENOMEM;
	nw->tx_size = buf_size;

	/* DRM mode config */
	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_dma;

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
		goto err_dma;

	drm_connector_helper_add(&nw->connector, &nw_spifb_connector_hfuncs);

	/* Simple display pipe (CRTC + encoder + plane) */
	ret = drm_simple_display_pipe_init(drm, &nw->pipe,
					   &nw_spifb_pipe_funcs,
					   nw_spifb_formats,
					   ARRAY_SIZE(nw_spifb_formats),
					   NULL, /* no format modifiers */
					   &nw->connector);
	if (ret)
		goto err_dma;

	drm_plane_enable_fb_damage_clips(&nw->pipe.plane);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_dma;

	spi_set_drvdata(spi, drm);

	/* fbdev emulation — provides /dev/fb0 for legacy console/apps */
	drm_fbdev_dma_setup(drm, 16);

	dev_info(dev, "NumWorks SPI display: %ux%u (virtual %ux%u) @ SPI max %u Hz\n",
		 nw->width, nw->height, nw->vwidth, nw->vheight,
		 spi->max_speed_hz);

	return 0;

err_dma:
	dma_free_coherent(dev, nw->tx_size, nw->tx_buf, nw->tx_dma);
	return ret;
}

static void nw_spifb_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);
	struct nw_spifb *nw = drm_to_nw(drm);

	drm_dev_unplug(drm);
	dma_free_coherent(&spi->dev, nw->tx_size, nw->tx_buf, nw->tx_dma);
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
