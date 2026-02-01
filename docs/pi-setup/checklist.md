# Pi Setup Checklist

Quick checklist for a fresh SD card setup.

## SD Card

- [ ] Flash Raspbian Trixie 32-bit Desktop
- [ ] Enable SSH (create empty `ssh` file on boot partition)
- [ ] Configure WiFi (optional: `wpa_supplicant.conf` on boot partition)
- [ ] Boot and SSH in

## Display Driver

- [ ] Install kernel headers: `sudo apt install raspberrypi-kernel-headers build-essential`
- [ ] Build drm-spifb: `cd pi-linux/drm-spifb && make`
- [ ] Install module: `sudo make install && sudo depmod -a`
- [ ] Install overlay: `sudo pi-linux/scripts/install-overlay.sh`

## Keyboard Daemon

- [ ] Build: `cd pi-linux/uinput-serial-keyboard && gcc uinput.c -o uinput`
- [ ] Install service: `sudo cp pi-linux/config/nwinput.service /etc/systemd/system/`
- [ ] Enable: `sudo systemctl daemon-reload && sudo systemctl enable nwinput`

## Boot Config (`/boot/firmware/config.txt`)

- [ ] Add `enable_uart=1` under `[all]`
- [ ] Add `dtparam=spi=on`
- [ ] Add `dtoverlay=numworks-spifb`

## System Config

- [ ] Remove `console=serial0,115200` from `/boot/firmware/cmdline.txt`
- [ ] Add `i2c-dev` and `uinput` to `/etc/modules`
- [ ] Add `blacklist spifb` to `/etc/modprobe.d/numworks.conf`
- [ ] Set keyboard to US in `/etc/default/keyboard`: `XKBLAYOUT="us"`
- [ ] Set keyboard to US in `~/.config/labwc/environment`: `XKB_DEFAULT_LAYOUT=us`

## Verify

- [ ] Reboot
- [ ] `lsmod | grep spifb` — drm_spifb loaded
- [ ] `sudo systemctl status nwinput` — active
- [ ] Tap RPi icon on calculator — display shows Pi desktop
- [ ] Press keys — keyboard input works
- [ ] Press power button — toggles mouse mode
