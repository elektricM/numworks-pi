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

## Build Flags

| Flag | Description |
|------|-------------|
| `ENABLE_RPI` | Gate all Raspberry Pi integration code. Set to `1` to enable. Builds without this flag are completely unaffected. |
| `MODEL` | Hardware target. Use `n0100` for the flashable model. |
| `OMEGA_USERNAME` | Username shown in the About screen. |
| `EPSILON_I18N` | Language code (`en`, `fr`, `de`, etc.). |
| `THEME_NAME` | Optional theme name for Upsilon theming. |
| `THEME_REPO` | Optional Git URL for an external theme repository. |
| `PLATFORM` | Set to `simulator` for desktop simulator builds. |

## Notes

- **macOS Apple Silicon (libusb):** The Homebrew libusb path fix is already applied in the repo at `build/device/usb/backend/libusb1.py`. No manual patching needed.
- **Normal builds:** Omitting `ENABLE_RPI=1` produces a standard Upsilon firmware with no Pi-related code compiled in.
