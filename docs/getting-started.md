# Getting Started

Run a full Raspberry Pi desktop inside a NumWorks N0100 calculator. This guide covers the modern Trixie/Wayland setup using DRM/KMS -- no fbdev, no fbcp, no X11.

## 1. Prerequisites

**Hardware:**
- NumWorks N0100 calculator (the STM32-only model, no external flash)
- Raspberry Pi Zero 2 W
- MicroSD card (8 GB minimum, 16 GB recommended)
- USB-to-UART adapter (for initial Pi setup, or use SSH over Wi-Fi)
- STM32 DFU-capable USB cable (for flashing firmware)
- Soldering equipment for wiring SPI + UART + power between the Pi and the calculator

**Software (on your development machine):**
- [Raspberry Pi Imager](https://www.raspberrypi.com/software/) or `dd`
- ARM GCC toolchain (`arm-none-eabi-gcc`) for building firmware
- `dfu-util` for flashing the calculator

## 2. Flash the SD Card

1. Download **Raspberry Pi OS (32-bit) with Desktop** -- choose the **Trixie (Debian 13)** release.
2. Flash it to your MicroSD card using Raspberry Pi Imager.
3. In the imager's OS Customisation screen:
   - Set a hostname (e.g. `numworks`)
   - Set username to `pi` with a password
   - Configure Wi-Fi (SSID + password)
   - **Enable SSH** (password or key-based)
4. Insert the card into the Pi and boot it once on a normal monitor/keyboard to confirm it works. Alternatively, SSH in over Wi-Fi.

## 3. Clone the Repository

On the Pi (via SSH or terminal):

```bash
cd ~
git clone --recurse-submodules https://github.com/elektricM/numworks-rpi.git
```

This pulls down the DRM driver, keyboard daemon, Device Tree overlay, and config files.

## 4. Pi-Side Setup

All commands below run on the Pi itself.

### 4.1 Build and Install the DRM Display Driver

```bash
cd ~/numworks-rpi/pi-linux/drm-spifb
make
sudo make install
sudo depmod -a
```

This builds `drm-spifb.ko`, a DRM/KMS tiny driver that sends framebuffer contents over SPI to the calculator's STM32. No fbdev layer, no fbcp -- the GPU composites directly to SPI.

### 4.2 Install the Device Tree Overlay

```bash
sudo ~/numworks-rpi/pi-linux/scripts/install-overlay.sh
```

This compiles `numworks-spifb.dts` and copies the resulting `.dtbo` to `/boot/firmware/overlays/`.

### 4.3 Build the Keyboard Daemon

```bash
cd ~/numworks-rpi/pi-linux/uinput-serial-keyboard
gcc uinput.c -o uinput
```

The daemon reads key state from UART and injects Linux input events via uinput. It is patched for Wayland (direct `EV_REL` mouse events instead of X11 MouseKeys).

### 4.4 Configure Boot Files

**`/boot/firmware/config.txt`** -- add at the end of the `[all]` section:

```ini
enable_uart=1

# NumWorks SPI display
dtparam=spi=on
dtoverlay=numworks-spifb
```

**`/boot/firmware/cmdline.txt`** -- remove `console=serial0,115200` if present (the UART is used for keyboard data, not a serial console).

**`/etc/modules`** -- ensure these modules are listed:

```
i2c-dev
uinput
```

### 4.5 Install the Keyboard Service

```bash
sudo cp ~/numworks-rpi/pi-linux/config/nwinput.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable nwinput
```

> **Note:** The service expects the binary at `/home/pi/uinput-serial-keyboard/uinput`. If your paths differ, edit the `ExecStart` and `WorkingDirectory` lines in `nwinput.service`.

### 4.6 Set Keyboard Layout to US

The calculator keymap assumes US QWERTY. Set it for the Wayland compositor (labwc):

```bash
mkdir -p ~/.config/labwc
cp ~/numworks-rpi/pi-linux/config/labwc-environment ~/.config/labwc/environment
```

The file sets:

```
XKB_DEFAULT_LAYOUT=us
```

Also set the system keyboard layout:

```bash
sudo cp ~/numworks-rpi/pi-linux/config/keyboard /etc/default/keyboard
```

### 4.7 Reboot

```bash
sudo reboot
```

## 5. Firmware Side

This runs on your development machine (not the Pi).

### 5.1 Build the Firmware

The firmware is an Upsilon fork with Raspberry Pi integration support.

```bash
cd ~/numworks-rpi/firmware
make MODEL=n0100 ENABLE_RPI=1 OMEGA_USERNAME="YourName" EPSILON_I18N=en -j$(nproc)
```

### 5.2 Flash the Calculator

Put the calculator into DFU mode: **hold Reset, then press 6, then release Reset**.

```bash
make MODEL=n0100 ENABLE_RPI=1 OMEGA_USERNAME="YourName" EPSILON_I18N=en epsilon_flash
```

Requires `dfu-util` installed on your machine.

## 6. Hardware Wiring

Connect the Pi Zero 2 W to the NumWorks N0100 mainboard:

| Signal | Calculator Pin | Pi GPIO | Notes |
|--------|---------------|---------|-------|
| SPI CLK | PA5 (SPI1_SCK) | GPIO 11 (SPI0_SCLK) | |
| SPI CS | PA6 (SPI1_NSS) | GPIO 8 (SPI0_CE0) | Active low |
| SPI MOSI | PA7 (SPI1_MOSI) | GPIO 10 (SPI0_MOSI) | Pi sends pixels to STM32 |
| UART TX | PD8 (USART3_TX) | GPIO 15 (UART0_RX) | Keyboard data, calc to Pi |
| Power control | PB9 | MOSFET gate | STM32 controls Pi power on/off |

SPI runs at 62.5 MHz (250 MHz / 4). UART runs at 115200 baud, 8N1.

> See `docs/hardware/` for wiring photos and detailed pinout information.

## 7. First Boot

1. Power on the calculator.
2. On the calculator home screen, tap the **RPi** icon (added by the `ENABLE_RPI=1` firmware build).
3. The STM32 powers on the Pi via the MOSFET, then waits for SPI frames.
4. After the Pi boots (~20-30 seconds), the Raspberry Pi desktop should appear on the calculator screen.
5. Use the calculator keys to navigate. Press the **power** button to toggle between keyboard and mouse mode.

## 8. Troubleshooting

### Black screen after selecting RPi

- Verify the DT overlay is installed: `ls /boot/firmware/overlays/numworks-spifb.dtbo`
- Check that `dtparam=spi=on` and `dtoverlay=numworks-spifb` are in `config.txt`
- Confirm the driver loads: `lsmod | grep drm_spifb`
- Check kernel logs: `dmesg | grep spifb`
- Verify wiring -- SPI CLK, CS, and MOSI must all be connected

### No keyboard input

- Check the service is running: `systemctl status nwinput`
- Verify UART wiring (PD8 to GPIO 15)
- Confirm `console=serial0,115200` is **removed** from `cmdline.txt` (otherwise the kernel grabs the UART)
- Mask the serial getty: `sudo systemctl mask serial-getty@ttyS0.service` (it competes with nwinput for the port, causing crashes and random mode switches)
- Check that `uinput` is in `/etc/modules` or loaded: `lsmod | grep uinput`

### Wrong keyboard layout / garbled input

- Ensure `~/.config/labwc/environment` contains `XKB_DEFAULT_LAYOUT=us`
- Ensure `/etc/default/keyboard` is set to `us`
- Log out and back in (or reboot) after changing layout

### Display works but resolution is wrong

The default virtual resolution is 480x360 (1.5x scaling). To change it, edit the overlay line in `/boot/firmware/config.txt`:

```ini
dtoverlay=numworks-spifb,vwidth=640,vheight=480   # 2x -- more content, smaller text
dtoverlay=numworks-spifb,vwidth=320,vheight=240   # 1x -- native, large UI elements
```

Reboot after changing.
