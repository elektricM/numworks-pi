# Camera

The Pi Zero 2 W's CSI connector is accessible inside the calculator and supports standard Raspberry Pi camera modules.

## Tested Cameras

### OV5647 (Pi Camera Module v1)

- **Resolution**: 2592x1944 (5MP)
- **Color**: RGB Bayer
- **Sensor format**: 10-bit GBRG
- **I2C address**: 0x36
- **Driver**: `ov5647` (mainline kernel)

| Mode | Max FPS | Crop |
|---|---|---|
| 640x480 | 58.92 | (16, 0)/2560x1920 |
| 1296x972 | 46.34 | (0, 0)/2592x1944 |
| 1920x1080 | 32.81 | (348, 434)/1928x1080 |
| 2592x1944 | 15.63 | (0, 0)/2592x1944 |

**Config**: Works with `camera_auto_detect=1` — no overlay needed.

### OV9281 (Global Shutter, Mono)

- **Resolution**: 1280x800 (1MP)
- **Color**: Monochrome
- **Sensor format**: 10-bit MONO
- **I2C address**: 0x60
- **Driver**: `ov9282` (mainline kernel, covers both OV9281 and OV9282)
- **Tuning file**: `/usr/share/libcamera/ipa/rpi/vc4/ov9281_mono.json`

| Mode | Format | Max FPS | Crop |
|---|---|---|---|
| 640x400 | R8 (8-bit) | 309.79 | (0, 0)/1280x800 |
| 1280x720 | R8 (8-bit) | 171.79 | (0, 0)/1280x720 |
| 1280x800 | R8 (8-bit) | 143.66 | (0, 0)/1280x800 |
| 640x400 | R10_CSI2P | 247.83 | (0, 0)/1280x800 |
| 1280x720 | R10_CSI2P | 137.42 | (0, 0)/1280x720 |
| 1280x800 | R10_CSI2P | 114.93 | (0, 0)/1280x800 |

**Config**: Does **not** work with `camera_auto_detect`. Requires manual overlay:

```
camera_auto_detect=0
dtoverlay=ov9281
```

## Requirements

### config.txt

The camera overlay must be in the `[all]` section of `/boot/firmware/config.txt`, after the NumWorks SPI display overlay:

```
[all]
enable_uart=1

# NumWorks SPI display
dtparam=spi=on
dtoverlay=numworks-spifb,vwidth=640,vheight=480

# Camera
dtoverlay=ov9281
```

For cameras supported by auto-detection (like the OV5647), use `camera_auto_detect=1` instead of a manual overlay.

### Kernel modules

The following modules are loaded automatically via the DT overlay — no manual `modules-load.d` entry needed:

- `ov5647` or `ov9282` (sensor driver)
- `bcm2835_unicam_legacy` (CSI receiver)
- `bcm2835_isp` (image signal processor)
- `bcm2835_codec` (hardware H.264 encoder/decoder)

### Software

Camera tools are pre-installed on Raspberry Pi OS (Trixie):

- `rpicam-still` — capture stills
- `rpicam-vid` — record video / stream
- `rpicam-hello` — viewfinder preview
- `v4l2-ctl` — low-level V4L2 control
- `ffmpeg` / `ffplay` — encoding, playback

### Hot-swap

Cameras can be physically swapped while the Pi is running, but the kernel detects cameras at boot. A **reboot is required** after swapping.

## Usage

### List detected cameras

```bash
rpicam-hello --list-cameras
```

### Capture a still

```bash
rpicam-still -o photo.jpg --width 1280 --height 800 --nopreview
```

### Preview on the calculator display

The rpicam EGL preview renders via XWayland. From SSH:

```bash
DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 rpicam-hello -t 0
```

This requires `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128` in the labwc environment so that the vc4 GPU is the render device for DRI3/linux-dmabuf (see [XWayland fix](debugging/xwayland-transparency.md)).

### Remote streaming to another machine

Stream over SSH — no server setup needed. The Pi hardware-encodes H.264:

```bash
ssh pi@nwpi.local "rpicam-vid -t 0 --width 1280 --height 800 --framerate 60 --codec h264 --inline --nopreview -o -" \
  | ffplay -probesize 32 -analyzeduration 0 -fflags nobuffer -flags low_delay -framedrop -i -
```

**Codec options**:

| Codec | Bandwidth | Latency | CPU | Notes |
|---|---|---|---|---|
| `h264` | ~5 Mbps | Low-medium | Low (hardware encoder) | Best for WiFi streaming |
| `mjpeg` | ~30+ Mbps | Lowest | Medium | May stutter on WiFi |

**Encoder limits**: The BCM2835 hardware H.264 encoder handles up to ~1280x800 @ 60fps. Higher framerates (e.g. 144fps from the OV9281) exceed its capacity and will fail with `failed to start output streaming`.

### Record video to file

```bash
rpicam-vid -t 10000 -o video.h264 --width 1280 --height 800 --framerate 60 --nopreview
```

## Troubleshooting

### "No cameras available"

- Check the ribbon cable is seated properly (contacts facing the board on Pi Zero)
- Verify the correct overlay is in config.txt
- Reboot after any config.txt change or cable swap
- Check `dmesg | grep -i -E 'ov|unicam|csi'` for probe errors

### Preview window doesn't show on calculator display

- Ensure XWayland is running (`DISPLAY=:0` must be available)
- Set `DISPLAY=:0` and `XDG_RUNTIME_DIR=/run/user/1000` when launching from SSH
- The EGL preview uses X11, not native Wayland — it requires XWayland

### Remote stream stutters

- WiFi signal below -65 dBm causes packet loss. Check with: `cat /proc/net/wireless`
- Switch from `mjpeg` to `h264` to reduce bandwidth ~6x
- Lower resolution or framerate
- Lower bitrate: add `--bitrate 2000000` (2 Mbps)
