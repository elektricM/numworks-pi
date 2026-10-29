# NumWorks AI Assistant - Project Plan

## Overview
Add an AI assistant app to Upsilon that communicates with the Pi via UART, allowing natural language queries and responses displayed on the calculator.

---

## 1. Hardware Changes

### Required
- **Add wire**: Pi GPIO 14 (TX, Pin 8) → Calculator UART RX pin
- **Verify ground**: Shared GND between Pi and calc (likely already connected)

### To Investigate
- [ ] Identify calculator's UART RX pin (check STM32F412 pinout in firmware)
- [ ] Check if existing UART is 3.3V (should be, both are 3.3V logic)

---

## 2. Protocol Design

### Approach A: Prefix-Based (Recommended)
Single UART channel, differentiate by message prefix.

| Direction | Prefix | Format | Example |
|-----------|--------|--------|---------|
| Calc → Pi | `:` | Keyboard hex | `:0000000000000001\n` |
| Calc → Pi | `>` | AI query | `>What is 2+2?\n` |
| Pi → Calc | `<` | AI response | `<The answer is 4.\n` |

**Pros**: Simple, backward compatible, one daemon
**Cons**: Need to handle long responses (chunking)

### Approach B: Mode Switching
Send command to switch daemon mode entirely.

```
!MODE:AI\n      → Switch to AI mode (text in/out)
!MODE:KBD\n     → Switch back to keyboard mode
```

**Pros**: Cleaner separation, no prefix on every message
**Cons**: Can't use keyboard while in AI mode

### Approach C: Separate Channel
Use different UART or add I2C for AI.

**Pros**: Complete isolation
**Cons**: More hardware, more complexity

### Recommendation
Start with **Approach A** (prefix-based). Simple to implement, easy to extend.

---

## 3. Calculator Firmware (Upsilon App)

### New App: "Assistant" or "AI"
Location: `firmware/apps/assistant/`

### Components Needed

#### 3.1 UI Layer
- **Text input field**: Full keyboard support, multi-line
- **Response display**: Scrollable text view, word wrap
- **Status indicator**: "Thinking...", "Connected", "Error"
- **History** (optional): Previous Q&A pairs

#### 3.2 UART Communication
- **TX function**: Send query with `>` prefix
- **RX handler**: Listen for `<` prefixed responses
- **Chunking**: Handle long responses (>256 bytes?)
- **Timeout**: Handle no-response scenario

#### 3.3 App States
```
IDLE        → User can type
SENDING     → Query sent, waiting
SNAPPING    → Taking photo (for AIV)
RECEIVING   → Getting response chunks
DISPLAYING  → Showing response
ERROR       → Connection/timeout issue
```

#### 3.4 Vision Mode
- Button/shortcut to enable "vision mode" (uses AIV instead of AI)
- Shows camera icon or indicator when enabled
- Photo is taken automatically when query is sent
- No image preview needed (photo stays on Pi)

### Approach Options for UI

#### Option 1: Simple Chat
Single question → single answer, clear between queries.
Easiest to implement.

#### Option 2: Chat History
Keep conversation history, scroll through.
More complex, memory considerations.

#### Option 3: Markdown Rendering
Parse basic markdown (bold, code, lists).
Nice to have, not essential for v1.

### Recommendation
Start with **Option 1** (simple chat). Add history in v2.

---

## 4. Pi-Side Daemon: nwpid

### Replace: `uinput-serial-keyboard` → `nwpid` (NumWorks Pi Daemon)

### Components Needed

#### 4.1 Message Router
```c
if (line[0] == ':') {
    handle_keyboard(line);
} else if (line[0] == '>') {
    handle_ai_query(line + 1);
}
```

#### 4.2 AI Backend Interface
- HTTP client to call AI API
- Parse response
- Send back over UART with `<` prefix

#### 4.3 Response Chunking
Long responses need to be split:
```
<chunk1...\n
<chunk2...\n
<END\n
```
Or length-prefixed:
```
<L:1234\n     → 1234 bytes coming
<...data...
```

### Architecture Decision

**Chosen approach:** C daemon (`nwpid`) + Python helper for AI

The `nwpid` daemon handles:
- UART read/write loop
- Message parsing and routing  
- Keyboard → uinput (fast, stays in C)
- System commands (C, direct syscalls)

For AI queries, daemon calls Python helper:
```c
// In ai.c
FILE *fp = popen("python3 /opt/nwpid/ai/query.py 'user question'", "r");
// Read response, send back over UART
```

**Why this approach:**
- Keyboard stays fast (pure C, no interpreter)
- AI code is easy to write/modify (Python + anthropic SDK)
- Single daemon owns the serial port
- Easy to add new handlers later

---

## 5. AI Backend Options

### Option A: Cloud API (Recommended for v1)
- Claude API, OpenAI API, etc.
- Requires internet on Pi
- Simple HTTP POST, get response

**Text query:**
```python
import anthropic
client = anthropic.Anthropic(api_key="...")
response = client.messages.create(
    model="claude-3-haiku-20240307",
    max_tokens=500,
    messages=[{"role": "user", "content": query}]
)
print(response.content[0].text)
```

**Vision query (AIV command):**
```python
import anthropic
import base64

# Snap photo first
os.system("libcamera-still -o /tmp/query.jpg --width 1280 --height 720 -t 1")

with open("/tmp/query.jpg", "rb") as f:
    image_data = base64.standard_b64encode(f.read()).decode("utf-8")

response = client.messages.create(
    model="claude-3-haiku-20240307",
    max_tokens=500,
    messages=[{
        "role": "user",
        "content": [
            {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": image_data}},
            {"type": "text", "text": query}
        ]
    }]
)
print(response.content[0].text)
```

### Option B: Local LLM
- Ollama, llama.cpp on Pi
- No internet needed
- Pi Zero 2W is weak (1GB RAM) - very limited models

### Option C: Proxy to Home Server
- Pi forwards to your home server running larger model
- Best of both: local network, better model

### Recommendation
Start with **Option A** (Claude Haiku - fast, cheap).
Add Option C later if you want offline capability.

---

## 6. Configuration

### Pi-Side Config File
`/etc/nwai.conf` or `~/.config/nwai/config.json`

```json
{
  "api": "anthropic",
  "api_key": "sk-...",
  "model": "claude-3-haiku-20240307",
  "max_tokens": 500,
  "system_prompt": "You are a helpful calculator assistant. Keep responses brief.",
  "timeout_seconds": 30
}
```

### System Prompt Considerations
- Keep responses short (small screen)
- Math-focused context
- Maybe include calculator capabilities

---

## 7. Error Handling

| Error | Calc Display | Recovery |
|-------|--------------|----------|
| No Pi connection | "Not connected" | Retry button |
| API timeout | "Timeout" | Retry |
| API error | "API error: {code}" | Show error, allow retry |
| No internet | "No internet" | Check connection |
| Response too long | Truncate + "..." | Allow scroll |

---

## 8. Implementation Order

### Phase 1: Hardware + Basic Comms
1. [ ] Identify calc RX pin
2. [ ] Solder Pi TX → Calc RX
3. [ ] Test bidirectional UART (simple echo)

### Phase 2: Protocol + Daemon
4. [ ] Add prefix routing to daemon
5. [ ] Create Python AI query script
6. [ ] Test: send `>hello\n` from calc, get `<response\n` back

### Phase 3: Calculator App (Minimal)
7. [ ] Create basic Upsilon app skeleton
8. [ ] Add text input field
9. [ ] Add UART TX for queries
10. [ ] Add UART RX handler
11. [ ] Display response (single screen, no scroll)

### Phase 4: Polish
12. [ ] Add scrolling for long responses
13. [ ] Add "thinking" indicator
14. [ ] Add error handling
15. [ ] Add conversation history (optional)

### Phase 5: Optimization
16. [ ] Response streaming (show text as it arrives)
17. [ ] Local LLM option
18. [ ] Custom system prompts

---

## 9. Files to Create/Modify

### Calculator (Upsilon-RPI fork)
Repo: `github.com/elektricM/Upsilon-RPI` (branch: `rpi-integration`)
Submodule path: `firmware/`

```
apps/assistant/
├── assistant_app.cpp      # Main app class
├── assistant_app.h
├── assistant_controller.cpp
├── assistant_controller.h
├── assistant_view.cpp     # UI rendering
├── assistant_view.h
└── Makefile.app

apps/shared/nwpi/          # Shared protocol library
├── nwpi_protocol.cpp
├── nwpi_protocol.h
└── uart_handler.cpp
```

### Pi (nwpid)
Location: `pi-linux/nwpid/`

```
pi-linux/nwpid/
├── nwpid.c                # Main daemon (replaces uinput.c)
├── handlers/
│   ├── keyboard.c         # KEY: handler (from old uinput)
│   ├── ai.c               # AI:/AIV:/AIA: handlers
│   ├── camera.c           # CAM: handler
│   └── system.c           # SYS: handler
├── ai/
│   ├── query.py           # AI query script (called by ai.c)
│   └── config.json        # API keys, model config
├── nwpid.service          # systemd service file
├── Makefile
└── install.sh
```

---

## 10. Testing Plan

### Unit Tests
- [ ] UART echo test (calc sends, Pi echoes back)
- [ ] Prefix routing test (`:` vs `>`)
- [ ] API query test (Pi only, no calc)

### Integration Tests
- [ ] Full loop: type on calc → see response
- [ ] Long response handling
- [ ] Timeout handling
- [ ] Rapid queries (don't crash)

### Edge Cases
- [ ] Empty query
- [ ] Very long query (>256 chars)
- [ ] Special characters in query
- [ ] API returns error
- [ ] Network drops mid-response

---

## 11. Open Questions

1. **Max response length?** What fits reasonably on 320x240?
2. **Conversation context?** Send previous Q&A for context or stateless?
3. **Keyboard shortcut?** Quick way to open AI app from Pi desktop?
4. **Power consideration?** Does API call drain battery faster?

---

## Notes

- Keep v1 simple: one question, one answer, no history
- Use Haiku for speed (important on slow UART display)
- System prompt should encourage brief responses
- Consider character limit hint in prompt: "Keep responses under 500 characters"
