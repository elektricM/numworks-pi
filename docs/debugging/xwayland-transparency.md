# XWayland Transparency on SPI Display — Debug Notes

## Problem
X11 apps running under XWayland render as fully transparent windows on the
custom SPI display (drm-spifb). Only the window decorations (title bar,
shadow) are visible — the window contents are invisible.

## Hardware
- Raspberry Pi Zero 2W
- Custom SPI display (320x240, DRM driver `drm-spifb` at /dev/dri/card1)
- vc4 GPU (Broadcom VideoCore IV, /dev/dri/card0, render node /dev/dri/renderD128)
- Display stack: labwc (wlroots 0.19 compositor) → XWayland 24.1.6 → X11 apps
- XWayland version: 2:24.1.6-1+rpt1 (RPi OS Trixie)
- wlroots: 0.19.1-1+rpt2
- Mesa: 25.0.7-2+rpt3

## Root Cause

**The compositor (labwc/wlroots) was advertising the wrong DRM device
(drm-spifb/card1) as the primary render device instead of the GPU (vc4/card0).**

By default, wlroots picks the display output device as the primary DRM device.
On our dual-GPU setup:
- card0 = vc4 (GPU, render node renderD128) — the actual GPU
- card1 = drm-spifb (SPI display, no render node) — just a dumb framebuffer

wlroots picked card1 as primary because it's the display device. This caused
a cascade of failures:

### Failure chain

1. **Compositor advertises card1 via linux-dmabuf and wl_drm protocols**
   - `wayland-info` showed: `main device: 0xE201 (/dev/dri/card1)`

2. **XWayland initializes GBM on card1 (drm-spifb)**
   - `xwl_drm_handle_device()` receives card1 from wl_drm
   - `gbm_create_device(card1_fd)` creates GBM on the wrong device
   - DRI3 Open returns card1 fd to clients

3. **GBM BO creation with explicit modifiers fails on card1**
   - `gbm_bo_create_with_modifiers2()` returns NULL (drm-spifb has no modifier support)
   - Falls to implicit `gbm_bo_create()` → `implicit_modifier=TRUE`

4. **Implicit modifier triggers wl_drm legacy path**
   - `init_buffer_params_fallback()` sets `modifier = DRM_FORMAT_MOD_INVALID`
   - `xwl_glamor_is_modifier_supported(XRGB8888, INVALID)` → FALSE
   - Falls to `wl_drm_create_prime_buffer()` (legacy)

5. **wlroots 0.19 can't import wl_drm buffers across devices**
   - `wlr_egl_create_image_from_dmabuf()` fails
   - Result: "Failed to create texture" / "Failed to upload buffer"
   - Windows render transparent (no content texture)

6. **DRI3 clients (GL apps) got the wrong GPU entirely**
   - Mesa loader: `using driver drm-spifb for 4` (fd 4 = card1)
   - No GL acceleration on drm-spifb → client DMA-BUFs are wrong device
   - Even with dmabuf path forced, GL content was BLACK

### Compositor error log
```
[ERROR] [types/buffer/client.c:109] Failed to create texture
[ERROR] [types/wlr_compositor.c:454] Failed to upload buffer
```

## The Fix

**Set `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128` in the labwc environment.**

This overrides only the render device used for DRI3 and linux-dmabuf, while
keeping card1 (drm-spifb) as the sole primary output. The compositor then:
1. Uses card1 as the only output device (fast direct scanout path)
2. Advertises vc4/renderD128 as the render device for linux-dmabuf and DRI3
3. XWayland's GBM uses vc4 → proper DMA-BUF sharing
4. DRI3 clients get the vc4 render node → hardware GL works

### Why not WLR_DRM_DEVICES?

The earlier fix used `WLR_DRM_DEVICES=/dev/dri/card0:/dev/dri/card1` to force
vc4 as primary. This fixed XWayland but caused compositor lag: wlroots treated
the setup as multi-GPU, copying frames between devices on every output commit.
`WLR_RENDER_DRM_DEVICE` avoids this by only overriding the render device
advertisement, not the output device selection.

### Configuration
In `~/.config/labwc/environment`:
```
WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
```

Then restart: `sudo systemctl restart lightdm`

### Verification
```bash
# Compositor advertises vc4:
wayland-info | grep 'main device'
# Should show: main device: 0xE200 (/dev/dri/card0 or /dev/dri/renderD128)

# DRI3 gives clients vc4:
DISPLAY=:0 LIBGL_DEBUG=verbose glxgears 2>&1 | grep 'driver'
# Should show: using driver vc4 for 4

# Hardware GL works:
DISPLAY=:0 glxinfo | grep 'renderer'
# Should show: OpenGL renderer string: VC4 V3D 2.1

# Camera EGL preview works (no --qt-preview needed):
rpicam-hello
```

### What this replaces
- **No `XWAYLAND_NO_GLAMOR=1` needed** — glamor DMA-BUF sharing works properly
- **No XWayland patches needed** — stock XWayland works
- **No `--qt-preview` flag needed** — native EGL camera preview works
- **Hardware GPU rendering** — vc4 DRI3 direct rendering, not llvmpipe software

## Investigation History

### Phase 1: Wrong theories
- **GLES/ARGB8888 theory** — Hypothesized vc4 GLES forces ARGB8888 format causing
  alpha transparency. Debug revealed vc4 uses desktop GL 2.1 (not GLES), format is
  XRGB8888 (opaque). Alpha was never the issue.
- **Missing opaque region** — Patched XWayland to call `wl_surface_set_opaque_region()`.
  No effect: it's just a compositor hint, doesn't fix buffer import failure.
- **ARGB8888 in drm-spifb driver** — Added format to driver. No effect: compositor
  blends before sending to display driver.

### Phase 2: Identified buffer import failure
- Added debug logging to XWayland's `xwayland-glamor-gbm.c`
- Discovered the wl_drm fallback path and "Failed to create texture" errors
- Identified the implicit modifier → wl_drm → failed import chain

### Phase 3: Tried forcing linux-dmabuf path
- **Approach A**: Override modifier from INVALID to LINEAR in fallback path
  → dmabuf import succeeds, but window content BLACK
- **Approach B**: Always use per-plane init (`init_buffer_params_with_modifiers`)
  → Same: dmabuf works, no errors, but BLACK content
- **glFinish() patch**: Changed `glamor_flush()` from `glFlush()` to `glFinish()`
  to force GPU sync → Still BLACK

### Phase 4: Narrowed down to GL-specific issue
- **xeyes (non-GL X11)**: Content visible with dmabuf patch → DMA-BUF export works
- **glxgears (GL/DRI3)**: Still BLACK → GL-specific problem
- **glxgears with LIBGL_ALWAYS_SOFTWARE=1**: Visible → software rendering works
- **glxgears with LIBGL_DRI3_DISABLE=1**: Visible → falls to llvmpipe

### Phase 5: Found the real root cause
- Checked Mesa loader: `using driver drm-spifb for 4` — **wrong GPU!**
- DRI3 Open was giving clients the drm-spifb device fd
- `wayland-info`: compositor advertised `main device: 0xE201 (/dev/dri/card1)`
- Root cause: wlroots picked drm-spifb as primary because it's the display device
- Initial fix: `WLR_DRM_DEVICES=/dev/dri/card0:/dev/dri/card1` → vc4 as primary
- Better fix: `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128` → avoids multi-GPU lag
- Result: All apps work, GL rendering works, camera EGL works, no patches needed

### Why the dmabuf-forced approach showed BLACK for GL apps
Even with the dmabuf path forced (modifier override to LINEAR), GL apps showed
black because:
1. GBM was initialized on card1 (drm-spifb), not vc4
2. EGL/glamor somehow used vc4 internally (via Mesa's GPU selection)
3. GBM BOs were allocated on card1, but GL rendered on vc4
4. DMA-BUF exported from card1 BO contained no rendered data (wrong device memory)
5. Non-GL apps (xeyes) worked because their content was CPU-rendered (no GPU mismatch)

## Key Debugging Commands
```bash
# Check which device compositor advertises:
wayland-info | grep -A5 'linux-dmabuf'

# Check which GPU Mesa uses for DRI3:
DISPLAY=:0 LIBGL_DEBUG=verbose glxgears 2>&1 | grep 'driver'

# Check GL renderer:
DISPLAY=:0 glxinfo | grep 'renderer\|direct'

# Check XWayland's open devices:
ls -la /proc/$(pgrep -x Xwayland)/fd/ | grep dri

# Check DRM devices:
ls -la /dev/dri/
for card in /dev/dri/card*; do
    echo "$card: $(udevadm info -q property $card | grep DEVPATH)"
done
```

## Key Source Files
- XWayland glamor GBM: `hw/xwayland/xwayland-glamor-gbm.c`
  - `xwl_drm_handle_device()` — receives wl_drm device, opens GBM fd (line ~1352)
  - `xwl_glamor_gbm_init_main_dev()` — linux-dmabuf feedback device (line ~1704)
  - `xwl_glamor_gbm_init_egl()` — EGL init on GBM device (line ~1750)
  - `xwl_dri3_open_client()` — DRI3 Open gives fd to clients (line ~744)
  - `gbm_format_for_depth()` — format selection (line ~127)
  - `xwl_glamor_gbm_create_pixmap_internal()` — BO creation (line ~340)
  - `xwl_glamor_pixmap_get_wl_buffer()` — buffer creation (line ~558)
- XWayland window: `hw/xwayland/xwayland-window.c`
  - `xwl_window_attach_buffer()` — buffer attach (line ~1991)
  - `xwl_screen_post_damage()` — damage + commit flow (xwayland-screen.c:433)
- XWayland Present: `hw/xwayland/xwayland-present.c`
  - `xwl_present_flip()` — DRI3 present flip (line ~862)
  - `xwl_present_check_flip()` — flip eligibility check (line ~732)
- labwc env user: `~/.config/labwc/environment` (WLR_RENDER_DRM_DEVICE)
- labwc env system: `/etc/xdg/labwc/environment`
- DRM driver: `pi-linux/drm-spifb/drm-spifb.c`

