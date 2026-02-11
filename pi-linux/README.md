# numworks-pi-modern

Port of the NumWorks Pi integration to modern Raspberry Pi OS (Trixie / kernel 6.x).

## Background

The NumWorks calculator can act as a display and keyboard for a Raspberry Pi Zero 2 W. The STM32F412 on the calculator receives raw RGB565 pixels over SPI and DMA-transfers them directly to the LCD. A separate UART link sends keyboard state back to the Pi. This project provides the Linux-side drivers to make that work on current kernels.

## What Changed From the Original

The original Pi-side software (zardam, 2018) has two blockers on modern kernels:

1. **spifb** calls `spi_busnum_to_master()` — removed in kernel 5.18. Modern kernels require Device Tree for SPI device instantiation.
2. **fbcp** calls Broadcom `dispmanx` — removed in Bookworm. The proprietary GPU capture API is gone, replaced by DRM/KMS.

Both are replaced by a single DRM/KMS driver (`drm-spifb`) that lets the GPU composite directly to the SPI display — no intermediate copies, no fbcp.

| Old (zardam 2018) | New | What Changed |
|---|---|---|
| spifb (fbdev) + rpi-fbcp (dispmanx) | `drm-spifb` | Single DRM driver replaces both. KMS renders directly to SPI. |
| `spi_busnum_to_master()` hardcoded | Device Tree overlay | Standard kernel device instantiation. |
| Global variables, platform_driver | Per-device state, spi_driver | Proper driver model. |
| uinput-serial-keyboard (X11 MouseKeys) | uinput-serial-keyboard (native EV_REL) | Patched for Wayland: direct mouse events, continuous movement via poll timer. |

## SPI Protocol

The wire protocol is identical to the original — compatible with unmodified calculator firmware:

- **Bus**: SPI0, chip select 0, mode 0
- **Speed**: 70 MHz requested → BCM2835 rounds down to 62.5 MHz (250 MHz / 4)
- **Frame**: 320 x 240 x 2 bytes = 153,600 bytes of big-endian RGB565
- **Chunking**: 5 SPI transfers of 32,768 bytes each (BCM2835 DMA limit), last transfer 22,528 bytes
- **Byte order**: `cpu_to_be16()` — STM32 SPI is configured for 16-bit MSB-first
- **Timing**: CS assert → ~10 µs gap → clock starts. STM32 uses EXTI interrupt on CS to set up the LCD window (~3 µs), so the 10 µs gap provides margin.

No init commands, no register writes, no MIPI DBI protocol. Just raw pixels. The STM32 handles all LCD controller commands via FSMC.

## Directory Structure

```
drm-spifb/                 DRM/KMS tiny driver
  drm-spifb.c              drm_simple_display_pipe SPI driver (~480 lines)
  Makefile                 Kernel module build
overlay/
  numworks-spifb.dts       Device Tree overlay for SPI0/CE0 (with vwidth/vheight params)
uinput-serial-keyboard/
  uinput.c                 Keyboard/mouse daemon (patched for Wayland)
config/                    Pi config file copies (for reference/restore)
  config.txt               /boot/firmware/config.txt
  cmdline.txt              /boot/firmware/cmdline.txt
  nwinput.service          systemd service for keyboard daemon
  keyboard                 /etc/default/keyboard (US QWERTY)
  labwc-environment        Wayland compositor keyboard layout
  modules-load.d-numworks.conf  /etc/modules-load.d/numworks.conf
scripts/
  build.sh                 Build the driver
  install-overlay.sh       Compile and install the DT overlay
docs/
  architecture.md          Technical details and design decisions
archive/                   Old fallback drivers (fbdev + fbcp-drm), unused
```

## Quick Start

```bash
# On the Pi (Trixie):

# 1. Install the DT overlay
sudo scripts/install-overlay.sh

# 2. Build and install the DRM driver
cd drm-spifb
make
sudo cp drm-spifb.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a

# 3. Build and install the keyboard daemon
cd ../uinput-serial-keyboard
gcc uinput.c -o nwinput
sudo cp nwinput /usr/local/bin/
sudo cp ../config/nwinput.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable nwinput

# 4. Configure
#   Add to /boot/firmware/config.txt:
#     dtparam=spi=on
#     dtoverlay=numworks-spifb
#   Remove console=serial0,115200 from /boot/firmware/cmdline.txt
#   Load required modules:
#     sudo cp config/modules-load.d-numworks.conf /etc/modules-load.d/numworks.conf
#   Set keyboard layout to US in ~/.config/labwc/environment:
#     XKB_DEFAULT_LAYOUT=us

# 5. Reboot — display + keyboard working
sudo reboot
```

## Virtual Resolution

The driver renders at a configurable virtual resolution and downscales to the physical 320x240 with nearest-neighbor. Default is 480x360 (1.5x) — a good balance between readability and screen real estate.

Configure in `/boot/firmware/config.txt`:
```
dtoverlay=numworks-spifb                        # default: 480x360 (1.5x)
dtoverlay=numworks-spifb,vwidth=640,vheight=480 # 2x — more content, smaller text
dtoverlay=numworks-spifb,vwidth=320,vheight=240 # 1x — native, large UI
```

## Keyboard / Mouse

The calculator sends a 64-bit key state bitmask over UART (115200 8N1) as `:%016llX\r\n`. The `uinput` daemon on the Pi translates this to Linux input events.

**Two modes** (toggle with power button):
- **Keyboard mode**: arrow keys, A-Z letters (via calculator alpha labels), numbers, modifiers
- **Mouse mode**: arrows move cursor continuously, OK = left click, Back = right click

**Two keymaps** (switch with xnt/var keys):
- Mode 0 (xnt): letters A-Z, space, punctuation
- Mode 1 (var): numbers 0-9, F1-F12, arithmetic operators

### Changes from zardam's daemon

| Aspect | Original | Patched |
|---|---|---|
| Mouse movement | X11 MouseKeys (NUMLOCK + numpad) | Direct EV_REL events |
| Arrow keys | KEY_KP4/KP8/KP2/KP6 (numpad) | KEY_LEFT/UP/DOWN/RIGHT |
| Continuous mouse | Not needed (X11 handled repeat) | poll() with 20ms timer while held |
| fd leak on mode switch | Opens new /dev/uinput without closing old | Closes old fd first |

## Status

Tested on Pi Zero 2 W with Raspbian Trixie 13, kernel 6.12.62+rpt-rpi-v7. Wayland compositor: labwc.

| Component | Status |
|---|---|
| DT overlay | Working — auto-loads on boot, vwidth/vheight configurable |
| drm-spifb | Working — handles both RGB565 (fbcon) and XRGB8888 (desktop), virtual resolution scaling |
| nwinput (keyboard) | Working — keyboard + mouse, Wayland-native, continuous mouse movement |

## Why Not panel-mipi-dbi?

The mainline `panel-mipi-dbi` driver requires MIPI DBI command protocol (set column/row/write commands 0x2A/0x2B/0x2C). Our display is raw RGB565 pixels over SPI — the STM32 handles all LCD controller commands internally. No MIPI DBI commands are sent from the Pi.

## References

- [DRM tiny drivers (mainline)](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/tiny) — pattern we follow
- [zardam's NumWorks Pi post](https://zardam.github.io/post/raspberrypi-numworks/) — original inspiration
