# NumWorks + Raspberry Pi

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](LICENSE.md)
[![Platform: Raspberry Pi](https://img.shields.io/badge/Platform-Raspberry_Pi_Zero_2W-c51a4a.svg)](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/)
[![OS: Debian Trixie](https://img.shields.io/badge/OS-Debian_Trixie-a80030.svg)](https://www.debian.org/)
[![Display: DRM/KMS](https://img.shields.io/badge/Display-DRM%2FKMS-green.svg)](pi-linux/drm-spifb/)
[![Compositor: Wayland](https://img.shields.io/badge/Compositor-Wayland-yellow.svg)](docs/pi-setup/trixie.md)

A Raspberry Pi Zero 2 W running Linux inside a NumWorks calculator. The calculator's display shows the Pi desktop via SPI, and the keyboard sends input via UART.

Based on [zardam's original project](https://zardam.github.io/post/raspberrypi-numworks/) (2018), updated for modern Linux (Debian Trixie, Wayland, DRM/KMS).

<!-- TODO: add photo/GIF here -->

## What's in This Repo

| Component | Location | Description |
|-----------|----------|-------------|
| **Firmware** | [`firmware/`](firmware/) | Git submodule: [Upsilon](https://github.com/UpsilonNumworks/Upsilon) fork with RPi app, SPI display bridge, UART keyboard, power control |
| **DRM display driver** | [`pi-linux/drm-spifb/`](pi-linux/drm-spifb/) | Kernel module: DRM/KMS tiny driver, XRGB8888 + RGB565, virtual resolution scaling |
| **Device Tree overlay** | [`pi-linux/overlay/`](pi-linux/overlay/) | SPI device registration for the display driver |
| **Keyboard daemon** | [`pi-linux/uinput-serial-keyboard/`](pi-linux/uinput-serial-keyboard/) | uinput daemon: UART key input + mouse mode, patched for Wayland |
| **Pi config files** | [`pi-linux/config/`](pi-linux/config/) | Boot config, systemd service, keyboard layout |
| **Power PCB** | [`nwpi-pcb`](https://github.com/elektricM/nwpi-pcb) | Custom PCB: USB charging, LiPo battery, 5V boost for Pi |
| **Documentation** | [`docs/`](docs/) | Architecture, setup guides, firmware reference |

## How It Works

```
Calculator (STM32F412)              Pi Zero 2 W (BCM2710)
+----------------------+            +----------------------+
| RPi app              |            | labwc (Wayland)      |
|   keyboard scan loop |  UART TX   |   uinput daemon      |
|   64-bit key bitmap  |----------->|   /dev/ttyS0 115200  |
|                      |            |                      |
| SPI1 slave + DMA     |  SPI 62.5  |   drm-spifb          |
|   EXTI on CS (PA6)   |<-----------|   KMS -> SPI direct  |
|   -> LCD FSMC write  |   MHz      |   480x360 -> 320x240 |
+----------------------+            +----------------------+
```

The Pi renders its desktop at 480x360 (configurable). The DRM driver downscales to 320x240 RGB565 and pushes frames over SPI. The STM32 DMA writes pixels directly to the LCD. No intermediate copies.

The keyboard daemon reads UART hex-encoded key state, converts to Linux input events, and supports two modes: keyboard (letters/numbers) and mouse (arrow keys move cursor, OK = click).

## Quick Start

### 1. Clone

```bash
git clone --recurse-submodules https://github.com/elektricM/numworks-pi.git
cd numworks-pi
```

### 2. Build & Flash Firmware (on your computer)

```bash
cd firmware
make MODEL=n0100 ENABLE_RPI=1 OMEGA_USERNAME="YourName" EPSILON_I18N=en -j$(nproc)
# Put calculator in DFU mode: hold reset + press 6
make MODEL=n0100 ENABLE_RPI=1 OMEGA_USERNAME="YourName" EPSILON_I18N=en epsilon_flash
```

### 3. Set Up the Pi

Flash Raspbian Trixie 32-bit Desktop to an SD card, enable SSH, boot, then:

```bash
# On the Pi — build display driver
cd pi-linux/drm-spifb
make
sudo make install
sudo depmod -a

# Install DT overlay
sudo ../scripts/install-overlay.sh

# Build keyboard daemon
cd ../uinput-serial-keyboard
gcc uinput.c -o uinput

# Install service
sudo cp ../config/nwinput.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable nwinput

# Add to /boot/firmware/config.txt:
#   enable_uart=1
#   dtparam=spi=on
#   dtoverlay=numworks-spifb

# Remove console=serial0,115200 from /boot/firmware/cmdline.txt
# Set keyboard to US (both /etc/default/keyboard and ~/.config/labwc/environment)

sudo reboot
```

### 4. Wire & Test

See [zardam's original blog post](https://zardam.github.io/post/raspberrypi-numworks/) for wiring details and hardware requirements. Tap the RPi icon on the calculator to start.

## What's Different from zardam's Original

| | zardam (2018) | This project |
|---|---|---|
| Pi OS | Raspbian Stretch/Buster | Trixie (Debian 13) |
| Display stack | fbdev (spifb) + fbcp (dispmanx) | DRM/KMS (drm-spifb), direct |
| Compositor | X11 | Wayland (labwc) |
| Display driver | `spi_busnum_to_master()` | Device Tree overlay |
| Resolution | Native 320x240 only | Virtual resolution (default 480x360) |
| Mouse input | X11 MouseKeys via NUMLOCK | Direct `EV_REL` events |
| Arrow keys | Numpad codes | Real arrow key codes |
| Firmware base | Epsilon | Upsilon (community fork) |
| UART | Worked by default | Fixed: consoleuart flavor + USART3 clock |

## Documentation

- [Architecture](docs/architecture.md) — display pipeline, power design, keyboard protocol, driver internals
- [Getting Started](docs/getting-started.md) — full setup walkthrough
- [Performance Investigation](docs/performance-investigation.md) — FPS analysis, SPI optimization, NEON experiments
- [Keyboard Map](docs/keymap.md) — full key mapping reference (modes, mouse, special characters)
- [Running Doom](docs/doom.md) — Chocolate Doom setup and controls
- [Firmware Build Guide](docs/firmware/build-guide.md) — build, flash, build flags
- [Firmware Porting Notes](docs/firmware/porting-notes.md) — differences from zardam's epsilon
- [UART Fix](docs/firmware/uart-fix.md) — consoleuart + USART3 clock fix
- [Pi Setup (Trixie)](docs/pi-setup/trixie.md) — Wayland, DRM, keyboard layout
- [Pi Setup Checklist](docs/pi-setup/checklist.md) — quick reference for fresh SD card
- [Hardware & Wiring (zardam's blog)](https://zardam.github.io/post/raspberrypi-numworks/) — original hardware guide

## Credits

- **Zardam** — original design, firmware, SPI driver, keyboard daemon
- **Upsilon** / **Omega** / **NumWorks** — calculator firmware
- See [CREDITS.md](CREDITS.md)

## License

Pi-side code and docs: GPL-2.0. Firmware submodule: CC BY-NC-SA 4.0 (see [`firmware/LICENSE.md`](firmware/LICENSE.md)).
