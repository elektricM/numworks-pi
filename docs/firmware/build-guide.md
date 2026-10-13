# Firmware Build & Flash Guide

## Prerequisites

- **ARM GCC toolchain** (`arm-none-eabi-gcc`)
- **Python 3** (3.8+)
- **libusb** — on macOS: `brew install libusb`

## Clone

```bash
git clone --recurse-submodules https://github.com/elektricM/numworks-pi.git
cd numworks-pi/firmware
```

## Build (N0100 device)

```bash
make MODEL=n0100 ENABLE_RPI=1 OMEGA_USERNAME="Martin" EPSILON_I18N=en -j$(nproc)
```

Output binary: `output/release/device/n0100/epsilon.dfu`

## Build (simulator)

```bash
make PLATFORM=simulator ENABLE_RPI=1 -j$(nproc)
```

## Flash (N0100)

1. Put the calculator in DFU mode: **hold reset + press 6**.
2. Flash:

```bash
make MODEL=n0100 ENABLE_RPI=1 OMEGA_USERNAME="Martin" EPSILON_I18N=en epsilon_flash
```

### Remote reflash (no buttons)

Once an `ENABLE_RPI=1` firmware from 2026-10-13 or later is running, with the Pi app open and the calculator plugged into the Mac over USB:

```bash
make MODEL=n0100 ENABLE_RPI=1 RPI_AUTOSTART=1 OMEGA_USERNAME="Martin" EPSILON_I18N=en output/release/device/n0100/epsilon.bin
../tools/reflash-n0100.sh
```

The Pi asks the calculator to enter DFU (`SYS:DFU`), powers itself off, and the script flashes over USB. With `RPI_AUTOSTART=1` the new firmware boots straight into the Pi app and powers the Pi up again. The script also works when the calculator is already in the ROM bootloader (reset + 6), or sitting on the Upsilon home screen with USB plugged in: it is then already a USB device, so the script flashes it directly. `nwpi-ctl dfu` alone gets no answer there, because the calculator only listens to the Pi inside the Pi app.

After a reflash, if the Pi doesn't come back, unplug USB. The Pi has been seen not booting while the calculator is on USB (see `hardware/nwpi/README.md`).

Flash space is tight: about 0.5 KiB of the 1 MiB is free with `ENABLE_RPI=1`.

## Build Flags

| Flag | Description |
|------|-------------|
| `ENABLE_RPI` | Gate all Raspberry Pi integration code. Set to `1` to enable. Builds without this flag are completely unaffected. |
| `RPI_AUTOSTART` | Development: open the Pi app at boot, so the Pi powers up after every (re)flash. Default `0`. |
| `MODEL` | Hardware target. Use `n0100` for the flashable model. |
| `OMEGA_USERNAME` | Username shown in the About screen. |
| `EPSILON_I18N` | Language code (`en`, `fr`, `de`, etc.). |
| `THEME_NAME` | Optional theme name for Upsilon theming. |
| `THEME_REPO` | Optional Git URL for an external theme repository. |
| `PLATFORM` | Set to `simulator` for desktop simulator builds. |

## Notes

- **macOS Apple Silicon (libusb):** The Homebrew libusb path fix is already applied in the repo at `build/device/usb/backend/libusb1.py`. No manual patching needed.
- **Normal builds:** Omitting `ENABLE_RPI=1` produces a standard Upsilon firmware with no Pi-related code compiled in.
