# M5Claw AGENTS.md

## Memory

Project-specific memory is at `.claude/projects/<project>/memory/`. See MEMORY.md for the index.

Key constraints when modifying this codebase:
- [[esp32-memory-constraints]] — ESP32-S3 heap limits, PSRAM unreliability, buffer allocation strategy
- [[tts-response-format]] — OpenAI-compatible TTS format rules
- [[tts-wav-playback-order]] — Content-Type-based playback method ordering
- [[stt-key-fallback]] — API key fallback chain for STT
- [[http-debug-logging]] — Required logging for HTTP features

## Build

`python3 flash.py` — interactive config → cache → erase → build → upload (PlatformIO via `pio run`)

## Architecture

- `src/main.cpp` — boot, WiFi, setup/companion/chat modes, serial CLI, NVS protocol
- `src/llm_client.cpp` — LLM/TTS/STT HTTP clients, provider registry, audio playback
- `src/agent.cpp` — AI agent loop, tool calling, session management
- `src/context_builder.cpp` — system prompt assembly from SOUL.md, memory, skills
- `src/config.cpp` — NVS-backed persistent config (Preferences library)
- `src/m5claw_config.h` — all build-time defaults and limits
- `flash.py` — interactive flash tool
- `load_config.py` — PlatformIO build script, injects `-D` flags from env/ini
