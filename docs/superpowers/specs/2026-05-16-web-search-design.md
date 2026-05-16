# Web Search Tool Design

## Summary

Add a `web_search` function tool using Bocha Search API, available to all LLM providers.

## Configuration

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `bocha_api_key` | Yes | — | Bocha API key from bochaai.com |
| `bocha_search_endpoint` | No | `api.bochaai.com` | Bocha API host |

The API key is stored encrypted in NVS (same `enc:v1:` scheme as LLM/TTS/STT keys).

## Files changed

| File | Change |
|------|--------|
| `m5claw_config.h` | Add `M5CLAW_BOCHA_HOST`, `M5CLAW_BOCHA_SEARCH_PATH` |
| `config.h` | Add `getBochaApiKey()`, `setBochaApiKey()` |
| `config.cpp` | NVS read/write for `bocha_key`, bootstrap import |
| `tool_registry.cpp` | Add `web_search` tool definition and `tool_web_search` handler |
| `user_config.ini.example` | Add `bocha_api_key` field |

## Data flow

```
LLM calls: web_search({"query": "...", "count": 5})
  → tool_web_search()
  → HTTPS POST https://api.bochaai.com/api/ai/search
    Header: Authorization: Bearer {bocha_api_key}
    Body: {"query":"...","count":5}
  → Parse response JSON
  → Format as compact result array → return to LLM
```

Each result: `{"title","url","snippet"}`. Max 5 results returned.

## Memory constraints

- Body buffer sized from Content-Length header, not fixed 256KB
- Response trimmed to 8KB max to stay within tool output limits
- Uses existing `WiFiClientSecure` and JSON streaming where possible

## Tool schema

```json
{
  "name": "web_search",
  "description": "Search the web using Bocha search engine. Returns title, URL, and snippet for each result.",
  "input_schema": {
    "type": "object",
    "properties": {
      "query": {"type": "string", "description": "Search query"},
      "count": {"type": "integer", "description": "Number of results (1-10, default 5)"}
    },
    "required": ["query"]
  }
}
```
