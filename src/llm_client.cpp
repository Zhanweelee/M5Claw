#include "llm_client.h"
#include "m5claw_config.h"
#include "tls_utils.h"
#include <M5Cardputer.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

static char s_api_key[320] = {0};
static char s_model[64] = M5CLAW_LLM_DEFAULT_MODEL;
static char s_custom_host[128] = {0};
static char s_custom_path[128] = {0};
static const LlmProviderInfo* s_provider = nullptr;

static volatile bool* s_abort_flag = nullptr;
static bool is_aborted() { return s_abort_flag && *s_abort_flag; }

static LlmPreReadFreeFn s_pre_read_free_fn = nullptr;

static const LlmProviderInfo kProviders[] = {
    {
        M5CLAW_PROVIDER_MIMO, "Xiaomi MiMo",
        M5CLAW_MIMO_HOST, M5CLAW_MIMO_CHAT_PATH, M5CLAW_MIMO_MODEL,
        true, M5CLAW_MIMO_TTS_PATH, M5CLAW_MIMO_TTS_MODEL, M5CLAW_MIMO_TTS_VOICE, M5CLAW_MIMO_TTS_SAMPLE_RATE,
        true, M5CLAW_MIMO_SEARCH_MAX_KEYWORD, M5CLAW_MIMO_SEARCH_LIMIT,
        true, nullptr, nullptr
    },
    {
        M5CLAW_PROVIDER_DEEPSEEK, "DeepSeek",
        M5CLAW_DEEPSEEK_HOST, M5CLAW_DEEPSEEK_CHAT_PATH, M5CLAW_DEEPSEEK_MODEL,
        false, nullptr, nullptr, nullptr, 0,
        false, 0, 0,
        false, nullptr, nullptr
    },
    {
        M5CLAW_PROVIDER_OPENAI, "OpenAI",
        M5CLAW_OPENAI_HOST, M5CLAW_OPENAI_CHAT_PATH, M5CLAW_OPENAI_MODEL,
        false, nullptr, nullptr, nullptr, 0,
        false, 0, 0,
        false, nullptr, nullptr
    },
    {
        M5CLAW_PROVIDER_ANTHROPIC, "Anthropic",
        M5CLAW_ANTHROPIC_HOST, M5CLAW_ANTHROPIC_CHAT_PATH, M5CLAW_ANTHROPIC_MODEL,
        false, nullptr, nullptr, nullptr, 0,
        false, 0, 0,
        false, "x-api-key", "anthropic"
    },
    {
        M5CLAW_PROVIDER_CUSTOM, "Custom",
        "", "/v1/chat/completions", "",
        false, nullptr, nullptr, nullptr, 0,
        false, 0, 0,
        false, nullptr, nullptr
    },
};
static constexpr int kProviderCount = sizeof(kProviders) / sizeof(kProviders[0]);

static const TtsProviderInfo kTtsProviders[] = {
    {
        M5CLAW_TTS_PROVIDER_SILICONFLOW, "SiliconFlow",
        M5CLAW_SILICONFLOW_HOST, M5CLAW_SILICONFLOW_TTS_PATH,
        M5CLAW_SILICONFLOW_TTS_MODEL, M5CLAW_SILICONFLOW_TTS_VOICE,
        M5CLAW_SILICONFLOW_TTS_SAMPLE_RATE
    },
};
static constexpr int kTtsProviderCount = sizeof(kTtsProviders) / sizeof(kTtsProviders[0]);

static const SttProviderInfo kSttProviders[] = {
    {
        M5CLAW_STT_PROVIDER_SILICONFLOW, "SiliconFlow",
        M5CLAW_SILICONFLOW_HOST, M5CLAW_SILICONFLOW_STT_PATH,
        M5CLAW_SILICONFLOW_STT_MODEL
    },
};
static constexpr int kSttProviderCount = sizeof(kSttProviders) / sizeof(kSttProviders[0]);

static const SttProviderInfo* s_stt_provider = nullptr;
static char s_stt_api_key[320] = {0};
static char s_stt_model_override[64] = {0};

static const TtsProviderInfo* s_tts_provider = nullptr;
static char s_tts_api_key[320] = {0};
static char s_tts_model_override[64] = {0};
static char s_tts_voice_override[64] = {0};

static const char* kMediaPlaceholderPrefix = "__M5CLAW_MEDIA|";
static const char* kMediaPlaceholderSuffix = "__";
static constexpr int kMaxRequestMediaRefs = 4;
static constexpr size_t kErrorBodyPreviewMax = 4096;

struct RequestMediaRef {
    size_t pos;
    size_t token_len;
    size_t replacement_len;
    char mime[48];
    char path[128];
};

struct HttpResponseMeta {
    int status_code;
    bool chunked;
    int content_length;
    char content_type[64];
};

void llm_client_set_abort_flag(volatile bool* flag) { s_abort_flag = flag; }
void llm_client_set_pre_read_free(LlmPreReadFreeFn fn) { s_pre_read_free_fn = fn; }

int llm_provider_count() { return kProviderCount; }

const LlmProviderInfo* llm_provider_by_index(int idx) {
    if (idx < 0 || idx >= kProviderCount) return nullptr;
    return &kProviders[idx];
}

const LlmProviderInfo* llm_provider_by_id(const char* id) {
    if (!id || !id[0]) return &kProviders[0];
    for (int i = 0; i < kProviderCount; i++) {
        if (strcasecmp(kProviders[i].id, id) == 0) return &kProviders[i];
    }
    return nullptr;
}

const char* llm_current_provider() { return s_provider ? s_provider->id : M5CLAW_DEFAULT_PROVIDER; }
const char* llm_current_model()    { return s_model; }

int tts_provider_count() { return kTtsProviderCount; }

const TtsProviderInfo* tts_provider_by_index(int idx) {
    if (idx < 0 || idx >= kTtsProviderCount) return nullptr;
    return &kTtsProviders[idx];
}

const TtsProviderInfo* tts_provider_by_id(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (int i = 0; i < kTtsProviderCount; i++) {
        if (strcasecmp(kTtsProviders[i].id, id) == 0) return &kTtsProviders[i];
    }
    return nullptr;
}

const char* tts_current_provider() {
    return s_tts_provider ? s_tts_provider->id : "";
}

const char* tts_current_voice() {
    if (s_tts_voice_override[0]) return s_tts_voice_override;
    if (s_tts_provider) return s_tts_provider->tts_voice;
    return "";
}

const char* tts_current_model() {
    if (s_tts_model_override[0]) return s_tts_model_override;
    if (s_tts_provider) return s_tts_provider->tts_model;
    return "";
}

// ── STT provider queries ──

int stt_provider_count() { return kSttProviderCount; }

const SttProviderInfo* stt_provider_by_index(int idx) {
    if (idx < 0 || idx >= kSttProviderCount) return nullptr;
    return &kSttProviders[idx];
}

const SttProviderInfo* stt_provider_by_id(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (int i = 0; i < kSttProviderCount; i++) {
        if (strcasecmp(kSttProviders[i].id, id) == 0) return &kSttProviders[i];
    }
    return nullptr;
}

const char* stt_current_provider() {
    return s_stt_provider ? s_stt_provider->id : "";
}

const char* stt_current_model() {
    if (s_stt_model_override[0]) return s_stt_model_override;
    if (s_stt_provider) return s_stt_provider->stt_model;
    return "";
}

static void safe_copy(char* dst, size_t sz, const char* src) {
    if (!dst || !sz) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strnlen(src, sz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void tts_client_init(const char* tts_provider_id, const char* tts_api_key,
                     const char* tts_model, const char* tts_voice) {
    s_tts_provider = tts_provider_by_id(tts_provider_id);

    if (tts_api_key && tts_api_key[0]) {
        safe_copy(s_tts_api_key, sizeof(s_tts_api_key), tts_api_key);
    } else {
        s_tts_api_key[0] = '\0';
    }

    safe_copy(s_tts_model_override, sizeof(s_tts_model_override), tts_model ? tts_model : "");
    safe_copy(s_tts_voice_override, sizeof(s_tts_voice_override), tts_voice ? tts_voice : "");

    if (s_tts_provider) {
        Serial.printf("[TTS] Init provider=%s host=%s model=%s voice=%s\n",
                      s_tts_provider->name, s_tts_provider->host,
                      tts_current_model(), tts_current_voice());
    } else if (tts_provider_id && tts_provider_id[0]) {
        Serial.printf("[TTS] Unknown provider '%s', TTS will use LLM provider fallback\n",
                      tts_provider_id);
    }
}

void stt_client_init(const char* stt_provider_id, const char* stt_api_key,
                     const char* stt_model) {
    s_stt_provider = stt_provider_by_id(stt_provider_id);

    if (stt_api_key && stt_api_key[0]) {
        safe_copy(s_stt_api_key, sizeof(s_stt_api_key), stt_api_key);
    } else {
        s_stt_api_key[0] = '\0';
    }

    safe_copy(s_stt_model_override, sizeof(s_stt_model_override), stt_model ? stt_model : "");

    if (s_stt_provider) {
        Serial.printf("[STT] Init provider=%s host=%s model=%s\n",
                      s_stt_provider->name, s_stt_provider->host, stt_current_model());
    } else if (stt_provider_id && stt_provider_id[0]) {
        Serial.printf("[STT] Unknown provider '%s'\n", stt_provider_id);
    }
}

static const char* llm_host() {
    if (s_custom_host[0]) return s_custom_host;
    if (s_provider && s_provider->host[0]) return s_provider->host;
    return M5CLAW_MIMO_HOST;
}

static const char* llm_path() {
    if (s_custom_path[0]) return s_custom_path;
    if (s_provider && s_provider->chat_path[0]) return s_provider->chat_path;
    return M5CLAW_MIMO_CHAT_PATH;
}

const char* llm_current_host() { return llm_host(); }

bool llm_supports_audio_input() {
    return s_provider && s_provider->supports_audio_input;
}

static void* alloc_prefer_psram(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
}

void llm_client_init(const char* api_key, const char* model, const char* provider,
                     const char* custom_host, const char* custom_path) {
    s_provider = llm_provider_by_id(provider);
    if (!s_provider) {
        s_provider = &kProviders[0]; // fallback to MiMo
        Serial.printf("[LLM] Unknown provider '%s', falling back to %s\n",
                      provider ? provider : "(null)", s_provider->name);
    }

    if (api_key && api_key[0]) safe_copy(s_api_key, sizeof(s_api_key), api_key);

    if (model && model[0]) {
        safe_copy(s_model, sizeof(s_model), model);
    } else if (s_provider->default_model[0]) {
        safe_copy(s_model, sizeof(s_model), s_provider->default_model);
    }

    safe_copy(s_custom_host, sizeof(s_custom_host), custom_host ? custom_host : "");
    safe_copy(s_custom_path, sizeof(s_custom_path), custom_path ? custom_path : "");

    Serial.printf("[LLM] Init provider=%s model=%s host=%s path=%s tts=%d\n",
                  s_provider->name, s_model, llm_host(), llm_path(), s_provider->has_tts);
}

void llm_response_free(LlmResponse* resp) {
    free(resp->text);
    resp->text = nullptr;
    resp->text_len = 0;
    free(resp->reasoning_content);
    resp->reasoning_content = nullptr;
    free(resp->raw_content_json);
    resp->raw_content_json = nullptr;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = nullptr;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

static bool resolve_host(const char* host, IPAddress& ip, const char* tag) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("%s WiFi not connected\n", tag);
        return false;
    }
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (WiFi.hostByName(host, ip)) {
            Serial.printf("%s DNS %s -> %s\n", tag, host, ip.toString().c_str());
            return true;
        }
        delay(100);
    }
    return false;
}

static bool secure_connect(WiFiClientSecure& client, const char* host, uint16_t port, const char* tag) {
    IPAddress ip;
    if (!resolve_host(host, ip, tag)) return false;
    TlsConfig::configureClient(client, 30000);
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (client.connect(host, port)) return true;
        Serial.printf("%s connect failed (%d/2)\n", tag, attempt);
        delay(200);
    }
    return false;
}

static bool read_http_headers(WiFiClientSecure& client, HttpResponseMeta* meta) {
    if (!meta) return false;
    meta->status_code = 0;
    meta->chunked = false;
    meta->content_length = -1;
    meta->content_type[0] = '\0';
    char hdr[256];
    int hp = 0;
    int state = 0;
    bool firstLine = true;
    while (client.connected()) {
        if (is_aborted()) return false;
        if (!client.available()) { delay(10); continue; }
        char c = client.read();
        if (c != '\r' && c != '\n' && hp < (int)sizeof(hdr) - 1) hdr[hp++] = c;
        switch (state) {
            case 0: state = (c == '\r') ? 1 : (c == '\n') ? 2 : 0; break;
            case 1: state = (c == '\n') ? 2 : (c == '\r') ? 1 : 0; break;
            case 2: if (c == '\n') return true; state = (c == '\r') ? 3 : 0; break;
            case 3: if (c == '\n') return true; state = 0; break;
        }
        if (c == '\n') {
            hdr[hp] = '\0';
            if (firstLine) {
                firstLine = false;
                const char* sp = strchr(hdr, ' ');
                if (sp) meta->status_code = atoi(sp + 1);
            } else if (strncasecmp(hdr, "transfer-encoding:", 18) == 0) {
                const char* v = hdr + 18;
                while (*v == ' ') v++;
                if (strncasecmp(v, "chunked", 7) == 0) meta->chunked = true;
            } else if (strncasecmp(hdr, "content-length:", 15) == 0) {
                meta->content_length = atoi(hdr + 15);
            } else if (strncasecmp(hdr, "content-type:", 13) == 0) {
                const char* v = hdr + 13;
                while (*v == ' ') v++;
                size_t n = strcspn(v, ";");
                if (n >= sizeof(meta->content_type)) n = sizeof(meta->content_type) - 1;
                memcpy(meta->content_type, v, n);
                meta->content_type[n] = '\0';
            }
            hp = 0;
        }
    }
    return false;
}

struct ChunkedReader {
    WiFiClientSecure& client;
    bool chunked;
    int contentLen;
    int remaining;
    int bytesRead;
    bool eof;

    ChunkedReader(WiFiClientSecure& c, bool isChunked, int contentLength = -1)
        : client(c), chunked(isChunked), contentLen(contentLength), remaining(isChunked ? -1 : 0), bytesRead(0), eof(false) {}

    int readByte() {
        if (eof || is_aborted()) return -1;
        if (!chunked) {
            if (contentLen >= 0 && bytesRead >= contentLen) return -1;
            int c = rawRead();
            if (c >= 0) bytesRead++;
            else eof = true;
            return c;
        }
        if (remaining == 0) { skipTrailer(); remaining = -1; }
        if (remaining < 0) {
            remaining = nextChunkSize();
            if (remaining <= 0) { eof = true; return -1; }
        }
        int c = rawRead();
        if (c >= 0) remaining--;
        else eof = true;
        return c;
    }

private:
    int rawRead() {
        unsigned long t = millis();
        while (millis() - t < 30000) {
            if (is_aborted()) return -1;
            if (client.available()) return client.read();
            if (!client.connected()) break;
            delay(1);
        }
        return -1;
    }

    int nextChunkSize() {
        char buf[16];
        int p = 0;
        unsigned long t = millis();
        while (p < 15 && millis() - t < 10000) {
            if (is_aborted()) return 0;
            if (client.available()) {
                char c = client.read();
                if (c == '\n') break;
                if (c != '\r' && c != ' ') buf[p++] = c;
            } else if (!client.connected()) {
                return 0;
            } else {
                delay(1);
            }
        }
        buf[p] = '\0';
        return (int)strtol(buf, nullptr, 16);
    }

    void skipTrailer() {
        for (int i = 0; i < 2; i++) {
            unsigned long t = millis();
            while (millis() - t < 5000) {
                if (is_aborted()) return;
                if (client.available()) { client.read(); break; }
                if (!client.connected()) return;
                delay(1);
            }
        }
    }
};

static bool read_sse_line(ChunkedReader& reader, char* buf, int maxLen) {
    int pos = 0;
    while (pos < maxLen - 1) {
        int c = reader.readByte();
        if (c < 0) { buf[pos] = '\0'; return pos > 0; }
        if (c == '\n') { buf[pos] = '\0'; return true; }
        if (c != '\r') buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return true;
}

static bool text_append(LlmResponse* resp, const char* str, size_t len) {
    if (!len) return true;
    size_t new_len = resp->text_len + len;
    if (new_len >= M5CLAW_LLM_TEXT_MAX) {
        if (resp->text_len >= M5CLAW_LLM_TEXT_MAX - 1) return true;
        len = M5CLAW_LLM_TEXT_MAX - 1 - resp->text_len;
        new_len = resp->text_len + len;
    }
    char* nb = (char*)realloc(resp->text, new_len + 1);
    if (!nb) return false;
    memcpy(nb + resp->text_len, str, len);
    nb[new_len] = '\0';
    resp->text = nb;
    resp->text_len = new_len;
    return true;
}

static bool str_contains_nocase(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t needleLen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        if (strncasecmp(p, needle, needleLen) == 0) return true;
    }
    return false;
}

static bool parse_media_placeholder(const String& body, size_t tokenPos, RequestMediaRef* outRef) {
    if (!outRef) return false;
    size_t prefixLen = strlen(kMediaPlaceholderPrefix);
    size_t suffixLen = strlen(kMediaPlaceholderSuffix);
    size_t bodyLen = body.length();
    if (tokenPos + prefixLen >= bodyLen) return false;
    if (body.substring(tokenPos, tokenPos + prefixLen) != kMediaPlaceholderPrefix) return false;

    size_t mimeSep = body.indexOf('|', tokenPos + prefixLen);
    if (mimeSep == (size_t)-1) return false;
    size_t suffixPos = body.indexOf(kMediaPlaceholderSuffix, mimeSep + 1);
    if (suffixPos == (size_t)-1) return false;
    if (suffixPos <= mimeSep + 1) return false;

    String mime = body.substring(tokenPos + prefixLen, mimeSep);
    String path = body.substring(mimeSep + 1, suffixPos);
    if (mime.isEmpty() || path.isEmpty()) return false;
    if (mime.length() >= sizeof(outRef->mime) || path.length() >= sizeof(outRef->path)) return false;

    strlcpy(outRef->mime, mime.c_str(), sizeof(outRef->mime));
    strlcpy(outRef->path, path.c_str(), sizeof(outRef->path));
    outRef->pos = tokenPos;
    outRef->token_len = (suffixPos + suffixLen) - tokenPos;

    File f = SPIFFS.open(outRef->path, "r");
    if (!f) {
        Serial.printf("[LLM] Media file open failed: %s\n", outRef->path);
        return false;
    }
    size_t fileSize = f.size();
    f.close();
    if (fileSize == 0) {
        Serial.printf("[LLM] Media file is empty: %s\n", outRef->path);
        return false;
    }
    Serial.printf("[LLM] Media file %s size=%u mime=%s\n",
                  outRef->path, (unsigned)fileSize, outRef->mime);

    size_t base64Len = 4 * ((fileSize + 2) / 3);
    outRef->replacement_len = strlen("data:;base64,") + strlen(outRef->mime) + base64Len;
    if (outRef->replacement_len > M5CLAW_MEDIA_DATA_URI_MAX) {
        Serial.printf("[LLM] Media data URI too large: %u\n", (unsigned)outRef->replacement_len);
        return false;
    }
    return true;
}

static bool collect_request_media_refs(const String& body,
                                       RequestMediaRef* refs,
                                       int* outCount,
                                       size_t* outContentLen) {
    if (!outCount || !outContentLen) return false;
    *outCount = 0;
    *outContentLen = body.length();

    size_t prefixLen = strlen(kMediaPlaceholderPrefix);
    int from = 0;
    while (true) {
        int found = body.indexOf(kMediaPlaceholderPrefix, from);
        if (found < 0) return true;
        if (*outCount >= kMaxRequestMediaRefs) {
            Serial.println("[LLM] Too many media attachments in one request");
            return false;
        }

        RequestMediaRef ref = {};
        if (!parse_media_placeholder(body, (size_t)found, &ref)) {
            Serial.println("[LLM] Invalid media placeholder");
            return false;
        }

        refs[*outCount] = ref;
        (*outCount)++;
        if (ref.replacement_len >= ref.token_len) {
            *outContentLen += ref.replacement_len - ref.token_len;
        } else {
            *outContentLen -= ref.token_len - ref.replacement_len;
        }
        from = (int)(ref.pos + ref.token_len);
    }
}

static bool write_all(WiFiClientSecure& client, const char* data, size_t len, size_t* written_total = nullptr) {
    size_t sent = 0;
    while (sent < len) {
        if (is_aborted()) return false;
        size_t wrote = client.write((const uint8_t*)data + sent, len - sent);
        if (wrote == 0) {
            delay(1);
            if (!client.connected()) return false;
            continue;
        }
        sent += wrote;
    }
    if (written_total) *written_total += sent;
    return true;
}

static bool write_media_data_uri(WiFiClientSecure& client, const RequestMediaRef& ref, size_t* written_total = nullptr) {
    File f = SPIFFS.open(ref.path, "r");
    if (!f) {
        Serial.printf("[LLM] Media file open failed during send: %s\n", ref.path);
        return false;
    }

    char prefix[80];
    int prefixLen = snprintf(prefix, sizeof(prefix), "data:%s;base64,", ref.mime);
    if (prefixLen <= 0 || prefixLen >= (int)sizeof(prefix)) {
        f.close();
        return false;
    }
    if (!write_all(client, prefix, prefixLen, written_total)) {
        f.close();
        return false;
    }

    uint8_t rawBuf[768 + 2];
    unsigned char encBuf[4 * ((sizeof(rawBuf) + 2) / 3) + 4];
    size_t carry = 0;

    while (!is_aborted()) {
        size_t got = f.read(rawBuf + carry, sizeof(rawBuf) - carry);
        if (got == 0) break;

        size_t total = carry + got;
        size_t chunkLen = (total / 3) * 3;
        if (chunkLen > 0) {
            size_t encLen = 0;
            if (mbedtls_base64_encode(encBuf, sizeof(encBuf), &encLen, rawBuf, chunkLen) != 0) {
                f.close();
                return false;
            }
            if (!write_all(client, (const char*)encBuf, encLen, written_total)) {
                f.close();
                return false;
            }
        }

        carry = total - chunkLen;
        if (carry > 0) {
            memmove(rawBuf, rawBuf + chunkLen, carry);
        }
    }

    if (carry > 0) {
        size_t encLen = 0;
        if (mbedtls_base64_encode(encBuf, sizeof(encBuf), &encLen, rawBuf, carry) != 0) {
            f.close();
            return false;
        }
        if (!write_all(client, (const char*)encBuf, encLen, written_total)) {
            f.close();
            return false;
        }
    }

    f.close();
    return !is_aborted();
}

static bool send_request_body(WiFiClientSecure& client, const String& body, size_t* outContentLen = nullptr, size_t* outSentLen = nullptr) {
    RequestMediaRef refs[kMaxRequestMediaRefs];
    int refCount = 0;
    size_t contentLen = 0;
    if (!collect_request_media_refs(body, refs, &refCount, &contentLen)) return false;
    if (outContentLen) *outContentLen = contentLen;
    if (outSentLen) *outSentLen = 0;

    client.printf("Content-Length: %u\r\n", (unsigned)contentLen);
    client.println("Connection: close");
    client.println();

    if (refCount == 0) {
        return write_all(client, body.c_str(), body.length(), outSentLen);
    }

    size_t cursor = 0;
    for (int i = 0; i < refCount; i++) {
        const RequestMediaRef& ref = refs[i];
        if (ref.pos < cursor || ref.pos > body.length()) return false;
        if (!write_all(client, body.c_str() + cursor, ref.pos - cursor, outSentLen)) return false;
        if (!write_media_data_uri(client, ref, outSentLen)) return false;
        cursor = ref.pos + ref.token_len;
    }

    if (cursor < body.length()) {
        if (!write_all(client, body.c_str() + cursor, body.length() - cursor, outSentLen)) return false;
    }
    return true;
}

static void build_tool_response_json(LlmResponse* resp) {
    if (resp->call_count <= 0) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < resp->call_count; i++) {
        JsonObject tc = arr.add<JsonObject>();
        tc["id"] = resp->calls[i].id;
        tc["name"] = resp->calls[i].name;
        JsonDocument args;
        deserializeJson(args, resp->calls[i].input ? resp->calls[i].input : "{}");
        tc["arguments"] = args.as<JsonVariant>();
    }
    String raw;
    serializeJson(arr, raw);
    resp->raw_content_json = strdup(raw.c_str());
}

static void build_openai_body(JsonDocument& doc, const char* system_prompt,
                              JsonDocument& messages, const char* tools_json) {
    doc["model"] = s_model;
    doc["max_tokens"] = M5CLAW_LLM_MAX_TOKENS;
    doc["stream"] = true;

    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sysMsg = msgs.add<JsonObject>();
    sysMsg["role"] = "system";
    sysMsg["content"] = system_prompt;

    JsonArray src = messages.as<JsonArray>();
    for (JsonVariant v : src) msgs.add(v);

    bool hasTools = false;
    if (s_provider && s_provider->has_web_search) {
        JsonArray dstTools = doc["tools"].to<JsonArray>();
        JsonObject webSearch = dstTools.add<JsonObject>();
        webSearch["type"] = "web_search";
        webSearch["max_keyword"] = s_provider->search_max_keyword;
        webSearch["force_search"] = false;
        webSearch["limit"] = s_provider->search_limit;
        hasTools = true;
    }

    if (tools_json && tools_json[0]) {
        JsonDocument toolsDoc;
        deserializeJson(toolsDoc, tools_json);
        JsonArray srcTools = toolsDoc.as<JsonArray>();
        if (srcTools.size() > 0) {
            JsonArray dstTools = doc["tools"].to<JsonArray>();
            for (JsonVariant t : srcTools) {
                JsonObject wrap = dstTools.add<JsonObject>();
                wrap["type"] = "function";
                JsonObject func = wrap["function"].to<JsonObject>();
                func["name"] = t["name"];
                if (t["description"]) func["description"] = t["description"];
                if (t["input_schema"]) func["parameters"] = t["input_schema"];
            }
            hasTools = true;
        }
    }

    if (!hasTools) doc.remove("tools");
}

static void build_anthropic_body(JsonDocument& doc, const char* system_prompt,
                                  JsonDocument& messages, const char* tools_json) {
    doc["model"] = s_model;
    doc["max_tokens"] = M5CLAW_LLM_MAX_TOKENS;
    doc["stream"] = true;

    if (system_prompt && system_prompt[0]) {
        doc["system"] = system_prompt;
    }

    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonArray src = messages.as<JsonArray>();
    for (JsonVariant v : src) msgs.add(v);

    if (tools_json && tools_json[0]) {
        JsonDocument toolsDoc;
        deserializeJson(toolsDoc, tools_json);
        JsonArray srcTools = toolsDoc.as<JsonArray>();
        if (srcTools.size() > 0) {
            JsonArray dstTools = doc["tools"].to<JsonArray>();
            for (JsonVariant t : srcTools) {
                JsonObject tool = dstTools.add<JsonObject>();
                tool["name"] = t["name"];
                if (t["description"]) tool["description"] = t["description"];
                if (t["input_schema"]) tool["input_schema"] = t["input_schema"];
            }
        }
    }
}

static bool process_anthropic_stream(ChunkedReader& reader, LlmResponse* resp,
                                      LlmStreamCallback on_token) {
    char* line = (char*)alloc_prefer_psram(M5CLAW_SSE_LINE_BUF);
    if (!line) return false;

    String tool_inputs[M5CLAW_MAX_TOOL_CALLS];
    int tool_indices[M5CLAW_MAX_TOOL_CALLS] = {};
    int next_tool_slot = 0;
    bool got_response = false;

    while (!is_aborted()) {
        if (!read_sse_line(reader, line, M5CLAW_SSE_LINE_BUF)) break;
        if (strncmp(line, "data: ", 6) != 0) continue;
        const char* data = line + 6;

        JsonDocument chunk;
        if (deserializeJson(chunk, data) != DeserializationError::Ok) continue;

        const char* type = chunk["type"] | "";

        if (strcmp(type, "content_block_delta") == 0) {
            JsonObject delta = chunk["delta"];
            const char* deltaType = delta["type"] | "";
            int idx = chunk["index"] | 0;

            if (strcmp(deltaType, "text_delta") == 0) {
                const char* text = delta["text"] | "";
                if (text[0]) {
                    size_t tlen = strlen(text);
                    text_append(resp, text, tlen);
                    if (on_token) on_token(text);
                    got_response = true;
                }
            } else if (strcmp(deltaType, "input_json_delta") == 0) {
                const char* partial = delta["partial_json"] | "";
                if (partial[0]) {
                    int slot = -1;
                    for (int i = 0; i < next_tool_slot; i++) {
                        if (tool_indices[i] == idx) { slot = i; break; }
                    }
                    if (slot < 0 && next_tool_slot < M5CLAW_MAX_TOOL_CALLS) {
                        slot = next_tool_slot++;
                        tool_indices[slot] = idx;
                        if (slot >= resp->call_count) resp->call_count = slot + 1;
                    }
                    if (slot >= 0) tool_inputs[slot] += partial;
                }
            }
        } else if (strcmp(type, "content_block_start") == 0) {
            JsonObject block = chunk["content_block"];
            const char* blockType = block["type"] | "";
            int idx = chunk["index"] | 0;

            if (strcmp(blockType, "tool_use") == 0) {
                int slot = -1;
                for (int i = 0; i < next_tool_slot; i++) {
                    if (tool_indices[i] == idx) { slot = i; break; }
                }
                if (slot < 0 && next_tool_slot < M5CLAW_MAX_TOOL_CALLS) {
                    slot = next_tool_slot++;
                    tool_indices[slot] = idx;
                    if (slot >= resp->call_count) resp->call_count = slot + 1;
                }
                if (slot >= 0) {
                    LlmToolCall& call = resp->calls[slot];
                    const char* tcId = block["id"] | "";
                    const char* tcName = block["name"] | "";
                    if (tcId[0]) strlcpy(call.id, tcId, sizeof(call.id));
                    if (tcName[0]) strlcpy(call.name, tcName, sizeof(call.name));
                }
            }
        } else if (strcmp(type, "message_delta") == 0) {
            JsonObject delta = chunk["delta"];
            const char* stop = delta["stop_reason"] | "";
            if (strcmp(stop, "tool_use") == 0) {
                resp->tool_use = true;
            }
            got_response = true;
        } else if (strcmp(type, "message_stop") == 0) {
            got_response = true;
            break;
        }
    }

    for (int i = 0; i < resp->call_count; i++) {
        if (tool_inputs[i].length() > 0 && resp->calls[i].input == nullptr) {
            resp->calls[i].input = strdup(tool_inputs[i].c_str());
            resp->calls[i].input_len = tool_inputs[i].length();
        }
    }
    if (resp->call_count > 0) {
        resp->tool_use = true;
        build_tool_response_json(resp);
    }

    heap_caps_free(line);
    return got_response;
}

static bool process_openai_stream(ChunkedReader& reader, LlmResponse* resp,
                                  LlmStreamCallback on_token) {
    char* line = (char*)alloc_prefer_psram(M5CLAW_SSE_LINE_BUF);
    if (!line) return false;

    String tool_inputs[M5CLAW_MAX_TOOL_CALLS];
    String reasoning;
    bool got_response = false;

    while (!is_aborted()) {
        if (!read_sse_line(reader, line, M5CLAW_SSE_LINE_BUF)) break;
        if (strncmp(line, "data: ", 6) != 0) continue;
        const char* data = line + 6;

        if (strcmp(data, "[DONE]") == 0) {
            got_response = true;
            break;
        }

        JsonDocument chunk;
        if (deserializeJson(chunk, data) != DeserializationError::Ok) continue;

        JsonObject choice = chunk["choices"][0];
        if (choice.isNull()) continue;
        JsonObject delta = choice["delta"];

        const char* rc = delta["reasoning_content"] | (const char*)nullptr;
        if (rc && rc[0]) {
            reasoning += rc;
        }

        const char* content = delta["content"] | (const char*)nullptr;
        if (content) {
            size_t clen = strlen(content);
            if (clen > 0) {
                text_append(resp, content, clen);
                if (on_token) on_token(content);
                got_response = true;
            }
        }

        JsonArray tool_calls = delta["tool_calls"];
        if (!tool_calls.isNull()) {
            for (JsonVariant tc : tool_calls) {
                int idx = tc["index"] | 0;
                if (idx >= M5CLAW_MAX_TOOL_CALLS) continue;
                if (idx >= resp->call_count) resp->call_count = idx + 1;
                LlmToolCall& call = resp->calls[idx];
                const char* tc_id = tc["id"] | (const char*)nullptr;
                const char* fn_name = tc["function"]["name"] | (const char*)nullptr;
                if (tc_id) strlcpy(call.id, tc_id, sizeof(call.id));
                if (fn_name) strlcpy(call.name, fn_name, sizeof(call.name));
                const char* args = tc["function"]["arguments"] | (const char*)nullptr;
                if (args) tool_inputs[idx] += args;
            }
        }

        const char* finish = choice["finish_reason"] | (const char*)nullptr;
        if (finish) {
            resp->tool_use = (strcmp(finish, "tool_calls") == 0);
            got_response = true;
        }
    }

    for (int i = 0; i < resp->call_count; i++) {
        if (tool_inputs[i].length() > 0 && resp->calls[i].input == nullptr) {
            resp->calls[i].input = strdup(tool_inputs[i].c_str());
            resp->calls[i].input_len = tool_inputs[i].length();
        }
    }
    if (resp->call_count > 0) {
        resp->tool_use = true;
        build_tool_response_json(resp);
    }
    if (reasoning.length() > 0) {
        resp->reasoning_content = strdup(reasoning.c_str());
    }

    heap_caps_free(line);
    return got_response;
}

static bool read_json_body(WiFiClientSecure& client, bool chunked, int contentLength, char** outBuf, size_t* outLen, size_t maxLen) {
    *outBuf = nullptr;
    *outLen = 0;

    // Use Content-Length when available to avoid overallocation
    size_t allocSize;
    if (contentLength > 0) {
        allocSize = (size_t)contentLength + 1;
        if (allocSize > maxLen) allocSize = maxLen;
    } else {
        allocSize = maxLen;
    }

    char* buf = nullptr;
    size_t attemptSize = allocSize;
    while (!buf && attemptSize >= 4096) {
        buf = (char*)alloc_prefer_psram(attemptSize);
        if (!buf) {
            Serial.printf("[HTTP] Alloc %u failed\n", (unsigned)attemptSize);
            attemptSize = attemptSize * 2 / 3;
        }
    }
    if (!buf && allocSize < 4096) {
        buf = (char*)alloc_prefer_psram(allocSize);
    }
    if (!buf) {
        Serial.printf("[HTTP] Alloc %u failed (final)\n", (unsigned)attemptSize > 0 ? (unsigned)attemptSize : (unsigned)allocSize);
        return false;
    }
    if (attemptSize >= 4096) allocSize = attemptSize;

    size_t len = 0;
    ChunkedReader reader(client, chunked, contentLength);
    while (len < allocSize - 1) {
        int c = reader.readByte();
        if (c < 0) break;
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    *outBuf = buf;
    *outLen = len;
    if (len == 0) Serial.println("[HTTP] Body read returned 0 bytes");
    return len > 0;
}

static bool read_body_to_spiffs(WiFiClientSecure& client, bool chunked, int contentLength, const char* path) {
    SPIFFS.remove(path);
    File f = SPIFFS.open(path, "w");
    if (!f) {
        Serial.printf("[HTTP] Cannot open SPIFFS for write: %s\n", path);
        return false;
    }
    ChunkedReader reader(client, chunked, contentLength);
    uint8_t buf[4096];
    size_t bufPos = 0;
    size_t total = 0;
    while (true) {
        int c = reader.readByte();
        if (c < 0) break;
        buf[bufPos++] = (uint8_t)c;
        if (bufPos == sizeof(buf)) {
            if (f.write(buf, sizeof(buf)) != sizeof(buf)) {
                Serial.println("[HTTP] SPIFFS write failed");
                f.close();
                return false;
            }
            total += sizeof(buf);
            bufPos = 0;
        }
    }
    if (bufPos > 0) {
        f.write(buf, bufPos);
        total += bufPos;
    }
    f.close();
    Serial.printf("[HTTP] Streamed %u bytes to %s\n", (unsigned)total, path);
    return total > 0;
}

// Stream a WAV body from HTTP directly to the speaker — no SPIFFS round-trip.
// Parses the WAV header from initial body bytes, then feeds PCM to the speaker
// in 8KB chunks with a 10ms overlap to avoid gaps.
static bool stream_wav_body_to_speaker(WiFiClientSecure& client, bool chunked, int contentLength) {
    ChunkedReader reader(client, chunked, contentLength);

    const size_t kMaxHdr = 1024;
    uint8_t* hdr = (uint8_t*)malloc(kMaxHdr);
    if (!hdr) return false;

    size_t hdrLen = 0;
    while (hdrLen < kMaxHdr) {
        int c = reader.readByte();
        if (c < 0) break;
        hdr[hdrLen++] = (uint8_t)c;
    }

    if (hdrLen < 44) { free(hdr); return false; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        free(hdr);
        return false;
    }

    uint16_t audioFormat = 0, channels = 1, bitsPerSample = 16;
    uint32_t sampleRate = 24000;
    uint32_t pcmOffset = 0, pcmLen = 0;

    size_t pos = 12;
    while (pos + 8 <= hdrLen) {
        const uint8_t* chunk = hdr + pos;
        uint32_t chunkSize = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8)
                           | ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16 && pos + 8 + chunkSize <= hdrLen) {
            const uint8_t* fmt = hdr + pos + 8;
            audioFormat = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            sampleRate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8)
                       | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bitsPerSample = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
        } else if (memcmp(chunk, "data", 4) == 0) {
            pcmOffset = pos + 8;
            pcmLen = chunkSize;
            break;
        }
        pos += 8 + chunkSize + (chunkSize & 1U);
    }

    if (!pcmLen || audioFormat != 1 || bitsPerSample != 16 || sampleRate == 0) {
        Serial.printf("[TTS] Bad WAV stream: fmt=%u ch=%u rate=%u bps=%u pcm=%u/%u\n",
                      audioFormat, channels, sampleRate, bitsPerSample,
                      (unsigned)pcmOffset, (unsigned)pcmLen);
        free(hdr);
        return false;
    }

    Serial.printf("[TTS] Streaming WAV: %uHz %uch %ubit, PCM %u bytes at offset %u\n",
                  sampleRate, channels, bitsPerSample, (unsigned)pcmLen, (unsigned)pcmOffset);

    // Skip any header bytes beyond our initial read window
    if (pcmOffset > hdrLen) {
        size_t skip = pcmOffset - hdrLen;
        while (skip--) {
            if (reader.readByte() < 0) { free(hdr); return false; }
        }
    }

    // Initial PCM bytes already in the header buffer
    const uint8_t* initialPcm = hdr + pcmOffset;
    size_t initialPcmLen = (pcmOffset < hdrLen) ? (hdrLen - pcmOffset) : 0;

    // Allocate streaming chunk buffer
    const size_t kChunkBytes = 8192;
    uint8_t* chunk = (uint8_t*)malloc(kChunkBytes);
    if (!chunk) { free(hdr); return false; }
    free(hdr);

    M5Cardputer.Speaker.stop();
    bool first = true;
    size_t pcmRemaining = pcmLen;

    auto feedChunk = [&](const uint8_t* data, size_t len) -> bool {
        if (len == 0) return true;
        bool ok = M5Cardputer.Speaker.playRaw((const int16_t*)data, len / 2,
                                               sampleRate, channels > 1, 1, -1, first);
        if (!ok) return false;
        first = false;
        // Wait slightly less than chunk duration so the next chunk arrives
        // before the speaker buffer drains (10ms overlap, no gaps).
        unsigned long chunkMs = len * 1000UL / 2 / sampleRate;
        if (chunkMs > 10) chunkMs -= 10;
        if (chunkMs > 0) delay(chunkMs);
        pcmRemaining -= len;
        return true;
    };

    if (initialPcmLen > 0) {
        if (!feedChunk(initialPcm, initialPcmLen)) { free(chunk); return false; }
    }

    while (pcmRemaining > 0) {
        size_t toRead = pcmRemaining < kChunkBytes ? pcmRemaining : kChunkBytes;
        size_t n = 0;
        while (n < toRead) {
            int c = reader.readByte();
            if (c < 0) break;
            chunk[n++] = (uint8_t)c;
        }
        if (n == 0) break;
        if (!feedChunk(chunk, n)) { free(chunk); return false; }
    }

    // Drain — wait for the speaker to finish the last chunk
    unsigned long waitUntil = millis() + (pcmLen * 1000UL / 2 / sampleRate) + 1500;
    while (M5Cardputer.Speaker.isPlaying() && millis() < waitUntil) {
        delay(10);
    }

    free(chunk);
    return true;
}

static bool parse_openai_json_response(const char* body, size_t bodyLen, LlmResponse* resp) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, bodyLen);
    if (err) return false;

    JsonObject choice = doc["choices"][0];
    if (choice.isNull()) return false;

    JsonObject message = choice["message"];
    const char* content = message["content"] | (const char*)nullptr;
    if (content && content[0]) {
        if (!text_append(resp, content, strlen(content))) return false;
    }

    const char* reasoning = message["reasoning_content"] | (const char*)nullptr;
    if (reasoning && reasoning[0]) {
        resp->reasoning_content = strdup(reasoning);
    }

    JsonArray toolCalls = message["tool_calls"];
    if (!toolCalls.isNull()) {
        int idx = 0;
        for (JsonVariant tc : toolCalls) {
            if (idx >= M5CLAW_MAX_TOOL_CALLS) break;
            LlmToolCall& call = resp->calls[idx];
            const char* tcId = tc["id"] | "";
            const char* fnName = tc["function"]["name"] | "";
            const char* args = tc["function"]["arguments"] | "{}";
            strlcpy(call.id, tcId, sizeof(call.id));
            strlcpy(call.name, fnName, sizeof(call.name));
            call.input = strdup(args);
            call.input_len = strlen(args);
            idx++;
        }
        resp->call_count = idx;
        if (resp->call_count > 0) {
            resp->tool_use = true;
            build_tool_response_json(resp);
        }
    }

    const char* finish = choice["finish_reason"] | "";
    if (strcmp(finish, "tool_calls") == 0) {
        resp->tool_use = true;
    }
    return resp->text_len > 0 || resp->call_count > 0;
}

bool llm_chat_tools(const char* system_prompt,
                    JsonDocument& messages,
                    const char* tools_json,
                    LlmResponse* resp,
                    LlmStreamCallback on_token) {
    memset(resp, 0, sizeof(*resp));
    if (s_api_key[0] == '\0') {
        Serial.println("[LLM] No API key configured");
        return false;
    }

    bool isAnthropic = s_provider && s_provider->api_format &&
                       strcmp(s_provider->api_format, "anthropic") == 0;
    bool isXApiKey = s_provider && s_provider->auth_format &&
                     strcmp(s_provider->auth_format, "x-api-key") == 0;

    void* bodyDocMem = alloc_prefer_psram(sizeof(JsonDocument));
    JsonDocument* bodyDoc = bodyDocMem ? new (bodyDocMem) JsonDocument : nullptr;
    if (!bodyDoc) return false;

    if (isAnthropic) {
        build_anthropic_body(*bodyDoc, system_prompt, messages, tools_json);
    } else {
        build_openai_body(*bodyDoc, system_prompt, messages, tools_json);
    }

    String bodyStr;
    bodyStr.reserve(8192);
    serializeJson(*bodyDoc, bodyStr);
    bodyDoc->~JsonDocument();
    heap_caps_free(bodyDoc);

    Serial.printf("[LLM] Request %d bytes to %s%s\n", bodyStr.length(), llm_host(), llm_path());

    if (s_pre_read_free_fn) {
        s_pre_read_free_fn();
    }

    WiFiClientSecure client;
    if (!secure_connect(client, llm_host(), 443, "[LLM]")) {
        Serial.println("[LLM] Connection failed");
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", llm_path());
    client.printf("Host: %s\r\n", llm_host());
    client.println("Content-Type: application/json");
    client.println("Accept: text/event-stream");
    if (isXApiKey) {
        client.printf("x-api-key: %s\r\n", s_api_key);
        client.println("anthropic-version: 2023-06-01");
    } else {
        client.printf("Authorization: Bearer %s\r\n", s_api_key);
    }
    size_t contentLen = 0;
    size_t sentLen = 0;
    if (!send_request_body(client, bodyStr, &contentLen, &sentLen)) {
        client.stop();
        return false;
    }
    Serial.printf("[LLM] Sent %u/%u bytes\n", (unsigned)sentLen, (unsigned)contentLen);
    bodyStr = String();

    if (is_aborted()) {
        client.stop();
        return false;
    }

    HttpResponseMeta meta = {};
    if (!read_http_headers(client, &meta)) {
        client.stop();
        return false;
    }
    Serial.printf("[LLM] Response status=%d type=%s chunked=%d\n",
                  meta.status_code, meta.content_type[0] ? meta.content_type : "(unknown)", meta.chunked);

    if (meta.status_code < 200 || meta.status_code >= 300) {
        char* errBody = nullptr;
        size_t errLen = 0;
        if (read_json_body(client, meta.chunked, meta.content_length, &errBody, &errLen, kErrorBodyPreviewMax) && errBody) {
            Serial.printf("[LLM] Error body: %.400s\n", errBody);
            heap_caps_free(errBody);
        }
        client.stop();
        return false;
    }

    if (str_contains_nocase(meta.content_type, "application/json")) {
        char* jsonBody = nullptr;
        size_t jsonLen = 0;
        bool ok = read_json_body(client, meta.chunked, meta.content_length, &jsonBody, &jsonLen, 256 * 1024);
        client.stop();
        if (!ok || !jsonBody) return false;
        bool parsed = parse_openai_json_response(jsonBody, jsonLen, resp);
        if (!parsed) {
            Serial.printf("[LLM] JSON response parse failed: %.400s\n", jsonBody);
        }
        heap_caps_free(jsonBody);
        Serial.printf("[LLM] %d text bytes, %d tool calls, tool_use=%d\n",
                      (int)resp->text_len, resp->call_count, resp->tool_use);
        return parsed;
    }

    ChunkedReader reader(client, meta.chunked);
    bool ok;
    if (isAnthropic) {
        ok = process_anthropic_stream(reader, resp, on_token);
    } else {
        ok = process_openai_stream(reader, resp, on_token);
    }
    client.stop();

    if (!ok && (!resp->text || resp->text_len == 0) && resp->call_count == 0) {
        Serial.println("[LLM] Empty/failed stream response");
    }
    Serial.printf("[LLM] %d text bytes, %d tool calls, tool_use=%d\n",
                  (int)resp->text_len, resp->call_count, resp->tool_use);
    return ok;
}

static bool tts_post_json(const char* path, const String& bodyStr,
                          HttpResponseMeta* meta, char** body, size_t* bodyLen) {
    *body = nullptr;
    *bodyLen = 0;

    WiFiClientSecure client;
    if (!secure_connect(client, llm_host(), 443, "[TTS]")) return false;

    client.printf("POST %s HTTP/1.1\r\n", path);
    client.printf("Host: %s\r\n", llm_host());
    client.println("Content-Type: application/json");
    client.println("Accept: application/json, audio/*, application/octet-stream");
    client.printf("Authorization: Bearer %s\r\n", s_api_key);
    client.printf("Content-Length: %d\r\n", bodyStr.length());
    client.println("Connection: close");
    client.println();
    client.print(bodyStr);

    HttpResponseMeta localMeta = {};
    if (!read_http_headers(client, &localMeta)) {
        client.stop();
        return false;
    }

    char* respBody = nullptr;
    size_t respLen = 0;
    bool ok = read_json_body(client, localMeta.chunked, localMeta.content_length, &respBody, &respLen, 256 * 1024);
    client.stop();
    if (meta) *meta = localMeta;
    if (!ok || !respBody) return false;

    if (localMeta.status_code < 200 || localMeta.status_code >= 300) {
        Serial.printf("[TTS] HTTP %d path=%s type=%s cl=%d\n",
                      localMeta.status_code, path,
                      localMeta.content_type[0] ? localMeta.content_type : "(unknown)",
                      localMeta.content_length);
        Serial.printf("[TTS] Error body: %.200s\n", respBody);
        heap_caps_free(respBody);
        return false;
    }

    *body = respBody;
    *bodyLen = respLen;
    return true;
}

static bool play_pcm_audio(const uint8_t* decoded, size_t decodedLen) {
    if (!decoded || decodedLen < 2) return false;
    int sampleRate = s_provider ? s_provider->tts_sample_rate : M5CLAW_MIMO_TTS_SAMPLE_RATE;

    M5Cardputer.Speaker.stop();
    bool played = M5Cardputer.Speaker.playRaw((const int16_t*)decoded, decodedLen / 2,
                                              sampleRate, false, 1, -1, true);
    if (played) {
        unsigned long waitUntil = millis() + (decodedLen * 1000UL / 2 / sampleRate) + 1500;
        while (M5Cardputer.Speaker.isPlaying() && millis() < waitUntil) {
            delay(10);
        }
    }
    return played;
}

static bool play_wav_audio(const uint8_t* data, size_t len) {
    if (!data || len < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmLen = 0;

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t* chunk = data + pos;
        uint32_t chunkSize = (uint32_t)chunk[4]
                           | ((uint32_t)chunk[5] << 8)
                           | ((uint32_t)chunk[6] << 16)
                           | ((uint32_t)chunk[7] << 24);
        size_t chunkData = pos + 8;
        if (chunkData + chunkSize > len) break;

        if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFormat = (uint16_t)data[chunkData] | ((uint16_t)data[chunkData + 1] << 8);
            channels = (uint16_t)data[chunkData + 2] | ((uint16_t)data[chunkData + 3] << 8);
            sampleRate = (uint32_t)data[chunkData + 4]
                       | ((uint32_t)data[chunkData + 5] << 8)
                       | ((uint32_t)data[chunkData + 6] << 16)
                       | ((uint32_t)data[chunkData + 7] << 24);
            bitsPerSample = (uint16_t)data[chunkData + 14] | ((uint16_t)data[chunkData + 15] << 8);
        } else if (memcmp(chunk, "data", 4) == 0) {
            pcm = data + chunkData;
            pcmLen = chunkSize;
            break;
        }

        pos = chunkData + chunkSize + (chunkSize & 1U);
    }

    if (!pcm || pcmLen < 2) return false;
    if (audioFormat != 1 || bitsPerSample != 16 || sampleRate == 0) return false;

    M5Cardputer.Speaker.stop();
    bool played = M5Cardputer.Speaker.playRaw((const int16_t*)pcm, pcmLen / 2,
                                              sampleRate, channels > 1, 1, -1, true);
    if (played) {
        unsigned long waitUntil = millis() + (pcmLen * 1000UL / 2 / sampleRate) + 1500;
        while (M5Cardputer.Speaker.isPlaying() && millis() < waitUntil) {
            delay(10);
        }
    }
    return played;
}

static bool play_wav_from_spiffs(const char* path) {
    File f = SPIFFS.open(path, "r");
    if (!f) {
        Serial.printf("[TTS] Cannot open WAV file: %s\n", path);
        return false;
    }
    size_t fileSize = f.size();
    if (fileSize < 44) { f.close(); return false; }

    // Some WAV files carry metadata chunks (LIST, fact) between fmt
    // and data, pushing data past the 128-byte mark. Use a 1024-byte
    // heap buffer for header scan; freed before the PCM allocation below.
    size_t hdrSize = fileSize < 1024 ? fileSize : 1024;
    uint8_t* hdr = (uint8_t*)malloc(hdrSize);
    if (!hdr) { f.close(); return false; }
    if (f.read(hdr, hdrSize) != hdrSize) { free(hdr); f.close(); return false; }

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        Serial.println("[TTS] Not a valid WAV file");
        free(hdr); f.close();
        return false;
    }

    uint16_t audioFormat = 0, channels = 1, bitsPerSample = 16;
    uint32_t sampleRate = 24000;
    uint32_t pcmOffset = 0, pcmLen = 0;

    size_t pos = 12;
    while (pos + 8 <= hdrSize) {
        const uint8_t* chunk = hdr + pos;
        uint32_t chunkSize = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8)
                           | ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16 && pos + 8 + chunkSize <= hdrSize) {
            const uint8_t* fmt = hdr + pos + 8;
            audioFormat = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            sampleRate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8)
                       | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bitsPerSample = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
        } else if (memcmp(chunk, "data", 4) == 0) {
            pcmOffset = pos + 8;
            pcmLen = chunkSize;
            if (pcmOffset + pcmLen > fileSize) pcmLen = fileSize - pcmOffset;
            break;
        }
        pos += 8 + chunkSize + (chunkSize & 1U);
    }
    free(hdr);

    if (!pcmLen || audioFormat != 1 || bitsPerSample != 16 || sampleRate == 0) {
        Serial.printf("[TTS] Bad WAV: fmt=%u ch=%u rate=%u bps=%u pcm=%u/%u\n",
                      audioFormat, channels, sampleRate, bitsPerSample,
                      (unsigned)pcmOffset, (unsigned)pcmLen);
        f.close();
        return false;
    }

    Serial.printf("[TTS] WAV: %uHz %uch %ubit, PCM %u bytes at offset %u\n",
                  sampleRate, channels, bitsPerSample, (unsigned)pcmLen, (unsigned)pcmOffset);

    // Try to allocate full PCM buffer (SSL client is already freed, heap should be clean)
    size_t allocSize = pcmLen + 4;
    uint8_t* pcmBuf = (uint8_t*)alloc_prefer_psram(allocSize);

    if (pcmBuf) {
        f.seek(pcmOffset);
        size_t read = f.read(pcmBuf, pcmLen);
        f.close();
        if (read != pcmLen) {
            Serial.printf("[TTS] PCM read mismatch: got %u expected %u\n", (unsigned)read, (unsigned)pcmLen);
            heap_caps_free(pcmBuf);
            return false;
        }
        M5Cardputer.Speaker.stop();
        bool played = M5Cardputer.Speaker.playRaw((const int16_t*)pcmBuf, pcmLen / 2,
                                                   sampleRate, channels > 1, 1, -1, true);
        if (played) {
            unsigned long waitUntil = millis() + (pcmLen * 1000UL / 2 / sampleRate) + 1500;
            while (M5Cardputer.Speaker.isPlaying() && millis() < waitUntil) {
                delay(10);
            }
        }
        heap_caps_free(pcmBuf);
        return played;
    }

    // Chunked fallback — read and play in 4KB chunks
    Serial.printf("[TTS] PCM alloc %u failed, chunked playback\n", (unsigned)allocSize);
    f.seek(pcmOffset);
    constexpr size_t kChunkBytes = 4096;
    uint8_t chunk[kChunkBytes];
    M5Cardputer.Speaker.stop();
    bool first = true;
    while (pcmLen > 0) {
        size_t n = pcmLen < kChunkBytes ? pcmLen : kChunkBytes;
        f.read(chunk, n);
        bool ok = M5Cardputer.Speaker.playRaw((const int16_t*)chunk, n / 2,
                                               sampleRate, channels > 1, 1, -1, !first);
        if (!ok) { f.close(); return false; }
        unsigned long chunkMs = n * 1000UL / 2 / sampleRate;
        if (chunkMs > 10) chunkMs -= 10;
        if (chunkMs > 0) delay(chunkMs);
        pcmLen -= n;
        first = false;
    }
    f.close();
    return true;
}

static bool extract_audio_b64(const char* body, size_t bodyLen, String& audioB64) {
    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, body, bodyLen);
    if (err) return false;

    const char* candidate = respDoc["choices"][0]["message"]["audio"]["data"] | "";
    if (!candidate[0]) candidate = respDoc["choices"][0]["audio"]["data"] | "";
    if (!candidate[0]) candidate = respDoc["audio"]["data"] | "";
    if (!candidate[0]) candidate = respDoc["data"] | "";
    if (!candidate[0]) return false;

    audioB64 = candidate;
    return true;
}

static bool tts_post_json_to_host(const char* host, const char* path,
                                   const char* api_key, const String& bodyStr,
                                   HttpResponseMeta* meta, char** body, size_t* bodyLen) {
    *body = nullptr;
    *bodyLen = 0;

    WiFiClientSecure client;
    if (!secure_connect(client, host, 443, "[TTS]")) return false;

    client.printf("POST %s HTTP/1.1\r\n", path);
    client.printf("Host: %s\r\n", host);
    client.println("Content-Type: application/json");
    client.println("Accept: application/json, audio/*, application/octet-stream");
    client.printf("Authorization: Bearer %s\r\n", api_key);
    client.printf("Content-Length: %d\r\n", bodyStr.length());
    client.println("Connection: close");
    client.println();
    client.print(bodyStr);

    HttpResponseMeta localMeta = {};
    if (!read_http_headers(client, &localMeta)) {
        client.stop();
        return false;
    }

    char* respBody = nullptr;
    size_t respLen = 0;
    bool ok = read_json_body(client, localMeta.chunked, localMeta.content_length, &respBody, &respLen, 256 * 1024);
    client.stop();
    if (meta) *meta = localMeta;
    if (!ok || !respBody) return false;

    if (localMeta.status_code < 200 || localMeta.status_code >= 300) {
        Serial.printf("[TTS] HTTP %d host=%s path=%s type=%s\n",
                      localMeta.status_code, host, path,
                      localMeta.content_type[0] ? localMeta.content_type : "(unknown)");
        Serial.printf("[TTS] Error body: %.200s\n", respBody);
        heap_caps_free(respBody);
        return false;
    }

    *body = respBody;
    *bodyLen = respLen;
    return true;
}

static bool tts_send_request(WiFiClientSecure& client, const char* host, const char* path,
                              const char* api_key, const String& bodyStr, HttpResponseMeta* meta) {
    if (!secure_connect(client, host, 443, "[TTS]")) return false;

    client.printf("POST %s HTTP/1.1\r\n", path);
    client.printf("Host: %s\r\n", host);
    client.println("Content-Type: application/json");
    client.println("Accept: application/json, audio/*, application/octet-stream");
    client.printf("Authorization: Bearer %s\r\n", api_key);
    client.printf("Content-Length: %d\r\n", bodyStr.length());
    client.println("Connection: close");
    client.println();
    client.print(bodyStr);

    HttpResponseMeta localMeta = {};
    if (!read_http_headers(client, &localMeta)) {
        client.stop();
        return false;
    }
    if (meta) *meta = localMeta;

    if (localMeta.status_code < 200 || localMeta.status_code >= 300) {
        char* errBody = nullptr;
        size_t errLen = 0;
        if (read_json_body(client, localMeta.chunked, localMeta.content_length, &errBody, &errLen, kErrorBodyPreviewMax) && errBody) {
            Serial.printf("[TTS] HTTP %d host=%s path=%s: %.200s\n",
                         localMeta.status_code, host, path, errBody);
            heap_caps_free(errBody);
        } else {
            Serial.printf("[TTS] HTTP %d host=%s path=%s (no body)\n",
                         localMeta.status_code, host, path);
        }
        client.stop();
        return false;
    }
    return true;
}

bool llm_speak_text(const char* text) {
    if (!text || !text[0] || WiFi.status() != WL_CONNECTED) return false;

    const char* ttsApiKey = s_tts_api_key[0] ? s_tts_api_key : s_api_key;
    if (ttsApiKey[0] == '\0') {
        Serial.println("[TTS] No API key configured");
        return false;
    }

    String clipped = text;
    if (clipped.length() > M5CLAW_TTS_TEXT_MAX) {
        clipped = clipped.substring(0, M5CLAW_TTS_TEXT_MAX);
    }

    auto tryPlayBody = [&](const HttpResponseMeta& meta, char* body, size_t bodyLen,
                           int ttsSampleRate = 0) -> bool {
        if (!body || bodyLen == 0) {
            Serial.println("[TTS] Empty response body");
            return false;
        }

        bool isJson = str_contains_nocase(meta.content_type, "application/json");
        bool isWav = str_contains_nocase(meta.content_type, "audio/wav");
        if (!isJson) {
            Serial.printf("[TTS] Audio response (%u bytes, type=%s, sampleRate=%d)\n",
                          (unsigned)bodyLen,
                          meta.content_type[0] ? meta.content_type : "(unknown)",
                          ttsSampleRate);
            bool played = false;
            if (isWav) {
                // WAV response — decode header first, then play PCM
                played = play_wav_audio((const uint8_t*)body, bodyLen);
                if (!played) Serial.println("[TTS] WAV decode failed");
            }
            if (!played && ttsSampleRate > 0 && bodyLen >= 2) {
                M5Cardputer.Speaker.stop();
                played = M5Cardputer.Speaker.playRaw((const int16_t*)body, bodyLen / 2,
                                                     ttsSampleRate, false, 1, -1, true);
                if (played) {
                    unsigned long waitUntil = millis() + (bodyLen * 1000UL / 2 / ttsSampleRate) + 1500;
                    while (M5Cardputer.Speaker.isPlaying() && millis() < waitUntil) {
                        delay(10);
                    }
                } else {
                    Serial.println("[TTS] playRaw failed");
                }
            }
            if (!played) played = play_pcm_audio((const uint8_t*)body, bodyLen);
            if (!played) Serial.println("[TTS] All audio playback methods failed");
            heap_caps_free(body);
            return played;
        }

        Serial.printf("[TTS] JSON response (%u bytes):\n%.500s\n", (unsigned)bodyLen, body);
        String audioB64;
        bool found = extract_audio_b64(body, bodyLen, audioB64);
        if (!found || audioB64.length() == 0) {
            Serial.printf("[TTS] Audio B64 extraction failed. Response: %.300s\n", body);
            heap_caps_free(body);
            return false;
        }
        heap_caps_free(body);

        Serial.printf("[TTS] Extracted B64 audio (%d chars), decoding...\n", audioB64.length());
        size_t maxDecoded = (audioB64.length() * 3) / 4 + 4;
        uint8_t* decoded = (uint8_t*)alloc_prefer_psram(maxDecoded);
        if (!decoded) {
            Serial.println("[TTS] Failed to alloc decode buffer");
            return false;
        }

        size_t decodedLen = 0;
        if (mbedtls_base64_decode(decoded, maxDecoded, &decodedLen,
                                  (const unsigned char*)audioB64.c_str(), audioB64.length()) != 0) {
            Serial.println("[TTS] Base64 decode failed");
            heap_caps_free(decoded);
            return false;
        }

        bool played = play_wav_audio(decoded, decodedLen);
        if (!played) played = play_pcm_audio(decoded, decodedLen);
        if (!played) Serial.println("[TTS] Audio playback failed (WAV/PCM)");
        heap_caps_free(decoded);
        return played;
    };

    // ── Standalone TTS provider (e.g. SiliconFlow) ──
    auto tryTtsProvider = [&]() -> bool {
        if (!s_tts_provider) return false;

        const char* voice = tts_current_voice();
        const char* model = tts_current_model();
        JsonDocument doc;
        doc["model"] = model;
        doc["input"] = clipped;
        doc["voice"] = voice;
        doc["response_format"] = "wav";

        String bodyStr;
        bodyStr.reserve(512);
        serializeJson(doc, bodyStr);

        Serial.printf("[TTS] Requesting standalone TTS: %s host=%s voice=%s key=%s\n",
                      s_tts_provider->name, s_tts_provider->host, voice,
                      s_tts_api_key[0] ? "explicit" : "LLM-fallback");

        WiFiClientSecure client;
        HttpResponseMeta meta = {};
        if (!tts_send_request(client, s_tts_provider->host, s_tts_provider->tts_path,
                              ttsApiKey, bodyStr, &meta)) {
            Serial.printf("[TTS] Standalone TTS HTTP request failed (status=%d)\n", meta.status_code);
            return false;
        }

        bool isJson = str_contains_nocase(meta.content_type, "application/json");
        if (isJson) {
            // JSON response — save to SPIFFS, extract base64 audio, decode, play
            if (!read_body_to_spiffs(client, meta.chunked, meta.content_length, M5CLAW_TTS_TEMP_FILE)) {
                client.stop();
                return false;
            }
            client.stop();

            File jf = SPIFFS.open(M5CLAW_TTS_TEMP_FILE, "r");
            if (!jf) { SPIFFS.remove(M5CLAW_TTS_TEMP_FILE); return false; }
            size_t jsz = jf.size();
            if (jsz == 0 || jsz > 16384) { jf.close(); SPIFFS.remove(M5CLAW_TTS_TEMP_FILE); return false; }
            char* jbuf = (char*)malloc(jsz + 1);
            if (!jbuf) { jf.close(); SPIFFS.remove(M5CLAW_TTS_TEMP_FILE); return false; }
            jf.readBytes(jbuf, jsz);
            jbuf[jsz] = '\0';
            jf.close();
            SPIFFS.remove(M5CLAW_TTS_TEMP_FILE);

            Serial.printf("[TTS] JSON response: %.500s\n", jbuf);
            String audioB64;
            bool found = extract_audio_b64(jbuf, jsz, audioB64);
            free(jbuf);
            if (!found || audioB64.length() == 0) {
                Serial.println("[TTS] Audio B64 extraction failed");
                return false;
            }
            Serial.printf("[TTS] Extracted B64 audio (%d chars), decoding...\n", audioB64.length());
            size_t maxDecoded = (audioB64.length() * 3) / 4 + 4;
            uint8_t* decoded = (uint8_t*)alloc_prefer_psram(maxDecoded);
            if (!decoded) { Serial.println("[TTS] Failed to alloc decode buffer"); return false; }
            size_t decodedLen = 0;
            if (mbedtls_base64_decode(decoded, maxDecoded, &decodedLen,
                                      (const unsigned char*)audioB64.c_str(), audioB64.length()) != 0) {
                Serial.println("[TTS] Base64 decode failed");
                heap_caps_free(decoded);
                return false;
            }
            bool played = play_wav_audio(decoded, decodedLen);
            if (!played) played = play_pcm_audio(decoded, decodedLen);
            if (!played) Serial.println("[TTS] Audio playback failed (WAV/PCM)");
            heap_caps_free(decoded);
            return played;
        }

        // Audio response — stream WAV directly to speaker, no SPIFFS
        Serial.printf("[TTS] Audio response, type=%s, streaming to speaker\n",
                      meta.content_type[0] ? meta.content_type : "(unknown)");
        bool played = stream_wav_body_to_speaker(client, meta.chunked, meta.content_length);
        client.stop();
        if (!played) Serial.println("[TTS] WAV streaming failed");
        return played;
    };

    // Try standalone TTS provider first if configured
    if (s_tts_provider) {
        if (tryTtsProvider()) return true;
        Serial.println("[TTS] Standalone TTS provider failed, trying LLM provider fallback...");
    }

    // ── LLM provider TTS (existing behavior) ──
    if (!s_provider || !s_provider->has_tts) {
        if (!s_tts_provider) {
            Serial.printf("[TTS] No TTS provider configured and LLM provider '%s' does not support TTS\n",
                          s_provider ? s_provider->name : "none");
        }
        return false;
    }

    auto tryChatTts = [&](bool addModalities) -> bool {
        JsonDocument doc;
        doc["model"] = s_provider->tts_model;
        JsonArray msgs = doc["messages"].to<JsonArray>();
        JsonObject msg = msgs.add<JsonObject>();
        msg["role"] = "assistant";
        msg["content"] = clipped;
        if (addModalities) {
            JsonArray modalities = doc["modalities"].to<JsonArray>();
            modalities.add("audio");
        }
        JsonObject audio = doc["audio"].to<JsonObject>();
        audio["format"] = "pcm16";
        audio["voice"] = s_provider->tts_voice;
        doc["stream"] = false;

        String bodyStr;
        bodyStr.reserve(1024);
        serializeJson(doc, bodyStr);

        Serial.printf("[TTS] Trying chat-TTS (modalities=%s) via %s%s\n",
                      addModalities ? "yes" : "no", llm_host(), llm_path());

        HttpResponseMeta meta = {};
        char* body = nullptr;
        size_t bodyLen = 0;
        if (!tts_post_json(llm_path(), bodyStr, &meta, &body, &bodyLen)) {
            Serial.println("[TTS] chat-TTS HTTP request failed");
            return false;
        }
        bool ok = tryPlayBody(meta, body, bodyLen);
        if (!ok) Serial.println("[TTS] chat-TTS response parse/playback failed");
        return ok;
    };

    auto tryAudioSpeech = [&]() -> bool {
        JsonDocument doc;
        doc["model"] = s_provider->tts_model;
        doc["input"] = clipped;
        doc["voice"] = s_provider->tts_voice;
        doc["format"] = "pcm16";
        doc["response_format"] = "wav";

        String bodyStr;
        bodyStr.reserve(512);
        serializeJson(doc, bodyStr);

        Serial.printf("[TTS] Trying audio/speech via %s%s\n", llm_host(), s_provider->tts_path);

        HttpResponseMeta meta = {};
        char* body = nullptr;
        size_t bodyLen = 0;
        if (!tts_post_json(s_provider->tts_path, bodyStr, &meta, &body, &bodyLen)) {
            Serial.println("[TTS] audio/speech HTTP request failed");
            return false;
        }
        bool ok = tryPlayBody(meta, body, bodyLen);
        if (!ok) Serial.println("[TTS] audio/speech response parse/playback failed");
        return ok;
    };

    if (tryChatTts(false)) return true;
    if (tryChatTts(true)) return true;
    if (tryAudioSpeech()) return true;

    Serial.println("[TTS] No playable audio in response");
    return false;
}

// ── STT (Speech-to-Text) ──

bool stt_transcribe_file(const char* file_path, char** out_text, size_t* out_len) {
    *out_text = nullptr;
    *out_len = 0;

    const char* sttApiKey = s_stt_api_key[0] ? s_stt_api_key : s_api_key;
    if (!s_stt_provider) {
        Serial.println("[STT] No STT provider configured");
        return false;
    }
    if (!sttApiKey[0]) {
        Serial.println("[STT] No STT API key (and no LLM key to fall back to)");
        return false;
    }
    if (!file_path || !file_path[0]) return false;

    File f = SPIFFS.open(file_path, "r");
    if (!f) {
        Serial.printf("[STT] Cannot open file: %s\n", file_path);
        return false;
    }
    size_t fileSize = f.size();
    if (fileSize == 0) {
        f.close();
        return false;
    }
    Serial.printf("[STT] Transcribing %s (%u bytes) via %s\n",
                  file_path, (unsigned)fileSize, s_stt_provider->name);

    const char* model = stt_current_model();
    Serial.printf("[STT] POST https://%s%s model=%s key=%s\n",
                  s_stt_provider->host, s_stt_provider->stt_path,
                  model, s_stt_api_key[0] ? "explicit" : "LLM-fallback");

    const char* boundary = "----M5ClawSttBoundary";
    const char* crlf = "\r\n";

    // Pre-calculate Content-Length
    size_t part1 = 0;  // --boundary + headers for file field
    part1 += strlen("--") + strlen(boundary) + strlen(crlf);
    part1 += strlen("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"") + strlen(crlf);
    part1 += strlen("Content-Type: audio/wav") + strlen(crlf);
    part1 += strlen(crlf);
    part1 += fileSize;
    part1 += strlen(crlf);

    size_t part2 = 0;  // --boundary + headers for model field
    part2 += strlen("--") + strlen(boundary) + strlen(crlf);
    part2 += strlen("Content-Disposition: form-data; name=\"model\"") + strlen(crlf);
    part2 += strlen(crlf);
    part2 += strlen(model);
    part2 += strlen(crlf);

    size_t part3 = strlen("--") + strlen(boundary) + strlen("--") + strlen(crlf);  // closing boundary

    size_t contentLength = part1 + part2 + part3;

    WiFiClientSecure client;
    if (!secure_connect(client, s_stt_provider->host, 443, "[STT]")) {
        f.close();
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", s_stt_provider->stt_path);
    client.printf("Host: %s\r\n", s_stt_provider->host);
    client.printf("Authorization: Bearer %s\r\n", sttApiKey);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
    client.println("Connection: close");
    client.println();

    // ── Send part 1: file ──
    client.printf("--%s\r\n", boundary);
    client.print("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n");
    client.print("Content-Type: audio/wav\r\n");
    client.print("\r\n");

    uint8_t buf[1024];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        size_t written = 0;
        while (written < n) {
            if (is_aborted()) { f.close(); client.stop(); return false; }
            size_t w = client.write(buf + written, n - written);
            if (w == 0) { delay(1); if (!client.connected()) { f.close(); return false; } continue; }
            written += w;
        }
    }
    f.close();
    client.print("\r\n");

    // ── Send part 2: model ──
    client.printf("--%s\r\n", boundary);
    client.print("Content-Disposition: form-data; name=\"model\"\r\n");
    client.print("\r\n");
    client.print(model);
    client.print("\r\n");

    // ── Send closing boundary ──
    client.printf("--%s--\r\n", boundary);

    // ── Read response ──
    HttpResponseMeta meta = {};
    if (!read_http_headers(client, &meta)) {
        client.stop();
        return false;
    }
    Serial.printf("[STT] Response status=%d type=%s cl=%d\n",
                  meta.status_code,
                  meta.content_type[0] ? meta.content_type : "(unknown)",
                  meta.content_length);

    if (meta.status_code < 200 || meta.status_code >= 300) {
        char* errBody = nullptr;
        size_t errLen = 0;
        if (read_json_body(client, meta.chunked, meta.content_length, &errBody, &errLen, kErrorBodyPreviewMax) && errBody) {
            Serial.printf("[STT] Error body: %.400s\n", errBody);
            heap_caps_free(errBody);
        }
        client.stop();
        return false;
    }

    char* respBody = nullptr;
    size_t respLen = 0;
    if (!read_json_body(client, meta.chunked, meta.content_length, &respBody, &respLen, 256 * 1024)) {
        client.stop();
        return false;
    }
    client.stop();

    if (!respBody || respLen == 0) return false;

    // Parse JSON response: {"text": "..."}
    Serial.printf("[STT] Response body: %.*s\n", (int)(respLen > 600 ? 600 : respLen), respBody);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, respBody, respLen);
    if (err) {
        Serial.printf("[STT] JSON parse error: %s\n", err.c_str());
        heap_caps_free(respBody);
        return false;
    }
    heap_caps_free(respBody);

    const char* text = doc["text"] | "";
    if (!text[0]) {
        Serial.println("[STT] No text in transcription response");
        return false;
    }

    *out_len = strlen(text);
    *out_text = strdup(text);
    Serial.printf("[STT] Transcription: \"%s\"\n", *out_text);
    return true;
}
