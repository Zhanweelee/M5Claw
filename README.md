# M5Claw

AI companion firmware for M5Stack Cardputer. A self-contained device agent built on ESP32-S3 + SPIFFS + PlatformIO that provides local companion UI, keyboard chat, voice input, TTS playback, WeChat integration, weather, cron jobs, persistent memory, and a skill system.

Designed to be flashed once and run long-term — not a one-shot demo. Ships with firmware code, SPIFFS data, and a one-click flash tool.

## Features

- **Companion screen** — time, battery, weather with dynamic day/night scenes
- **Keyboard chat** — type queries, stream AI replies token-by-token
- **Voice input** — hold `Fn` to record, release to send audio for transcription & response. Uses standalone STT provider (SiliconFlow) with API key fallback to LLM key when no explicit STT key is configured
- **TTS playback** — AI replies from voice input are spoken aloud via standalone TTS provider (SiliconFlow). Audio streams directly to SPIFFS to avoid heap fragmentation on ESP32; WAV header is parsed for correct sample rate/channels before PCM playback
- **WeChat bridge** — iLink-protocol bot: send/receive messages, QR pairing, proactive push
- **Image handling** — WeChat images are downloaded and sent as multimodal input to the model
- **Persistent memory & persona** — `SOUL.md`, `USER.md`, `MEMORY.md` define personality, user profile, and long-term memory
- **Skill system** — auto-loads `data/skills/*.md` as extra prompt skills at boot
- **Cron service** — periodic and one-shot jobs, results pushed to local or WeChat
- **Heartbeat** — periodically reads `HEARTBEAT.md`, triggers agent when pending items found
- **Weather** — Open-Meteo geocoding and real-time weather
- **Serial CLI** — on-device config, also compatible with M5Burner NVS protocol
- **Multi-provider LLM** — Xiaomi MiMo, DeepSeek, OpenAI, or custom OpenAI-compatible API

## Hardware

| Component | Detail |
|-----------|--------|
| Device | M5Stack Cardputer |
| MCU | ESP32-S3 |
| Framework | Arduino on PlatformIO |
| Storage | SPIFFS |
| Connectivity | 2.4 GHz Wi-Fi |

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    M5Claw                        │
│  ┌─────────┐  ┌──────────┐  ┌────────────────┐ │
│  │ Companion│  │  Chat    │  │  WeChat Status  │ │
│  │  (idle)  │  │ (key/voz)│  │   (QR/pair)    │ │
│  └────┬─────┘  └────┬─────┘  └───────┬────────┘ │
│       │              │               │          │
│  ┌────┴──────────────┴───────────────┴────────┐ │
│  │              Agent Loop                    │ │
│  │  system prompt → tools → LLM → response    │ │
│  └──────────────┬─────────────────────────────┘ │
│       │         │         │                     │
│  ┌────┴──┐ ┌────┴──┐ ┌───┴──────┐              │
│  │Tools  │ │Memory │ │Session   │              │
│  │(10+)  │ │Store  │ │Manager   │              │
│  └───────┘ └───────┘ └──────────┘              │
│                                                 │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐     │
│  │LLM Client│  │TTS/STT   │  │WeChat Bot │     │
│  │(multi-   │  │Client    │  │(iLink)    │     │
│  │provider) │  │(provider │  │           │     │
│  └──────────┘  │ aware)   │  └───────────┘     │
│                └──────────┘                    │
│                                                 │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐     │
│  │Cron      │  │Heartbeat │  │Weather    │     │
│  │Service   │  │          │  │Client     │     │
│  └──────────┘  └──────────┘  └───────────┘     │
└─────────────────────────────────────────────────┘
```

### Key libraries

- `M5Cardputer` — display, keyboard, speaker, battery
- `ArduinoJson` — JSON serialization for LLM protocol and config
- `WiFiClientSecure` — TLS 1.2 via ESP-IDF mbedTLS

### LLM provider system

Providers are defined in a static table (`src/llm_client.cpp`). Each entry specifies host, path, default model, TTS capability, and web-search support. Adding a new provider requires only a new row in the table — no other code changes.

Requests use OpenAI-compatible JSON format. Responses are parsed as SSE streams or JSON, depending on `Accept` / `Content-Type`.

### Memory layout (SPIFFS)

```
/spiffs
├── config/
│   ├── SOUL.md           persona & values
│   ├── USER.md           user profile
│   └── BOOTSTRAP.json    one-time bootstrap (auto-deleted after import)
├── memory/
│   └── MEMORY.md         long-term memory
├── skills/
│   └── *.md              skill prompt fragments
├── sessions/
│   └── *.jsonl           per-session message history
├── cron.json             scheduled jobs
├── HEARTBEAT.md          heartbeat task source
└── tmp_*                 temporary voice & image files
```

## Repository structure

```
.
├── src/                  Firmware source
│   ├── main.cpp          Entry point, UI, setup flow, serial CLI
│   ├── llm_client.*      Multi-provider LLM, TTS & STT client
│   ├── agent.*           Tool-calling agent loop
│   ├── config.*          NVS-backed configuration
│   ├── tool_registry.*   Built-in tool implementations
│   ├── wechat_bot.*      iLink WeChat protocol
│   ├── cron_service.*    Periodic / one-shot job scheduler
│   ├── weather_client.*  Open-Meteo weather
│   ├── memory_store.*    Persistent memory files
│   ├── session_mgr.*     Conversation history
│   ├── context_builder.* System prompt assembly
│   ├── skill_loader.*    Skills from SPIFFS
│   ├── companion.*       Idle screen UI
│   ├── chat.*            Chat screen UI
│   ├── message_bus.*     Inter-component message bus
│   ├── tls_utils.*       TLS configuration
│   ├── utils.*           Color/display helpers
│   └── m5claw_config.h   All compile-time constants & provider defs
├── data/                 SPIFFS data
│   ├── cert/             TLS certificate bundle
│   ├── config/           Persona, user profile
│   ├── memory/           Long-term memory
│   └── skills/           Built-in skills
├── flash.py              One-click flash tool with config caching
├── load_config.py        Build-time config injection (SCons script)
├── platformio.ini        PlatformIO build configuration
├── partitions.csv        Partition table
└── user_config.ini.example  Local config template
```

## Quick start

### Prerequisites

- M5Stack Cardputer
- Python 3
- PlatformIO CLI (`pio`) or VS Code + PlatformIO extension
- 2.4 GHz Wi-Fi network
- API key for at least one supported LLM provider

### Flash

```bash
python flash.py
```

The script will interactively prompt for:
- Serial port
- Wi-Fi credentials (primary + optional backup)
- LLM provider (MiMo / DeepSeek / OpenAI / Custom)
- API key and model
- Assistant name (displayed in system prompt)
- TTS provider, key, model, voice (standalone TTS, e.g. SiliconFlow)
- STT provider, key, model (standalone STT, e.g. SiliconFlow)
- City (for weather)

Config is cached to `.flash_cache.json` (gitignored) for reuse on the next flash.

To skip the interactive prompts and use `user_config.ini` instead:

```bash
cp user_config.ini.example user_config.ini
# edit user_config.ini with your values
pio run
pio run -t upload --upload-port COMx
pio run -t uploadfs --upload-port COMx
```

### Monitor serial output

```bash
pio device monitor -b 115200
```

### If upload fails

Put the device in download mode: hold **G0**, press **RST**, release **G0**, then retry.

## First boot

The device initializes in this order:

1. Mount SPIFFS
2. Load NVS config
3. Import bootstrap config (if present)
4. Apply build-time defaults
5. Init memory, skills, tools, WeChat, cron, heartbeat, agent
6. If config is complete → connect Wi-Fi automatically
7. Otherwise → enter on-device Setup

### Setup flow

Navigate with the keyboard:

| Step | Field | Notes |
|------|-------|-------|
| 1 | Wi-Fi SSID | |
| 2 | Wi-Fi Password | Masked input |
| 3 | Provider | type `mimo`, `deepseek`, `openai`, or `custom` |
| 4 | API Key | Skipped if baked-in key exists |
| 5 | Model | Pre-filled from provider default |
| 6 | City | Default: Beijing |

- **Enter** — confirm
- **Del** — backspace
- **Tab** — skip to offline mode

## Controls

### Companion screen

| Key | Action |
|-----|--------|
| `Tab` | Enter chat |
| `Ctrl` | WeChat status |
| `Fn` (tap) | Toggle day/night scene |
| `Fn + R` | Reset Wi-Fi, re-enter setup |

### Chat screen

| Key | Action |
|-----|--------|
| Keyboard | Type message |
| `Enter` | Send |
| `Del` | Delete character |
| `Tab` | Scroll up history |
| `Ctrl` | Scroll down; return to Companion if at bottom |
| `Alt` | Return to Companion |
| `Fn` (hold) | Record voice |
| `Fn` (release) | Send voice |
| `Fn + C` | Cancel current generation |

### WeChat status screen

| Key | Action |
|-----|--------|
| `Ctrl` | Enter from Companion |
| `Tab` | Return to Companion |
| `Enter` | Retry pairing |

### Offline / Wi-Fi failed screen

| Key | Action |
|-----|--------|
| `Enter` | Retry connection |
| `Fn + R` | Reset Wi-Fi credentials |
| `Tab` | Enter offline mode |

## WeChat integration

Built-in iLink protocol bridge:

- QR code pairing to obtain `bot_token`
- Alternatively, set `token` and `host` via serial CLI
- Incoming text messages are routed to the agent
- Incoming images are downloaded to SPIFFS and sent as multimodal input
- Model replies are automatically sent back to the user
- The model can proactively message users via the `wechat_send` tool

WeChat polling pauses during voice recording and model calls to avoid memory contention.

## Built-in tools

The agent can invoke these device-side tools:

- `get_current_time`
- `read_file`
- `write_file`
- `edit_file`
- `list_dir`
- `cron_add` / `cron_list` / `cron_remove`
- `wechat_send`

Web search is enabled for providers that support it (MiMo).

## Serial CLI

Available in the serial monitor (`pio device monitor -b 115200`):

```
help                          Show available commands
set_wifi <ssid> <pass>        Set primary Wi-Fi
set_wifi2 <ssid> <pass>       Set backup Wi-Fi
set_provider <id>             Set LLM provider (mimo/deepseek/openai/custom)
set_llm_key <key>             Set API key
set_llm_model <model>         Set model name
set_assistant_name <name>     Set assistant display name
set_tts_provider <id>         Set TTS provider (siliconflow)
set_tts_key <key>             Set TTS API key
set_tts_model <model>         Set TTS model override
set_tts_voice <voice>         Set TTS voice override
set_stt_provider <id>         Set STT provider (siliconflow)
set_stt_key <key>             Set STT API key
set_stt_model <model>         Set STT model override
set_city <city>               Set city for weather
set_wechat <token> <host>     Set WeChat credentials
show_config                   Display current config
list_providers                List supported providers
reset_config                  Clear all config
reboot                        Restart device
```

The device also responds to M5Burner-compatible `CMD::GET` / `CMD::SET` / `CMD::LIST` / `CMD::INIT` protocol for GUI-based config tools.

## Adding a provider

Add a row to the `kProviders` table in `src/llm_client.cpp`:

```cpp
{
    "myprovider", "My Provider",
    "api.myprovider.com", "/v1/chat/completions", "my-model-v1",
    false, nullptr, nullptr, nullptr, 0,   // no TTS
    false, 0, 0                             // no web search
},
```

Then add the corresponding defines in `src/m5claw_config.h` and wire up a build-time key macro in `load_config.py` if you want `flash.py` support.

## Configuration

| File | Purpose |
|------|---------|
| `SOUL.md` | Assistant personality, tone, boundaries |
| `USER.md` | User name, language, timezone, location |
| `MEMORY.md` | Long-term facts, preferences, plans |
| `skills/*.md` | Task-specific instructions (daily brief, weather, reminders, etc.) |

**Note:** After editing any file under `data/`, re-upload SPIFFS:

```bash
pio run -t uploadfs --upload-port COMx
```

### Build-time secrets

Copy `user_config.ini.example` → `user_config.ini` (gitignored). Values are compiled into firmware but **not persisted** to device NVS. API keys baked at build time are marked transient: they work immediately but won't survive a factory reset.

## Design notes

- **Local replies are kept short** — the system prompt instructs the model to be concise on the local display channel. WeChat replies can be longer.
- **Offline mode** — the Companion screen and keyboard work without Wi-Fi, but AI, weather, WeChat, and TTS require connectivity.
- **Memory is constrained** — the agent uses SPIFFS-backed swap for conversation history and PSRAM where available. Media data URIs are capped at 3 MB in-request.
- **Wi-Fi fallback** — if primary Wi-Fi fails, the device tries the backup network (`ssid2`/`pass2`) before showing the failure screen.

## License

GPL-3.0 — see [LICENSE](LICENSE).
