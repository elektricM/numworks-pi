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

---

## Future Considerations

1. **Compression**: For large file transfers, add optional compression
2. **Encryption**: If sensitive data, add optional encryption layer
3. **Multiplexing**: Allow multiple concurrent requests with request IDs
4. **Notifications**: Pi-initiated messages (e.g., "battery low")
5. **Display streaming**: Send images to calc display (very slow but possible)
