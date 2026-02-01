# Architecture

## How the Original Worked (zardam, 2018)

```
GPU renders to fb0 (virtual HDMI 640x480)
        |
        v
   fbcp (dispmanx)          captures GPU output via Broadcom proprietary API
        |
        v
   /dev/fb1 (spifb)         fbdev module, creates SPI device via spi_busnum_to_master()
        | cpu_to_be16 byte swap, 5 x 32KB SPI transfers
        v
   SPI0 @ 62.5 MHz          BCM2835 hardware SPI
        |
        v
   STM32 SPI slave + DMA    on the calculator, EXTI interrupt on CS configures LCD window
        |
        v
   LCD (320x240 RGB565)     via FSMC memory-mapped write
```

Two problems killed this on modern kernels:
- `spi_busnum_to_master()` removed in kernel 5.18
- `vc_dispmanx_*` removed in Bookworm

## How It Works Now

```
KMS compositor (Wayland labwc)
        | renders at virtual resolution (default 480x360)
        v
   drm-spifb                DRM/KMS driver (drm_simple_display_pipe)
        | format conversion: XRGB8888 → RGB565 (or RGB565 byte swap)
        | nearest-neighbor downscale: 480x360 → 320x240
        | cpu_to_be16 byte swap, 5 x 32KB SPI transfers
        v
   SPI0 @ 62.5 MHz          same hardware, same speed
        |
        v
   STM32 SPI slave + DMA    unchanged — same wire protocol
        |
        v
   LCD (320x240 RGB565)
```

No fbcp, no fbdev, no intermediate copy. KMS composites directly to the SPI display. This is how all modern SPI display drivers work in mainline Linux (`drivers/gpu/drm/tiny/`).

### Pixel Format Handling

The driver accepts two framebuffer formats:

- **RGB565** — used by fbcon (boot text). Byte-swapped to big-endian via `drm_fb_swab()` (1:1) or custom nearest-neighbor scaler.
- **XRGB8888** — used by the Wayland compositor (desktop). Converted to RGB565 via `drm_fb_xrgb8888_to_rgb565()` with `swab=true` (1:1) or custom scaler that does conversion + downscale + byte swap in one pass.

Format is detected per-frame via `fb->format->format`.

### Virtual Resolution Scaling

The desktop compositor (labwc) designed for 1080p renders UI that's too large at native 320x240. The driver advertises a configurable virtual resolution (default 480x360) to the compositor, then downscales to physical 320x240 before SPI transfer.

- Scaling uses nearest-neighbor (integer arithmetic: `sx = x * vw / w`)
- Custom scalers handle both XRGB8888→RGB565 conversion and downscale in a single pass
- When virtual == physical, the standard DRM format helpers are used (no custom scaler)
- Virtual resolution configurable via DT overlay `vwidth`/`vheight` properties

## Wire Protocol Compatibility

The data on the SPI bus is identical to the original:

| Property | Original (zardam) | drm-spifb |
|---|---|---|
| SPI bus / CS | 0 / 0 | 0 / 0 (from DT) |
| SPI mode | 0 | 0 (default) |
| Max speed requested | 70 MHz | 70 MHz (DT) |
| Effective speed | 62.5 MHz | 62.5 MHz |
| Frame size | 153,600 bytes | 153,600 bytes |
| Pixel format | big-endian RGB565 | big-endian RGB565 |
| Byte swap | `cpu_to_be16()` | `cpu_to_be16()` |
| Transfer chunking | 5 hardcoded transfers, 32768 bytes | Dynamic loop, 32768 byte chunks |
| Last chunk size | 22,528 bytes | 22,528 bytes |
| SPI call | `spi_sync()` single message | `spi_sync()` single message |

The STM32 firmware sees the exact same SPI traffic. No calculator-side changes needed.

## DT Overlay (`overlay/numworks-spifb.dts`)

Tells the kernel: "there's a device called `numworks,spifb` on SPI bus 0, chip select 0, max 70 MHz."

When loaded, it:
1. Enables SPI0
2. Disables the default `spidev` on CE0 (so our driver can claim it)
3. Creates an SPI device node with `compatible = "numworks,spifb"`
4. The driver's `probe()` gets called automatically

Parameters (speed, width, height, fps) can be overridden from config.txt:
```
dtoverlay=numworks-spifb,speed=70000000,width=320,height=240
```

## DRM Driver (`drm-spifb/drm-spifb.c`)

A DRM tiny driver following the `repaper.c` pattern:

- **Registration**: `struct spi_driver` + `module_spi_driver()`. Auto-loads via DT `compatible` match and SPI modalias (`spi:spifb`).
- **Allocation**: `devm_drm_dev_alloc()` — embedded `struct drm_device` inside `struct nw_spifb`, managed lifetime.
- **Display pipe**: `drm_simple_display_pipe` — single struct providing CRTC + encoder + plane. Shadow plane (`DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS`) gives CPU-accessible buffer.
- **Mode config**: `drm_mode_config_funcs` with `drm_gem_fb_create_with_dirty` (triggers update on userspace writes), `drm_atomic_helper_check`, `drm_atomic_helper_commit`.
- **Connector**: `DRM_MODE_CONNECTOR_SPI`, single fixed 320x240 mode.
- **Formats**: `DRM_FORMAT_RGB565` (native, fbcon) and `DRM_FORMAT_XRGB8888` (compositor). Format-aware `send_frame()` picks the right conversion path.
- **Virtual resolution**: `vwidth`/`vheight` DT properties (default 480x360). Connector advertises virtual resolution; driver downscales to physical before SPI transfer.
- **Frame send**: `pipe_update()` → reads shadow buffer → format conversion + optional downscale → DMA-coherent TX buffer → `spi_sync()` with 32KB chunks.
- **fbdev emulation**: `drm_fbdev_generic_setup()` provides `/dev/fb0` for legacy apps and fbcon.

### Differences from zardam's original

| Aspect | zardam spifb | drm-spifb |
|---|---|---|
| Subsystem | fbdev (deprecated) | DRM/KMS (modern) |
| GPU path | GPU → fbcp → fbdev → SPI (2 copies) | KMS → SPI (direct) |
| Device creation | `spi_busnum_to_master()` | Device Tree overlay |
| Driver type | `platform_driver` + manual SPI | `spi_driver` |
| State | Global variables | Per-device struct |
| fb_ops | `struct fb_ops` (mutable) | N/A (DRM handles it) |
| Resolution | `#define WIDTH 320` | DT property with default |
| Refresh trigger | Deferred I/O (`HZ/60`) | KMS atomic commit |
| Connector type | N/A (fbdev) | `DRM_MODE_CONNECTOR_SPI` |

## Kernel Compatibility

| Feature | 5.10 (Bullseye) | 6.1 (Bookworm) | 6.12 (Trixie) |
|---|---|---|---|
| `spi_busnum_to_master()` | present | removed | removed |
| `DRM_MODE_CONNECTOR_SPI` | available (5.14+) | available | available |
| `drm_fbdev_dma.h` | missing | missing | missing* |
| DT overlay approach | works | works | works |

*Pi kernel headers don't ship `drm_fbdev_dma.h`; the driver uses `drm_fb_helper.h` + `drm_fbdev_generic_setup()`.

## Resolved Issues

### NULL deref crash on kernel 6.12

**Symptom**: `modprobe drm-spifb` → NULL pointer dereference at `drm_mode_validate_driver+0xa8`

**Root cause**: `drm_mode_validate_driver()` accesses `dev->mode_config.funcs->mode_valid` without NULL-checking `funcs`. The driver was missing `drm->mode_config.funcs`.

**Fix**: Added `drm_mode_config_funcs` with `.fb_create`, `.atomic_check`, `.atomic_commit` — set in probe after `drmm_mode_config_init()`. Found by comparing with `repaper.c`.

### Module wouldn't auto-load on boot

**Symptom**: DT overlay active, SPI device created, but module not loaded.

**Root cause**: `spi_device_id` was `"numworks,spifb"` but SPI modalias strips the vendor prefix → actual modalias is `spi:spifb`.

**Fix**: Changed `spi_device_id` to `"spifb"`. The `of_match_table` still uses full `"numworks,spifb"` for DT matching.

### XRGB8888 format not handled

**Symptom**: Boot text fine (RGB565), desktop garbled (wrong colors, 2x horizontal zoom).

**Root cause**: Desktop compositor uses XRGB8888 (4 bytes/pixel). Driver treated all data as RGB565 (2 bytes/pixel). Each 4-byte pixel was read as two 2-byte pixels — doubling width and garbling colors.

**Fix**: Format-aware `send_frame()` checks `fb->format->format`. Uses `drm_fb_xrgb8888_to_rgb565()` with `swab=true` for XRGB8888, `drm_fb_swab()` for RGB565. Custom scalers for non-1:1 virtual resolution.

### Desktop UI too large at 320x240

**Symptom**: Pi OS desktop designed for 1080p. At native 320x240, taskbar and icons fill the entire screen.

**Fix**: Added virtual resolution support. Driver advertises configurable larger resolution to compositor (default 480x360), downscales to 320x240 with nearest-neighbor before SPI transfer. Configurable via DT overlay `vwidth`/`vheight` parameters.

## Keyboard / UART Architecture

```
Calculator (STM32F412)                    Pi (BCM2710)
┌──────────────────────┐                 ┌──────────────────────┐
│ Ion::Keyboard::scan()│                 │ /dev/ttyS0           │
│   → 64-bit bitmask   │                 │   115200 8N1         │
│                      │                 │                      │
│ if (scan != lastScan)│                 │ uinput daemon        │
│   format :%016llX\r\n│   USART3 TX    │   poll() loop        │
│   Ion::Console::     │───────────────→ │   parse hex          │
│     writeLine()      │  PD8 → GPIO15  │   process(scan)      │
│                      │                 │     ↓                │
│ USART3, 115200 baud  │                 │   /dev/uinput        │
│ APB1 clock, AF7      │                 │     ↓                │
└──────────────────────┘                 │   libinput → labwc   │
                                         └──────────────────────┘
```

### Protocol

- **Format**: `:%016llX\r\n` (colon + 16 zero-padded uppercase hex + CR + LF)
- **Trigger**: Only sent when key state changes (not continuous)
- **Example**: `:0000000000000001\r\n` = left arrow pressed

### Key Bitmap (64-bit, one bit per key)

```
Bit  Key              Bit  Key              Bit  Key
 0   Left              18  Exp (A)           36  Four (R)
 1   Up                19  Ln (B)            37  Five (S)
 2   Down              20  Log (C)           38  Six (T)
 3   Right             21  Imaginary (D)     39  Multiply (U)
 4   OK                22  Comma (E)         40  Divide (V)
 5   Back              23  Power ^ (F)       41  —
 6   Home              24  Sin (G)           42  One (W)
 7   OnOff             25  Cos (H)           43  Two (X)
 8-11 (undefined)      26  Tan (I)           44  Three (Y)
12   Shift             27  Pi (J)            45  Plus (Z)
13   Alpha             28  Sqrt (K)          46  Minus (Space)
14   XNT               29  Square (L)        47  —
15   Var               30  Seven (M)         48  Zero (?)
16   Toolbox           31  Eight (N)         49  Dot (,)
17   Backspace         32  Nine (O)          50  EE (Ctrl)
                       33  LeftParen (P)     51  Ans (Alt)
                       34  RightParen (Q)    52  EXE (Enter)
                       35  —
```

Letters in parentheses are the alpha labels printed on the calculator keys, which map to the keyboard output in mode 0.

### Daemon Design (uinput.c)

The daemon was patched from zardam's original for Wayland compatibility:

1. **Arrow keys** emit `KEY_LEFT/UP/DOWN/RIGHT` (not numpad KP4/8/2/6 which showed as digits on Wayland)
2. **Mouse mode** (toggled by power button, bit 7): emits `EV_REL` events directly instead of relying on X11 MouseKeys via NUMLOCK
3. **Continuous mouse movement**: since the calculator only sends on key state change, the daemon uses `poll()` with a 20ms timeout to keep emitting `REL_X`/`REL_Y` while arrow keys are held (~50 Hz)
4. **Release jump fix**: mouse movement only emitted on poll timeout (`ret == 0`), not when serial data arrives, preventing extra movement on key release
5. **fd leak fix**: `input_setup()` closes old `/dev/uinput` fd before opening a new one (zardam's leaked on every mode switch)
