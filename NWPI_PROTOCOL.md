# NWPI Protocol Specification v1.0

## Overview
Bidirectional UART protocol between NumWorks calculator (Upsilon) and Raspberry Pi.
Designed for extensibility while keeping messages simple and low-overhead.

---

## Physical Layer
- **UART**: 115200 baud, 8N1
- **Voltage**: 3.3V logic
- **Lines**: TX/RX (bidirectional), shared GND

---

## Message Format

### Basic Structure
```
<CMD>:<PAYLOAD>\n
```

- **CMD**: 2-4 uppercase characters (command identifier)
- **:**: Separator
- **PAYLOAD**: Command-specific data (may be empty)
- **\n**: Message terminator (0x0A)

### Examples
```
KEY:0000000000000001\n      # Keyboard state
AI:What is gravity?\n        # AI query
CAM:SNAP\n                   # Take photo
OK:CAM\n                     # Acknowledge
ERR:TIMEOUT\n                # Error
```

---

## Command Categories

### 1. Keyboard (Legacy Compatible)
For backward compatibility with existing keyboard daemon.

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| C→P | `KEY` | 16 hex chars | 64-bit key bitmap |

**Example:**
```
KEY:000000000000002A\n
```

---

### 2. AI Assistant

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| C→P | `AI` | query text | Send text-only AI query |
| C→P | `AIV` | query text | Vision query: snap photo + send both to multimodal AI |
| C→P | `AIA` | query text | AI with last photo attached (manual snap first) |
| P→C | `AIR` | response text | AI response (may be chunked) |
| P→C | `AIS` | chunk text | AI streaming chunk |
| P→C | `AIE` | (empty) | AI response end (for streaming) |

**Simple query/response:**
```
C→P: AI:What is 2+2?\n
P→C: AIR:The answer is 4.\n
```

**Vision query (auto-snap):**
```
C→P: AIV:What's on this page?\n
Pi:  [takes photo with libcamera]
Pi:  [sends image + text to Claude/GPT-4V]
P→C: AIR:I see a math worksheet with quadratic equations...\n
```

**Vision with manual snap:**
```
C→P: CAM:SNAP\n
P→C: OK:CAM:photo_001.jpg\n
C→P: AIA:Solve the equation in the photo\n
P→C: AIR:The equation is x²+2x-3=0, solving: x=1 or x=-3\n
```

**Streaming response:**
```
C→P: AI:Explain gravity\n
P→C: AIS:Gravity is \n
P→C: AIS:a force that \n
P→C: AIS:attracts objects...\n
P→C: AIE:\n
```

**Note:** Photos for vision queries stay on Pi - no image transfer over UART needed.

---

### 3. Camera

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| C→P | `CAM` | subcommand | Camera control |
| P→C | `CAMR` | result | Camera result |

**Subcommands:**
- `SNAP` - Take photo, save to Pi
- `SNAP:filename.jpg` - Take photo with specific name
- `LIST` - List saved photos
- `DEL:filename` - Delete photo
- `VIEW:filename` - Send thumbnail to calc (future)

**Examples:**
```
C→P: CAM:SNAP\n
P→C: OK:CAM:photo_001.jpg\n

C→P: CAM:LIST\n
P→C: CAMR:photo_001.jpg,photo_002.jpg\n

C→P: CAM:SNAP:homework.jpg\n
P→C: OK:CAM:homework.jpg\n
```

---

### 4. System Control

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| C→P | `SYS` | subcommand | System control |
| P→C | `SYSR` | result | System response |

**Subcommands:**
- `PING` - Check connection
- `INFO` - Get Pi system info
- `SHUT` - Shutdown Pi
- `REBOOT` - Reboot Pi
- `WIFI:ssid:pass` - Connect to WiFi
- `WIFIS` - Scan WiFi networks
- `IP` - Get IP address
- `BAT` - Get battery status (if monitored)

**Examples:**
```
C→P: SYS:PING\n
P→C: OK:PONG\n

C→P: SYS:INFO\n
P→C: SYSR:uptime=3600,mem=450M,cpu=12%\n

C→P: SYS:IP\n
P→C: SYSR:192.168.1.42\n

C→P: SYS:SHUT\n
P→C: OK:SYS:shutdown in 5s\n
```

---

### 4b. Remote reflash (implemented)

| Direction | Message | Meaning |
|-----------|---------|---------|
| P→C | `SYS:DFU` | Enter USB DFU so the calculator can be flashed without touching it |
| C→P | `SYS:DFU` | Acknowledged: the calculator is in DFU. nwpid powers the Pi off, because the reset that ends a flash cuts the Pi's power |

The calculator waits up to 10 s for USB enumeration. If no flash happens (Back key, unplugged, timeout), it waits until 20 s after the request, by which time the Pi has halted, and then power-cycles the Pi. From the Mac, `tools/reflash-n0100.sh` runs the whole sequence: `nwpi-ctl dfu`, wait for the Pi to halt, `dfu-util`. It matches only the N0100's 1 MiB flash map, so an N0110 plugged in alongside is never touched.

### 4c. Display bridge (implemented)

| Direction | Message | Meaning |
|-----------|---------|---------|
| C→P | `DSP:2` | Bridge (re)started and supports display protocol 2 (sent on entering the Pi app, and in reply to `DSP:?`) |
| P→C | `DSP:?` | Ask for the supported protocol |
| P→C | `DSP:ON` / `DSP:OFF` | Switch protocol 2 on/off; the calculator answers `DSP:OK` |
| P→C | `DSP:STAT` | Statistics, see below |
| P→C | `DSP:PX:x,y` | Read 4 landscape pixels back from LCD GRAM: `DSP:PX x,y=RGB565,...` (protocol 2 only) |

**Updates.** In protocol 2, every update is one SPI message of up to 8 rectangles (`NW_MAX_RECTS`). Each rectangle is an 8-word header frame (`0x4E57, 2, MADCTL, c0, c1, p0, p1, 0xFFFF ^ xor(w0..w6)`, big-endian 16-bit words), a CS-high gap, then the pixels of that window only, in the panel's native page order (MADCTL 0x00: column = landscape y, page = 319 − landscape x). Rectangles in the same message are separated by 10 µs of CS high, and so are consecutive messages. The driver turns the compositor's damage clips into rectangles and merges overlapping ones. If more than 8 remain, or they would cost more than their bounding box, it sends the bounding box instead.

**The header gap** is the time the calculator has, after the header's CS rise, to check it, choose a refresh rate, and program the LCD window and DMA before the first pixel arrives. The driver's `header_gap_us` defaults to 40 (maximum 60); the calculator measures it as `gap_us` (about 43). The gap must be spun, not slept. The SPI core spins only for `cs_change_delay` ≤ 10 µs and sleeps for anything longer, and those sleeps measured 66–103 µs for a nominal 40, which threw off the calculator's tear timing. So the driver uses 10 µs of `cs_change_delay`, then chains empty `cs_off` transfers of ≤ 10 µs `delay` each for the rest. If pixels arrive before the calculator is ready, the SPI overruns and `late` counts it; it should stay at 0.

**Tearing.** The calculator timestamps the LCD's tearing-effect line and, for each update, sets the panel refresh rate (FRCTRL2 RTNA, 4–30, about 90–40 Hz) so the scan never crosses the write. It aims for a line time equal to this rectangle's page write time: a pass can only tear if it starts within (pages − 1) × |line time − page time| of the write, so equal speeds never tear. If no rate in range avoids it, the update is counted as `torn`. Typical figures at 640×480: no torn full frames, and about 2% of small updates with `top` running.

**Recovery.** Entering the Pi app re-initialises the bridge from scratch: SPI1 GPIO alternate functions, a peripheral reset of SPI1 (only an RCC reset restarts the slave's bit count; one stray clock edge otherwise misaligns every later frame), DMA, and the CS interrupt. The SPI is also reset between every header and pixel frame. The calculator then sends `DSP:2`. nwpid switches protocol 2 back on (`DSP:ON`) and, on `DSP:OK`, writes `1` to the driver's `repaint` parameter (`/sys/module/drm_spifb/parameters/repaint`), so the whole desktop is resent at once instead of staying grey until something moves. With the legacy protocol it repaints straight away.

Switching order avoids showing garbage. Going up, the calculator switches first and drops legacy frames as bad headers until the driver follows. Going down, the driver switches first. nwpid does this automatically. `touch /etc/nwpi/display-legacy` keeps the legacy protocol.

**Statistics** (`nwpi-ctl stat`; they survive Upsilon's power-off, which is a sleep, and clear only on reset or reflash):

```
DSP:STAT frames= bad= rtna= switches= torn= isr_us= px_ns= gap_us= osc_pm= late= HHHH.HHHH.HHHH.HHHH.HHHH.HHHH.HHHH.HHHH.NNNN
```

`frames` pixel frames received; `bad` rejected headers; `rtna` the current refresh setting and `switches` how often it changed; `torn` updates no refresh rate could keep tear-free; `isr_us` the longest CS interrupt; `px_ns` measured time per pixel on the wire; `gap_us` measured header gap; `osc_pm` the panel oscillator calibration in per mille; `late` pixel frames that overran the SPI. The last field is the last rejected header (8 words) and the DMA count left (`NNNN`, 0 when all 8 words arrived), for diagnosing a bad link. `bad` rises by a few while the Pi boots or the driver reloads; if it climbs during normal use, check the MOSI wire (see `docs/pi-setup/handoff-2026-10-13.md`): with a marginal MOSI connection the headers arrive as `FFFF`, `0000` or bit-shifted words while slower SPI clocks still work.

The Pi side keeps its own counters in `/sys/kernel/debug/dri/1/spifb_stats` (root): mode, protocol, updates, rectangles, bytes, the time to build a frame (`prep_avg_us`, about 1.6 ms for a full 640×480 frame with NEON) and the time spent waiting for the previous transfer.

On the Pi, `nwpi-ctl stat|px X Y|dfu|send LINE|log` uses nwpid's control FIFO (`/run/nwpid.ctl`), which is writable by the `dialout` group, so no sudo is needed. Calculator replies are appended to `/run/nwpid.log`.

---

### 5. File Transfer (Future)

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| C→P | `FILE` | subcommand | File operations |
| P→C | `FILER` | result | File response |
| P→C | `FILED` | base64 chunk | File data |

**Subcommands:**
- `LIST:path` - List directory
- `GET:filename` - Download file to calc
- `PUT:filename:size` - Upload file from calc
- `DEL:filename` - Delete file

**Example (download):**
```
C→P: FILE:GET:notes.txt\n
P→C: FILED:SIZE:1234\n
P→C: FILED:SGVsbG8gV29ybGQ=\n    # base64 chunks
P→C: FILED:END\n
```

---

### 6. Acknowledgments & Errors

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| Both | `OK` | original cmd | Success ack |
| Both | `ERR` | error info | Error response |

**Error codes:**
- `TIMEOUT` - Operation timed out
- `NOTFOUND` - Resource not found
- `BUSY` - Pi busy with another operation
- `BADCMD` - Unknown command
- `BADARG` - Invalid argument
- `NOPERM` - Permission denied
- `NOCONN` - No network connection
- `APIERR` - External API error

**Examples:**
```
P→C: OK:CAM\n                    # Camera command succeeded
P→C: ERR:TIMEOUT:AI\n            # AI query timed out
P→C: ERR:NOTFOUND:photo.jpg\n    # File not found
P→C: ERR:NOCONN:AI\n             # No internet for AI
```

---

### 7. Mode/State

| Direction | Command | Payload | Description |
|-----------|---------|---------|-------------|
| C→P | `MODE` | mode name | Switch daemon mode |
| P→C | `MODER` | current mode | Mode confirmation |

**Modes:**
- `KBD` - Keyboard only (default)
- `FULL` - Full protocol (all commands)
- `AI` - AI-only mode (no keyboard passthrough)

**Example:**
```
C→P: MODE:FULL\n
P→C: OK:MODE:FULL\n
```

---

## Implementation Notes

### Message Size Limits
- **Max command**: 4 characters
- **Max payload**: 1024 bytes (configurable)
- **Max total message**: 1032 bytes

### Chunking Long Data
For payloads > 1024 bytes, use streaming/chunking:
1. Send initial command
2. Send data in chunks with sequence indicator
3. Send END marker

### Binary Data
Use base64 encoding for binary data (images, files).

### Timeouts
- **Default timeout**: 30 seconds
- **AI queries**: 60 seconds
- **Camera**: 10 seconds
- **Ping**: 5 seconds

### Concurrency
Protocol is request-response. One outstanding request at a time.
Exception: `KEY` commands are fire-and-forget (no ack needed).

---

## Pi Daemon Architecture

The `nwpid` (NumWorks Pi Daemon) runs on the Pi and handles all UART communication.

```
                    ┌─────────────────────────────┐
                    │         nwpid               │
                    │   (NumWorks Pi Daemon)      │
                    │                             │
 UART ──────────────┤  Parser/Router              │
                    │    │                        │
                    │    ├── KEY: → uinput        │
                    │    ├── AI:  → ai_handler    │──→ Claude API
                    │    ├── AIV: → vision_handler│──→ libcamera + Claude Vision
                    │    ├── CAM: → cam_handler   │──→ libcamera
                    │    ├── SYS: → sys_handler   │──→ system calls
                    │    └── FILE:→ file_handler  │──→ filesystem
                    │                             │
                    │  Response TX                │
 UART ←─────────────┤                             │
                    └─────────────────────────────┘
```

**Location:** `pi-linux/nwpid/`

---

## Calculator App Architecture

App lives in the Upsilon-RPI fork: `github.com/elektricM/Upsilon-RPI` (branch: `rpi-integration`)

```
┌─────────────────────────────────────────┐
│            Upsilon Apps                 │
├─────────────────────────────────────────┤
│  RPi App    │  AI App   │  Camera App   │
│  (existing) │  (new)    │  (new)        │
├─────────────┴───────────┴───────────────┤
│           NWPI Protocol Library         │
│  - nwpi_send(cmd, payload)              │
│  - nwpi_receive() → (cmd, payload)      │
│  - nwpi_query(cmd, payload) → response  │
├─────────────────────────────────────────┤
│              UART Driver                │
└─────────────────────────────────────────┘
```

**Submodule:** `firmware/` → `Upsilon-RPI.git`

---

## Example Flows

### AI Query (Text Only)
```
Calculator                    Pi
    │                          │
    │  AI:What is Pi?\n        │
    │─────────────────────────>│
    │                          │──→ Claude API
    │                          │<── response
    │  AIR:Pi is 3.14159...\n  │
    │<─────────────────────────│
    │                          │
```

### AI Vision Query (Auto-Snap)
```
Calculator                    Pi
    │                          │
    │  AIV:Solve this\n        │
    │─────────────────────────>│
    │                          │──→ libcamera (snap)
    │                          │──→ Claude Vision API
    │                          │    (image + text)
    │                          │<── response
    │  AIR:The answer is 42\n  │
    │<─────────────────────────│
    │                          │
```

### Take Photo (For Later Use)
```
Calculator                    Pi
    │                          │
    │  CAM:SNAP\n              │
    │─────────────────────────>│
    │                          │──→ libcamera
    │                          │    save photo
    │  OK:CAM:img_001.jpg\n    │
    │<─────────────────────────│
    │                          │
```

### System Shutdown
```
Calculator                    Pi
    │                          │
    │  SYS:SHUT\n              │
    │─────────────────────────>│
    │  OK:SYS:bye\n            │
    │<─────────────────────────│
    │                          │──→ shutdown -h now
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-02-27 | Initial protocol design |
| 1.1 | 2026-10-13 | `DSP` display bridge commands, `SYS:DFU` remote reflash |
| 1.2 | 2026-10-13 | Multi-rectangle updates, 40 µs spun header gap, `late` and bad-header dump in `DSP:STAT`, bridge re-init and repaint on app entry |

---

## Future Considerations

1. **Compression**: For large file transfers, add optional compression
2. **Encryption**: If sensitive data, add optional encryption layer
3. **Multiplexing**: Allow multiple concurrent requests with request IDs
4. **Notifications**: Pi-initiated messages (e.g., "battery low")
5. **Display streaming**: Send images to calc display (very slow but possible)
