# Pi Setup — Raspbian Trixie

## OS

- **Raspbian Trixie 13**, 32-bit Desktop
- **Kernel tested:** `6.12.62+rpt-rpi-v7`

## Compositor

Trixie uses **Wayland (labwc)** by default, not X11. This affects keyboard handling, mouse emulation, and display output.

## Display Driver — DRM/KMS

The display uses a **DRM/KMS** driver (`drm-spifb`), replacing zardam's fbdev pipeline (`spifb` + `fbcp`).

Why the old approach no longer works:
- `spi_busnum_to_master()` was removed in kernel 5.18, breaking the old `spifb` module.
- `dispmanx` was removed in Bookworm.
- `fbdev` is deprecated upstream.

### Virtual Resolution

The DRM driver advertises **480x360** to the compositor and downscales to **320x240** for SPI output. Both dimensions are configurable via `dtoverlay` parameters.

## Keyboard Layout

labwc ignores the system keyboard setting. You must set US QWERTY in **both** locations:

1. `/etc/default/keyboard`:
   ```
   XKBLAYOUT="us"
   XKBVARIANT=""
   ```

2. `~/.config/labwc/environment`:
   ```
   XKB_DEFAULT_LAYOUT=us
   ```

## Arrow Keys

The daemon was patched to emit `KEY_LEFT`, `KEY_UP`, `KEY_DOWN`, `KEY_RIGHT` instead of numpad arrow keys. Under Wayland, numpad keys were interpreted as digits rather than directional input.

## Mouse Mode

The daemon emits `EV_REL` events directly through the input device. This replaces the X11-era approach of using MouseKeys with NUMLOCK toggling, which does not work under Wayland.
