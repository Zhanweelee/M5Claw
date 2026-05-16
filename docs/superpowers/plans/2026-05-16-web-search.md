# Web Search Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `web_search` function tool backed by Bocha Search API, available to all LLM providers.

**Architecture:** Add bocha_api_key to Config (encrypted NVS), register a new `web_search` tool in ToolRegistry that makes an HTTPS POST to `api.bochaai.com/v1/web-search`, parses the JSON response, and returns formatted search results to the LLM.

**Tech Stack:** ESP32 Arduino, ArduinoJson, WiFiClientSecure, Preferences/NVS

---

### Task 1: Config constants and defaults

**Files:**
- Modify: `src/m5claw_config.h` (add after line 54, after SiliconFlow TTS defaults)

- [ ] **Step 1: Add Bocha config constants to m5claw_config.h**

```cpp
// Bocha Web Search
#define M5CLAW_BOCHA_HOST              "api.bochaai.com"
#define M5CLAW_BOCHA_SEARCH_PATH       "/v1/web-search"
#define M5CLAW_BOCHA_DEFAULT_COUNT     5
#define M5CLAW_BOCHA_MAX_COUNT         10
```

- [ ] **Step 2: Build verify**

```bash
python3 flash.py
# Select option to build only (or verify with pio run)
```

Expected: Compiles cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/m5claw_config.h
git commit -m "feat(config): add Bocha web search endpoint defaults"
```

---

### Task 2: Config getter/setter and NVS storage

**Files:**
- Modify: `src/config.h:65` (inside `Config` namespace, before closing brace)
- Modify: `src/config.cpp` (multiple locations)

- [ ] **Step 1: Add bochaApiKey field and getter/setter declarations to config.h**

After `setTransientLlmApiKey`, add:

```cpp
    const String& getBochaApiKey();
    void setBochaApiKey(const String& key);
```

After `static bool llmApiKeyTransient = false;` in the anonymous namespace block of config.cpp, there will be a corresponding static string. The config.h changes are:

```cpp
// Inside Config namespace, after line 62 (setTransientLlmApiKey)
    const String& getBochaApiKey();
    void setBochaApiKey(const String& key);
```

- [ ] **Step 2: Add static variable in config.cpp**

After `static bool muteTts = false;` (line 97), add:

```cpp
static String bochaApiKey;
```

- [ ] **Step 3: Add NVS load in Config::load()**

After `muteTts = prefs.getBool("mute_tts", false);` (line 135), add:

```cpp
    bochaApiKey     = readSecret("bocha_key");
```

- [ ] **Step 4: Add NVS save in Config::save()**

After `prefs.putBool("mute_tts", muteTts);` (line 162), add:

```cpp
    writeSecret("bocha_key",      bochaApiKey);
```

- [ ] **Step 5: Add reset in Config::reset()**

After `muteTts = false;` (line 176), add:

```cpp
    bochaApiKey = "";
```

- [ ] **Step 6: Add bootstrap import in Config::importBootstrapFile()**

After `changed += applyBootstrapValue(doc["wechat_api_host"], wechatApiHost) ? 1 : 0;` (line 203), add:

```cpp
    changed += applyBootstrapSecretValue(doc["bocha_api_key"], bochaApiKey, nullptr) ? 1 : 0;
```

- [ ] **Step 7: Add getter/setter implementations at end of config.cpp**

Before the last line (closing brace of Config namespace), add:

```cpp
const String& Config::getBochaApiKey()       { return bochaApiKey; }
void Config::setBochaApiKey(const String& k) { bochaApiKey = k; }
```

- [ ] **Step 8: Build verify**

```bash
python3 flash.py
# Build only
```

Expected: Compiles cleanly.

- [ ] **Step 9: Commit**

```bash
git add src/config.h src/config.cpp
git commit -m "feat(config): add Bocha API key storage with NVS encryption"
```

---

### Task 3: web_search tool implementation in ToolRegistry

**Files:**
- Modify: `src/tool_registry.cpp` (add after line 437, before the Tool registry section)

- [ ] **Step 1: Add includes needed for HTTP and Config**

At top of tool_registry.cpp, add after `#include "tool_registry.h"`:

```cpp
#include "config.h"
```

After `#include <SPIFFS.h>`, add:

```cpp
#include <WiFiClientSecure.h>
```

- [ ] **Step 2: Add tool_web_search handler function**

Add after `tool_wechat_send` function (after line 437) and before the `/* ── Tool registry ── */` comment:

```cpp
/* ── web_search ───────────────────────────────────── */
static bool tool_web_search(const char* input, char* output, size_t sz) {
    const String& apiKey = Config::getBochaApiKey();
    if (apiKey.length() == 0) {
        strlcpy(output, "Bocha API key not configured. Set bocha_api_key first.", sz);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, input)) { strlcpy(output, "Invalid JSON", sz); return false; }
    const char* query = doc["query"] | "";
    if (!query[0]) { strlcpy(output, "Missing query", sz); return false; }
    int count = doc["count"] | M5CLAW_BOCHA_DEFAULT_COUNT;
    if (count < 1) count = 1;
    if (count > M5CLAW_BOCHA_MAX_COUNT) count = M5CLAW_BOCHA_MAX_COUNT;

    // Build request body
    JsonDocument reqDoc;
    reqDoc["query"] = query;
    reqDoc["count"] = count;

    String reqBody;
    serializeJson(reqDoc, reqBody);

    WiFiClientSecure client;
    client.setTimeout(15);
    client.setInsecure();

    if (!client.connect(M5CLAW_BOCHA_HOST, 443)) {
        strlcpy(output, "Bocha API connection failed", sz);
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", M5CLAW_BOCHA_SEARCH_PATH);
    client.printf("Host: %s\r\n", M5CLAW_BOCHA_HOST);
    client.print("Authorization: Bearer ");
    client.println(apiKey);
    client.print("Content-Type: application/json\r\n");
    client.printf("Content-Length: %d\r\n", reqBody.length());
    client.print("\r\n");
    client.print(reqBody);

    // Read status line
    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    int httpCode = 0;
    if (statusLine.indexOf("200") < 0) {
        snprintf(output, sz, "Bocha API HTTP error: %s", statusLine.c_str());
        client.stop();
        return false;
    }

    // Skip headers
    String headers;
    String contentLength;
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r" || line == "") break;
        if (line.startsWith("Content-Length:")) {
            contentLength = line.substring(15);
            contentLength.trim();
        }
        headers += line;
    }

    // Read body
    size_t bodyLen = contentLength.length() > 0 ? contentLength.toInt() : 8192;
    if (bodyLen < 4) bodyLen = 4;
    if (bodyLen > 8192) bodyLen = 8192;
    char* bodyBuf = new char[bodyLen + 1];
    if (!bodyBuf) {
        strlcpy(output, "Memory allocation failed for search response", sz);
        client.stop();
        return false;
    }

    size_t readBytes = 0;
    unsigned long start = millis();
    while (client.connected() && readBytes < bodyLen && (millis() - start) < 10000) {
        if (client.available()) {
            bodyBuf[readBytes++] = client.read();
        }
    }
    bodyBuf[readBytes] = '\0';
    client.stop();

    // Parse response
    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, bodyBuf);
    delete[] bodyBuf;

    if (err) {
        strlcpy(output, "Failed to parse search response", sz);
        return false;
    }

    // Extract webPages array
    JsonArray webPages = respDoc["data"]["webPages"].as<JsonArray>();
    if (!webPages || webPages.size() == 0) {
        strlcpy(output, "No search results found", sz);
        return true;
    }

    // Format results for LLM
    size_t off = 0;
    int num = 0;
    for (JsonVariant page : webPages) {
        const char* title = page["title"] | "";
        const char* url = page["url"] | "";
        const char* snippet = page["snippet"] | page["summary"] | "";

        int w = snprintf(output + off, sz - off,
            "[%d] %s\n  URL: %s\n  %s\n\n",
            num + 1, title, url, snippet);
        if (w > 0) {
            off += w;
            num++;
        }
        if (off >= sz - 200) break;
    }
    output[off] = '\0';
    return true;
}
```

- [ ] **Step 3: Add tool definition to s_tools array**

Add after the `wechat_send` tool entry (after line 494):

```cpp
    {
        "web_search",
        "Search the web using Bocha search engine. Returns title, URL, and snippet for each result.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"},\"count\":{\"type\":\"integer\",\"description\":\"Number of results, 1-10, default 5\"}},\"required\":[\"query\"]}",
        tool_web_search
    },
```

- [ ] **Step 4: Build verify**

```bash
python3 flash.py
# Build only
```

Expected: Compiles cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/tool_registry.cpp
git commit -m "feat(tool): add web_search tool using Bocha Search API"
```

---

### Task 4: Add bocha_api_key to user_config.ini.example

**Files:**
- Modify: `src/user_config.ini.example` — or the root `user_config.ini.example`

Actually the example is at the repo root. Let me use the correct path.

- [ ] **Step 1: Add bocha_api_key to example config**

In `user_config.ini.example`, after `openai_api_key =` (line 24), add:

```ini
; Web search (Bocha AI — get key at https://open.bochaai.com)
bocha_api_key =
```

- [ ] **Step 2: Commit**

```bash
git add user_config.ini.example
git commit -m "feat(config): add bocha_api_key to example config template"
```

---

### Task 5: Build and verify full integration

- [ ] **Step 1: Clean build**

```bash
pio run
```

Expected: Compiles with no errors.

- [ ] **Step 2: Verify tool registration log**

Check that the serial output includes `[TOOLS] Registered 10 tools` (was 9, now +1 for web_search).

- [ ] **Step 3: Commit any remaining changes**

```bash
git status
# Commit if anything outstanding
```
