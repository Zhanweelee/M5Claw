#include <M5Cardputer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <time.h>
#include "utils.h"
#include "m5claw_config.h"
#include "config.h"
#include "companion.h"
#include "chat.h"
#include "weather_client.h"
#include "agent.h"
#include "llm_client.h"
#include "tool_registry.h"
#include "memory_store.h"
#include "session_mgr.h"
#include "context_builder.h"
#include "message_bus.h"
#include "wechat_bot.h"
#include "cron_service.h"
#include "heartbeat.h"
#include "skill_loader.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/queue.h>
#include "soc/rtc_cntl_reg.h"

#ifndef USER_WIFI_SSID
#define USER_WIFI_SSID ""
#endif
#ifndef USER_WIFI_PASS
#define USER_WIFI_PASS ""
#endif
#ifndef USER_WIFI_SSID2
#define USER_WIFI_SSID2 ""
#endif
#ifndef USER_WIFI_PASS2
#define USER_WIFI_PASS2 ""
#endif
#ifndef USER_PROVIDER
#define USER_PROVIDER ""
#endif
#ifndef USER_PROVIDER_MODEL
#define USER_PROVIDER_MODEL ""
#endif
#ifndef USER_PROVIDER_API_KEY
#define USER_PROVIDER_API_KEY ""
#endif
// Legacy build-time keys for individual providers
#ifndef USER_MIMO_KEY
#define USER_MIMO_KEY ""
#endif
#ifndef USER_DEEPSEEK_KEY
#define USER_DEEPSEEK_KEY ""
#endif
#ifndef USER_ANTHROPIC_KEY
#define USER_ANTHROPIC_KEY ""
#endif
#ifndef USER_OPENAI_KEY
#define USER_OPENAI_KEY ""
#endif
#ifndef USER_MIMO_MODEL
#define USER_MIMO_MODEL ""
#endif
#ifndef USER_CITY
#define USER_CITY ""
#endif
#ifndef USER_ASSISTANT_NAME
#define USER_ASSISTANT_NAME ""
#endif
#ifndef USER_STT_PROVIDER
#define USER_STT_PROVIDER ""
#endif
#ifndef USER_STT_KEY
#define USER_STT_KEY ""
#endif
#ifndef USER_STT_MODEL
#define USER_STT_MODEL ""
#endif

M5Canvas canvas(&M5Cardputer.Display);
Companion companion;
Chat chat;
WeatherClient weatherClient;

enum class AppMode { SETUP, COMPANION, CHAT, WECHAT_STATUS };
static AppMode appMode = AppMode::SETUP;
static bool offlineMode = false;

enum class SetupStep {
    SSID, PASSWORD, PROVIDER, LLM_KEY, LLM_MODEL, CITY, CONNECTING
};
static SetupStep setupStep = SetupStep::SSID;
static String setupInput;

static bool voiceRecording = false;
static unsigned long recordingStartMs = 0;

static int16_t* sttChunkBuf = nullptr;
static bool s_wechatPausedForVoice = false;
static File s_voiceFile;
static size_t s_voicePcmBytes = 0;
static bool s_voiceWriteFailed = false;
static bool s_playTtsForNextLocalReply = false;

static bool speakReplyWithPause(const char* text) {
    bool wechatWasActive = WechatBot::isRunning();
    if (wechatWasActive) {
        WechatBot::stop();
    }
    bool ok = llm_speak_text(text);
    if (wechatWasActive) {
        WechatBot::resume();
    }
    if (!ok) {
        Serial.println("[TTS] Speak failed");
    }
    return ok;
}

void enterSetupMode();
void updateSetupMode();
void handleSetupKey(char key, bool enter, bool backspace, bool tab);
bool tryConnect(const String& ssid, const String& pass);
void connectWiFi();
void initOnlineServices();
void enterCompanionMode();
void enterChatMode();
void startVoiceRecording();
String stopVoiceRecording();
void streamVoiceData();
void processSerialCommands();
void dispatchOutbound();
bool fillBuildTimeDefaults();

static void onAgentResponse(const char* text);
static void onAgentToken(const char* token);
static volatile bool agentResponseReady = false;
static char* agentResponseText = nullptr;
static bool s_discardAgentResponse = false;
static bool s_waitingForWechatResponse = false;
static QueueHandle_t s_tokenQueue = nullptr;
static volatile bool s_hasStreamedTokens = false;

static bool hasPreconfiguredOnlineSettings() {
    return Config::getLlmApiKey().length() > 0
        && Config::getLlmModel().length() > 0;
}

static const char* getBuiltInKeyForProvider(const char* provider) {
    if (!provider || !provider[0]) return USER_PROVIDER_API_KEY[0] ? USER_PROVIDER_API_KEY : nullptr;
    if (strcmp(provider, M5CLAW_PROVIDER_MIMO) == 0 && USER_MIMO_KEY[0]) return USER_MIMO_KEY;
    if (strcmp(provider, M5CLAW_PROVIDER_DEEPSEEK) == 0 && USER_DEEPSEEK_KEY[0]) return USER_DEEPSEEK_KEY;
    if (strcmp(provider, M5CLAW_PROVIDER_ANTHROPIC) == 0 && USER_ANTHROPIC_KEY[0]) return USER_ANTHROPIC_KEY;
    if (strcmp(provider, M5CLAW_PROVIDER_OPENAI) == 0 && USER_OPENAI_KEY[0]) return USER_OPENAI_KEY;
    return USER_PROVIDER_API_KEY[0] ? USER_PROVIDER_API_KEY : nullptr;
}

bool fillBuildTimeDefaults() {
    bool changed = false;

    if (Config::getSSID().length() == 0 && USER_WIFI_SSID[0]) {
        Config::setSSID(String(USER_WIFI_SSID));
        changed = true;
    }
    if (Config::getPassword().length() == 0 && USER_WIFI_PASS[0]) {
        Config::setPassword(String(USER_WIFI_PASS));
        changed = true;
    }
    if (Config::getSSID2().length() == 0 && USER_WIFI_SSID2[0]) {
        Config::setSSID2(String(USER_WIFI_SSID2));
        changed = true;
    }
    if (Config::getPassword2().length() == 0 && USER_WIFI_PASS2[0]) {
        Config::setPassword2(String(USER_WIFI_PASS2));
        changed = true;
    }

    // Provider default
    if (Config::getLlmProvider().length() == 0 && USER_PROVIDER[0]) {
        Config::setLlmProvider(String(USER_PROVIDER));
        changed = true;
    }

    // Model default - use USER_PROVIDER_MODEL first, then legacy USER_MIMO_MODEL
    if (Config::getLlmModel().length() == 0) {
        if (USER_PROVIDER_MODEL[0]) {
            Config::setLlmModel(String(USER_PROVIDER_MODEL));
        } else if (USER_MIMO_MODEL[0]) {
            Config::setLlmModel(String(USER_MIMO_MODEL));
        } else {
            // Look up provider default model
            const LlmProviderInfo* info = llm_provider_by_id(Config::getLlmProvider().c_str());
            if (info && info->default_model[0]) {
                Config::setLlmModel(String(info->default_model));
            }
        }
        if (Config::getLlmModel().length() > 0) changed = true;
    }

    if (Config::getCity().length() == 0 && USER_CITY[0]) {
        Config::setCity(String(USER_CITY));
        changed = true;
    }

    // Assistant name
    if (Config::getAssistantName().length() == 0 && USER_ASSISTANT_NAME[0]) {
        Config::setAssistantName(String(USER_ASSISTANT_NAME));
        changed = true;
    }

    // STT build-time defaults
    if (Config::getSttProvider().length() == 0 && USER_STT_PROVIDER[0]) {
        Config::setSttProvider(String(USER_STT_PROVIDER));
        changed = true;
    }
    if (Config::getSttApiKey().length() == 0 && USER_STT_KEY[0]) {
        Config::setSttApiKey(String(USER_STT_KEY));
        changed = true;
    }
    if (Config::getSttModel().length() == 0 && USER_STT_MODEL[0]) {
        Config::setSttModel(String(USER_STT_MODEL));
        changed = true;
    }

    // Built-in API key for the current provider
    if (Config::getLlmApiKey().length() == 0) {
        const char* providerId = Config::getLlmProvider().c_str();
        const char* builtInKey = getBuiltInKeyForProvider(providerId);
        if (builtInKey) {
            Config::setTransientLlmApiKey(String(builtInKey));
            Serial.printf("[CONFIG] Using built-in key for provider '%s'\n", providerId);
        }
    }

    // TTS build-time defaults
    if (Config::getTtsProvider().length() == 0 && USER_TTS_PROVIDER[0]) {
        Config::setTtsProvider(String(USER_TTS_PROVIDER));
        changed = true;
    }
    if (Config::getTtsApiKey().length() == 0 && USER_TTS_KEY[0]) {
        Config::setTtsApiKey(String(USER_TTS_KEY));
        changed = true;
    }
    if (Config::getTtsModel().length() == 0 && USER_TTS_MODEL[0]) {
        Config::setTtsModel(String(USER_TTS_MODEL));
        changed = true;
    }
    if (Config::getTtsVoice().length() == 0 && USER_TTS_VOICE[0]) {
        Config::setTtsVoice(String(USER_TTS_VOICE));
        changed = true;
    }
    return changed;
}

static bool isSensitiveNvsKey(const char* key) {
    return strcmp(key, "pass") == 0
        || strcmp(key, "llm_key") == 0
        || strcmp(key, "tts_key") == 0
        || strcmp(key, "stt_key") == 0
        || strcmp(key, "wc_token") == 0;
}

// ── M5Burner NVS Configure protocol ──────────────────────────
static const char* const NVS_KEYS[] = {
    "ssid", "pass", "llm_provider", "llm_key", "llm_model",
    "city", "assistant_name",
    "tts_provider", "tts_key", "tts_model", "tts_voice",
    "stt_provider", "stt_key", "stt_model",
    "wc_token", "wc_host"
};

static String nvsGet(const char* key) {
    if (isSensitiveNvsKey(key)) return "";
    if (strcmp(key, "ssid") == 0)      return Config::getSSID();
    if (strcmp(key, "pass") == 0)      return Config::getPassword();
    if (strcmp(key, "llm_provider") == 0) return Config::getLlmProvider();
    if (strcmp(key, "llm_key") == 0)   return Config::getLlmApiKey();
    if (strcmp(key, "llm_model") == 0) return Config::getLlmModel();
    if (strcmp(key, "city") == 0)      return Config::getCity();
    if (strcmp(key, "assistant_name") == 0) return Config::getAssistantName();
    if (strcmp(key, "tts_provider") == 0) return Config::getTtsProvider();
    if (strcmp(key, "tts_key") == 0)   return Config::getTtsApiKey();
    if (strcmp(key, "tts_model") == 0) return Config::getTtsModel();
    if (strcmp(key, "tts_voice") == 0) return Config::getTtsVoice();
    if (strcmp(key, "stt_provider") == 0) return Config::getSttProvider();
    if (strcmp(key, "stt_key") == 0)   return Config::getSttApiKey();
    if (strcmp(key, "stt_model") == 0) return Config::getSttModel();
    if (strcmp(key, "wc_token") == 0)  return Config::getWechatToken();
    if (strcmp(key, "wc_host") == 0)   return Config::getWechatApiHost();
    return "";
}

static void nvsSet(const char* key, const char* value) {
    if (strcmp(key, "ssid") == 0)      Config::setSSID(value);
    else if (strcmp(key, "pass") == 0)      Config::setPassword(value);
    else if (strcmp(key, "llm_provider") == 0) Config::setLlmProvider(value);
    else if (strcmp(key, "llm_key") == 0)   Config::setLlmApiKey(value);
    else if (strcmp(key, "llm_model") == 0) Config::setLlmModel(value);
    else if (strcmp(key, "city") == 0)      Config::setCity(value);
    else if (strcmp(key, "assistant_name") == 0) Config::setAssistantName(value);
    else if (strcmp(key, "tts_provider") == 0) Config::setTtsProvider(value);
    else if (strcmp(key, "tts_key") == 0)   Config::setTtsApiKey(value);
    else if (strcmp(key, "tts_model") == 0) Config::setTtsModel(value);
    else if (strcmp(key, "tts_voice") == 0) Config::setTtsVoice(value);
    else if (strcmp(key, "stt_provider") == 0) Config::setSttProvider(value);
    else if (strcmp(key, "stt_key") == 0)   Config::setSttApiKey(value);
    else if (strcmp(key, "stt_model") == 0) Config::setSttModel(value);
    else if (strcmp(key, "wc_token") == 0)  Config::setWechatToken(value);
    else if (strcmp(key, "wc_host") == 0)   Config::setWechatApiHost(value);
    Config::save();
}

static bool handleBurnerNVS(const String& line) {
    if (!line.startsWith("CMD::")) return false;

    if (line.startsWith("CMD::INIT:")) {
        Serial.println("__NVS_EXIST__");
    } else if (line.startsWith("CMD::LIST:")) {
        String keys;
        for (auto k : NVS_KEYS) { keys += k; keys += '/'; }
        Serial.println(keys);
    } else if (line.startsWith("CMD::GET:")) {
        String key = line.substring(9);
        if (isSensitiveNvsKey(key.c_str())) Serial.println("__PROTECTED__");
        else Serial.println(nvsGet(key.c_str()));
    } else if (line.startsWith("CMD::SET:")) {
        String payload = line.substring(9);
        int eq = payload.indexOf('=');
        if (eq > 0) {
            String key = payload.substring(0, eq);
            String val = payload.substring(eq + 1);
            nvsSet(key.c_str(), val.c_str());
        }
        Serial.println("OK");
    }
    return true;
}

// ── Serial CLI ───────────────────────────────────────────────
void processSerialCommands() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (handleBurnerNVS(line)) return;

    int sep = line.indexOf(' ');
    String cmd = (sep > 0) ? line.substring(0, sep) : line;
    String val = (sep > 0) ? line.substring(sep + 1) : "";
    val.trim();

    if (cmd == "help") {
        Serial.println("=== M5Claw Serial Config ===");
        Serial.println("  set_wifi <ssid> <pass>    - Set primary WiFi");
        Serial.println("  set_wifi2 <ssid> <pass>   - Set backup WiFi");
        Serial.println("  set_provider <id>         - Set LLM provider (mimo/deepseek/openai/custom)");
        Serial.println("  set_llm_key <key>         - Set API key for current provider");
        Serial.println("  set_llm_model <model>     - Set model name");
        Serial.println("  set_tts_provider <id>     - Set TTS provider (siliconflow / empty=use LLM)");
        Serial.println("  set_tts_key <key>         - Set TTS API key (empty=use LLM key)");
        Serial.println("  set_tts_model <model>     - Set TTS model override");
        Serial.println("  set_tts_voice <voice>     - Set TTS voice override");
        Serial.println("  set_stt_provider <id>    - Set STT provider (siliconflow / empty=disable)");
        Serial.println("  set_stt_key <key>        - Set STT API key");
        Serial.println("  set_stt_model <model>    - Set STT model override");
        Serial.println("  set_city <city>           - e.g. Beijing");
        Serial.println("  set_assistant_name <name> - Set assistant identity name");
        Serial.println("  set_wechat <token> <host> - Set WeChat bot credentials");
        Serial.println("  show_config               - Show current config");
        Serial.println("  list_providers            - List supported providers");
        Serial.println("  reset_config              - Clear all config");
        Serial.println("  reboot                    - Restart device");
    } else if (cmd == "set_wifi") {
        int sp = val.indexOf(' ');
        if (sp > 0) {
            Config::setSSID(val.substring(0, sp));
            Config::setPassword(val.substring(sp + 1));
            Config::save();
            Serial.printf("WiFi 1 set: %s\n", Config::getSSID().c_str());
        } else {
            Serial.println("Usage: set_wifi <ssid> <password>");
        }
    } else if (cmd == "set_wifi2") {
        int sp = val.indexOf(' ');
        if (sp > 0) {
            Config::setSSID2(val.substring(0, sp));
            Config::setPassword2(val.substring(sp + 1));
            Config::save();
            Serial.printf("WiFi 2 set: %s\n", Config::getSSID2().c_str());
        } else {
            Serial.println("Usage: set_wifi2 <ssid> <password>");
        }
    } else if (cmd == "set_provider") {
        const LlmProviderInfo* info = llm_provider_by_id(val.c_str());
        if (info) {
            Config::setLlmProvider(val);
            // Auto-fill model from provider default
            if (info->default_model[0]) {
                Config::setLlmModel(String(info->default_model));
            }
            // Auto-fill key from built-in if available
            const char* builtInKey = getBuiltInKeyForProvider(val.c_str());
            if (builtInKey) {
                Config::setTransientLlmApiKey(String(builtInKey));
                Serial.printf("Provider: %s (model=%s, built-in key loaded)\n", info->name, info->default_model);
            } else {
                Serial.printf("Provider: %s (model=%s, enter key manually)\n", info->name, info->default_model);
            }
            Config::save();
            // Re-init LLM client immediately with new provider
            llm_client_init(Config::getLlmApiKey().c_str(),
                            Config::getLlmModel().c_str(),
                            Config::getLlmProvider().c_str(),
                            nullptr, nullptr);
        } else {
            Serial.printf("Unknown provider '%s'. Use: mimo, deepseek, openai, custom\n", val.c_str());
            Serial.println("Use 'list_providers' to see all options.");
        }
    } else if (cmd == "list_providers") {
        Serial.println("=== Supported Providers ===");
        for (int i = 0; i < llm_provider_count(); i++) {
            const LlmProviderInfo* p = llm_provider_by_index(i);
            const char* hasTts = p->has_tts ? " [TTS]" : "";
            const char* hasSearch = p->has_web_search ? " [Search]" : "";
            const char* hasAudioIn = p->supports_audio_input ? " [AudioIn]" : "";
            Serial.printf("  %-10s - %s  host=%s  model=%s%s%s%s\n",
                          p->id, p->name, p->host[0] ? p->host : "(user-defined)",
                          p->default_model[0] ? p->default_model : "(user-defined)",
                          hasTts, hasSearch, hasAudioIn);
        }
        Serial.println();
        Serial.println("=== TTS Providers ===");
        for (int i = 0; i < tts_provider_count(); i++) {
            const TtsProviderInfo* tp = tts_provider_by_index(i);
            Serial.printf("  %-14s - %s\n", tp->id, tp->name);
            Serial.printf("    host=%s\n", tp->host);
            Serial.printf("    default model=%s\n", tp->tts_model);
            Serial.printf("    default voice=%s\n", tp->tts_voice);
            if (strcmp(tp->id, M5CLAW_TTS_PROVIDER_SILICONFLOW) == 0) {
                Serial.println("    --- Available models ---");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B (multi-lang, emotion)");
                Serial.println("    fnlp/MOSS-TTSD-v0.5         (expressive, voice clone)");
                Serial.println("    --- Available voices (CosyVoice2) ---");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:alex      (calm male)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:benjamin  (deep male)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:charles   (magnetic male)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:david     (cheerful male)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:anna      (calm female)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:bella     (passionate female)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:claire    (gentle female)");
                Serial.println("    FunAudioLLM/CosyVoice2-0.5B:diana     (cheerful female)");
            }
        }
        const char* curModel = Config::getTtsModel().length() > 0
                               ? Config::getTtsModel().c_str() : "(default)";
        const char* curVoice = Config::getTtsVoice().length() > 0
                               ? Config::getTtsVoice().c_str() : "(default)";
        Serial.printf("Current TTS: %s  model=%s  voice=%s\n",
                      Config::getTtsProvider().length() > 0
                          ? Config::getTtsProvider().c_str() : "(LLM provider)",
                      curModel, curVoice);
        Serial.printf("Current LLM: %s\n", Config::getLlmProvider().c_str());
        Serial.println();
        Serial.println("=== STT Providers ===");
        for (int i = 0; i < stt_provider_count(); i++) {
            const SttProviderInfo* sp = stt_provider_by_index(i);
            Serial.printf("  %-14s - %s\n", sp->id, sp->name);
            Serial.printf("    host=%s\n", sp->host);
            Serial.printf("    default model=%s\n", sp->stt_model);
            if (strcmp(sp->id, M5CLAW_STT_PROVIDER_SILICONFLOW) == 0) {
                Serial.println("    --- Available models ---");
                Serial.println("    FunAudioLLM/SenseVoiceSmall  (multi-lang, fast)");
                Serial.println("    TeleAI/TeleSpeechASR         (high accuracy)");
            }
        }
        Serial.printf("Current STT: %s\n",
                      Config::getSttProvider().length() > 0
                          ? Config::getSttProvider().c_str() : "(disabled)");
    } else if (cmd == "set_mimo_key" || cmd == "set_llm_key") {
        Config::setLlmApiKey(val); Config::save();
        llm_client_init(Config::getLlmApiKey().c_str(),
                        Config::getLlmModel().c_str(),
                        Config::getLlmProvider().c_str(),
                        nullptr, nullptr);
        Serial.printf("LLM key saved (%d chars)\n", val.length());
    } else if (cmd == "set_mimo_model" || cmd == "set_llm_model") {
        Config::setLlmModel(val); Config::save();
        llm_client_init(Config::getLlmApiKey().c_str(),
                        Config::getLlmModel().c_str(),
                        Config::getLlmProvider().c_str(),
                        nullptr, nullptr);
        Serial.printf("Model: %s\n", val.c_str());
    } else if (cmd == "set_tts_provider") {
        const TtsProviderInfo* ttsInfo = tts_provider_by_id(val.c_str());
        if (val.length() == 0 || ttsInfo) {
            Config::setTtsProvider(val);
            Config::save();
            tts_client_init(Config::getTtsProvider().c_str(),
                           Config::getTtsApiKey().c_str(),
                           Config::getTtsModel().c_str(),
                           Config::getTtsVoice().c_str());
            if (val.length() == 0) {
                Serial.println("TTS provider cleared (will use LLM provider TTS)");
            } else {
                Serial.printf("TTS provider: %s (model=%s, voice=%s)\n",
                              ttsInfo->name, ttsInfo->tts_model, ttsInfo->tts_voice);
            }
        } else {
            Serial.printf("Unknown TTS provider '%s'. Use: siliconflow\n", val.c_str());
            Serial.println("Use 'list_providers' to see all options.");
        }
    } else if (cmd == "set_tts_key") {
        Config::setTtsApiKey(val); Config::save();
        tts_client_init(Config::getTtsProvider().c_str(),
                       Config::getTtsApiKey().c_str(),
                       Config::getTtsModel().c_str(),
                       Config::getTtsVoice().c_str());
        Serial.printf("TTS key saved (%d chars)\n", val.length());
    } else if (cmd == "set_tts_model") {
        Config::setTtsModel(val); Config::save();
        tts_client_init(Config::getTtsProvider().c_str(),
                       Config::getTtsApiKey().c_str(),
                       Config::getTtsModel().c_str(),
                       Config::getTtsVoice().c_str());
        Serial.printf("TTS model: %s\n", val.length() > 0 ? val.c_str() : "(provider default)");
    } else if (cmd == "set_tts_voice") {
        Config::setTtsVoice(val); Config::save();
        tts_client_init(Config::getTtsProvider().c_str(),
                       Config::getTtsApiKey().c_str(),
                       Config::getTtsModel().c_str(),
                       Config::getTtsVoice().c_str());
        Serial.printf("TTS voice: %s\n", val.length() > 0 ? val.c_str() : "(provider default)");
    } else if (cmd == "set_stt_provider") {
        const SttProviderInfo* sttInfo = stt_provider_by_id(val.c_str());
        if (val.length() == 0 || sttInfo) {
            Config::setSttProvider(val);
            Config::save();
            stt_client_init(Config::getSttProvider().c_str(),
                           Config::getSttApiKey().c_str(),
                           Config::getSttModel().c_str());
            if (val.length() == 0) {
                Serial.println("STT provider cleared (voice input disabled)");
            } else {
                Serial.printf("STT provider: %s (model=%s)\n",
                              sttInfo->name, sttInfo->stt_model);
            }
        } else {
            Serial.printf("Unknown STT provider '%s'. Use: siliconflow\n", val.c_str());
            Serial.println("Use 'list_providers' to see all options.");
        }
    } else if (cmd == "set_stt_key") {
        Config::setSttApiKey(val); Config::save();
        stt_client_init(Config::getSttProvider().c_str(),
                       Config::getSttApiKey().c_str(),
                       Config::getSttModel().c_str());
        Serial.printf("STT key saved (%d chars)\n", val.length());
    } else if (cmd == "set_stt_model") {
        Config::setSttModel(val); Config::save();
        stt_client_init(Config::getSttProvider().c_str(),
                       Config::getSttApiKey().c_str(),
                       Config::getSttModel().c_str());
        Serial.printf("STT model: %s\n", val.length() > 0 ? val.c_str() : "(provider default)");
    } else if (cmd == "set_city") {
        Config::setCity(val); Config::save();
        Serial.printf("City: %s\n", val.c_str());
    } else if (cmd == "set_assistant_name") {
        Config::setAssistantName(val); Config::save();
        Serial.printf("Assistant name: %s\n", val.c_str());
    } else if (cmd == "set_wechat") {
        int sp = val.indexOf(' ');
        if (sp > 0) {
            Config::setWechatToken(val.substring(0, sp));
            Config::setWechatApiHost(val.substring(sp + 1));
            Config::save();
            Serial.println("WeChat credentials saved. Reboot to activate.");
        } else {
            Serial.println("Usage: set_wechat <bearer_token> <api_host>");
        }
    } else if (cmd == "show_config") {
        const LlmProviderInfo* info = llm_provider_by_id(Config::getLlmProvider().c_str());
        Serial.println("=== Current Config ===");
        Serial.printf("  WiFi SSID:     %s\n", Config::getSSID().c_str());
        Serial.printf("  WiFi Pass:     [%d chars]\n", Config::getPassword().length());
        if (Config::getSSID2().length() > 0) {
            Serial.printf("  WiFi2 SSID:    %s\n", Config::getSSID2().c_str());
            Serial.printf("  WiFi2 Pass:    [%d chars]\n", Config::getPassword2().length());
        }
        Serial.printf("  Provider:      %s (%s)\n", Config::getLlmProvider().c_str(),
                      info ? info->name : "unknown");
        Serial.printf("  LLM Model:     %s\n", Config::getLlmModel().c_str());
        Serial.printf("  LLM Key:       [%d chars]\n", Config::getLlmApiKey().length());
        if (Config::getTtsProvider().length() > 0 || Config::getTtsApiKey().length() > 0) {
            const TtsProviderInfo* ttsInfo = tts_provider_by_id(Config::getTtsProvider().c_str());
            Serial.printf("  TTS Provider:  %s (%s)\n", Config::getTtsProvider().c_str(),
                          ttsInfo ? ttsInfo->name : "unknown");
            Serial.printf("  TTS Key:       [%d chars]\n", Config::getTtsApiKey().length());
            if (Config::getTtsModel().length() > 0)
                Serial.printf("  TTS Model:     %s\n", Config::getTtsModel().c_str());
            if (Config::getTtsVoice().length() > 0)
                Serial.printf("  TTS Voice:     %s\n", Config::getTtsVoice().c_str());
        }
        Serial.printf("  City:          %s\n", Config::getCity().c_str());
        Serial.printf("  Ast Name:      %s\n", Config::getAssistantName().length() > 0
                      ? Config::getAssistantName().c_str() : "(default)");
        if (Config::getSttProvider().length() > 0 || Config::getSttApiKey().length() > 0) {
            const SttProviderInfo* sttInfo = stt_provider_by_id(Config::getSttProvider().c_str());
            Serial.printf("  STT Provider:  %s (%s)\n", Config::getSttProvider().c_str(),
                          sttInfo ? sttInfo->name : "unknown");
            Serial.printf("  STT Key:       [%d chars]\n", Config::getSttApiKey().length());
            if (Config::getSttModel().length() > 0)
                Serial.printf("  STT Model:     %s\n", Config::getSttModel().c_str());
        }
        Serial.printf("  WeChat Token:  [%d chars]\n", Config::getWechatToken().length());
        Serial.printf("  WeChat Host:   %s\n", Config::getWechatApiHost().c_str());
        Serial.printf("  Valid:         %s\n", Config::isValid() ? "YES" : "NO");
    } else if (cmd == "reset_config") {
        Config::reset(); Config::save();
        Serial.println("Config cleared. Reboot to enter setup.");
    } else if (cmd == "reboot") {
        Serial.println("Rebooting...");
        delay(100);
        ESP.restart();
    } else {
        Serial.printf("Unknown command: %s (type 'help')\n", cmd.c_str());
    }
}

void dispatchOutbound() {
    if (WiFi.status() != WL_CONNECTED) return;
    BusMessage msg;
    while (MessageBus::popOutbound(&msg, 0)) {
        if (strcmp(msg.channel, M5CLAW_CHAN_WECHAT) == 0) {
            if (WechatBot::isPaired()) {
                WechatBot::sendMessage(msg.chat_id, msg.content);
            } else {
                Serial.printf("[BUS] WeChat offline, dropped reply to %s\n", msg.chat_id);
            }
        }
        free(msg.content);
    }
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    esp_log_level_set("ssl_client", ESP_LOG_NONE);

    auto cfg = M5.config();
    cfg.output_power = true;
    M5Cardputer.begin(cfg, true);

    Serial.begin(115200);
    delay(500);
    Serial.println("[BOOT] M5Claw starting...");

    int32_t battLevel = M5Cardputer.Power.getBatteryLevel();
    int16_t battVolt = M5Cardputer.Power.getBatteryVoltage();
    Serial.printf("[POWER] Board=%d BattLevel=%d%% BattVolt=%dmV\n",
                  (int)M5.getBoard(), battLevel, battVolt);
    if (battVolt < 3000) {
        Serial.println("[POWER] WARNING: Low/no battery. Turn ON the battery switch on the side of M5Cardputer.");
    }

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(80);
    canvas.createSprite(SCREEN_W, SCREEN_H);
    canvas.setTextWrap(false);

    canvas.fillScreen(Color::BLACK);
    canvas.setTextColor(Color::WHITE);
    canvas.setTextSize(1);
    canvas.drawString("M5Claw booting...", 60, 60);
    canvas.pushSprite(0, 0);
    Serial.println("[BOOT] Display OK");

    Config::load();
    MemoryStore::init();
    Config::importBootstrapFile();
    bool configChanged = fillBuildTimeDefaults();
    configChanged |= Config::applyDefaults();
    if (configChanged) {
        Config::save();
    }
    Serial.println("[BOOT] Config loaded");
    Serial.println("[BOOT] Type 'help' in serial monitor for config commands");
    Serial.println("[BOOT] SPIFFS OK");

    MessageBus::init();
    SessionMgr::init();
    ToolRegistry::init();
    SkillLoader::init();
    CronService::init();
    Heartbeat::init();
    WechatBot::init();
    Agent::init();
    s_tokenQueue = xQueueCreate(32, sizeof(char*));
    Serial.println("[BOOT] All services initialized");

    playBootAnimation(canvas);

    if (Config::isValid()) {
        connectWiFi();
    } else {
        enterSetupMode();
    }
}

void loop() {
    M5Cardputer.update();

    // Drain streaming tokens from agent → chat UI
    if (s_tokenQueue) {
        char* token;
        while (xQueueReceive(s_tokenQueue, &token, 0) == pdTRUE) {
            if (!s_discardAgentResponse) {
                chat.appendAIToken(token);
            }
            free(token);
        }
    }

    // WeChat incoming → show one at a time, queue the rest
    if (WechatBot::hasIncomingForDisplay() && !s_waitingForWechatResponse) {
        char* incoming = WechatBot::takeIncomingForDisplay();
        if (incoming) {
            if (appMode != AppMode::CHAT) {
                appMode = AppMode::CHAT;
                chat.begin(canvas);
            }
            String label = "[wechat] ";
            label += incoming;
            chat.addMessage(label, true);
            chat.addMessage("thinking...", false);
            chat.scrollToBottom();
            s_waitingForWechatResponse = true;
            free(incoming);
        }
    }

    // Local agent response (from keyboard/voice)
    if (agentResponseReady && agentResponseText) {
        if (s_discardAgentResponse) {
            Serial.println("[MAIN] Discarding cancelled agent response");
            s_discardAgentResponse = false;
        } else {
            if (!s_hasStreamedTokens) {
                chat.appendAIToken(agentResponseText);
            }
            chat.onAIResponseComplete();

            bool shouldSpeak = s_playTtsForNextLocalReply && agentResponseText[0];
            char ttsText[M5CLAW_TTS_TEXT_MAX * 4 + 1] = {0};
            if (shouldSpeak) {
                strlcpy(ttsText, agentResponseText, sizeof(ttsText));
            }

            free(agentResponseText);
            agentResponseText = nullptr;
            agentResponseReady = false;
            s_hasStreamedTokens = false;
            s_playTtsForNextLocalReply = false;

            if (shouldSpeak) {
                delay(20);
                if (!speakReplyWithPause(ttsText)) {
                    companion.triggerIdle();
                }
            } else {
                companion.triggerIdle();
            }

            return;
        }
        free(agentResponseText);
        agentResponseText = nullptr;
        agentResponseReady = false;
        s_hasStreamedTokens = false;
        s_playTtsForNextLocalReply = false;
    }

    // External AI response (wechat reply / cron notification / heartbeat)
    if (Agent::hasExternalConv()) {
        ExternalConv conv = Agent::takeExternalConv();
        if (s_discardAgentResponse) {
            Serial.println("[MAIN] Discarding cancelled external conv");
            s_discardAgentResponse = false;
        } else if (conv.aiText) {
            if (appMode != AppMode::CHAT) {
                appMode = AppMode::CHAT;
                chat.begin(canvas);
            }
            if (s_waitingForWechatResponse) {
                chat.appendAIToken(conv.aiText);
                chat.onAIResponseComplete();
            } else if (conv.userText) {
                String label = "[";
                label += conv.channel;
                label += "] ";
                label += conv.userText;
                chat.addMessage(label, true);
                chat.addMessage(conv.aiText, false);
                chat.scrollToBottom();
            } else {
                chat.addMessage(conv.aiText, false);
                chat.scrollToBottom();
            }
        }
        s_waitingForWechatResponse = false;
        free(conv.userText);
        free(conv.aiText);
    }

    dispatchOutbound();

    switch (appMode) {
        case AppMode::SETUP:
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto keys = M5Cardputer.Keyboard.keysState();
                bool enter = keys.enter;
                bool backspace = keys.del;
                bool tab = keys.tab;
                char key = 0;
                if (keys.word.size() > 0) key = keys.word[0];
                handleSetupKey(key, enter, backspace, tab);
            }
            updateSetupMode();
            break;

        case AppMode::COMPANION: {
            auto ks = M5Cardputer.Keyboard.keysState();
            bool keyPressed = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();

            static bool prevFn = false;
            static bool fnComboUsed = false;

            bool fnDown = ks.fn && !prevFn;
            bool fnUp   = !ks.fn && prevFn;

            if (fnDown) fnComboUsed = false;

            if (keyPressed) {
                if (ks.tab) {
                    prevFn = ks.fn;
                    playTransition(canvas, true);
                    enterChatMode();
                    break;
                }
                if (ks.ctrl) {
                    prevFn = ks.fn;
                    playTransitionVertical(canvas, true);
                    appMode = AppMode::WECHAT_STATUS;
                    break;
                }
                if (ks.fn && ks.word.size() > 0) {
                    fnComboUsed = true;
                    if (ks.word[0] == 'r') {
                        WiFi.disconnect(true);
                        Config::setSSID("");
                        Config::setPassword("");
                        Config::save();
                        prevFn = ks.fn;
                        enterSetupMode();
                        break;
                    }
                }
                if (!ks.fn) Companion::playKeyClick();
            }

            if (fnUp && !fnComboUsed) {
                companion.cycleSunset();
                Companion::playKeyClick();
            }

            prevFn = ks.fn;

            if (!offlineMode) weatherClient.update();
            companion.setWeather(weatherClient.getData());
            companion.update(canvas);
            companion.drawNotificationOverlay(canvas);
            canvas.pushSprite(0, 0);
            break;
        }

        case AppMode::CHAT: {
            auto ks = M5Cardputer.Keyboard.keysState();
            static bool pFn = false, pEnter = false, pDel = false, pTab = false;
            static bool pAlt = false, pCtrl = false;
            static char pWordChar = 0;
            static unsigned long fnHoldStartMs = 0;
            static bool fnRecordTriggered = false;
            bool didBlock = false;

            bool fnDown = ks.fn && !pFn;
            bool fnUp = !ks.fn && pFn;
            bool fnAlone = ks.fn && ks.word.size() == 0
                           && !ks.tab && !ks.enter && !ks.del;

            if (fnUp) {
                fnHoldStartMs = 0;
                fnRecordTriggered = false;
            } else if (fnAlone) {
                if (fnDown || fnHoldStartMs == 0) {
                    fnHoldStartMs = millis();
                }
            } else {
                fnHoldStartMs = 0;
                fnRecordTriggered = false;
            }

            if (!offlineMode && fnAlone && !fnRecordTriggered && !voiceRecording
                && !Agent::isBusy() && !chat.isWaitingForAI()
                && fnHoldStartMs != 0 && millis() - fnHoldStartMs >= 120) {
                startVoiceRecording();
                fnRecordTriggered = voiceRecording;
            }
            if (fnUp && voiceRecording) {
                String audioPath = stopVoiceRecording();
                if (!offlineMode && audioPath.length() > 0) {
                    chat.addMessage("[voice]", true);
                    chat.addMessage("thinking...", false);
                    s_hasStreamedTokens = false;
                    s_playTtsForNextLocalReply = true;
                    Agent::sendVoiceMessage(audioPath.c_str(), "audio/wav", onAgentResponse, onAgentToken);
                } else if (audioPath.length() > 0) {
                    SPIFFS.remove(audioPath.c_str());
                }
                fnHoldStartMs = 0;
                fnRecordTriggered = false;
                didBlock = true;
            }

            if (!didBlock && voiceRecording) {
                streamVoiceData();
                if (millis() - recordingStartMs >= M5CLAW_AUDIO_MAX_SECONDS * 1000UL) {
                    String audioPath = stopVoiceRecording();
                    if (!offlineMode && audioPath.length() > 0) {
                        chat.addMessage("[voice]", true);
                        chat.addMessage("thinking...", false);
                        s_hasStreamedTokens = false;
                        s_playTtsForNextLocalReply = true;
                        Agent::sendVoiceMessage(audioPath.c_str(), "audio/wav", onAgentResponse, onAgentToken);
                    } else if (audioPath.length() > 0) {
                        SPIFFS.remove(audioPath.c_str());
                    }
                    didBlock = true;
                }
            }

            if (didBlock) {
                M5Cardputer.update();
                ks = M5Cardputer.Keyboard.keysState();
            }

            bool enterDown = !didBlock && ks.enter && !pEnter;
            bool delDown   = !didBlock && ks.del && !pDel;
            bool tabDown   = !didBlock && ks.tab && !pTab;
            bool altDown   = !didBlock && ks.alt && !pAlt;
            bool ctrlDown  = !didBlock && ks.ctrl && !pCtrl;
            char curWordChar = (ks.word.size() > 0) ? ks.word[0] : 0;
            bool charDown  = !didBlock && curWordChar != 0 && curWordChar != pWordChar;

            pFn = ks.fn; pEnter = ks.enter; pDel = ks.del; pTab = ks.tab;
            pAlt = ks.alt; pCtrl = ks.ctrl;
            pWordChar = curWordChar;

            if (!voiceRecording) {
                // Fn+C: cancel ongoing agent generation
                if (ks.fn && charDown && curWordChar == 'c'
                    && (Agent::isBusy() || chat.isWaitingForAI())) {
                    Serial.println("[MAIN] Fn+C: cancelling generation");
                    Agent::requestAbort();
                    chat.cancelWaiting();
                    s_discardAgentResponse = true;
                    if (s_waitingForWechatResponse) {
                        s_waitingForWechatResponse = false;
                    }
                } else if (altDown) {
                    playTransition(canvas, false);
                    enterCompanionMode();
                    break;
                } else if (tabDown) {
                    chat.scrollUp();
                } else if (ctrlDown) {
                    if (chat.isAtBottom()) {
                        playTransition(canvas, false);
                        enterCompanionMode();
                        break;
                    }
                    chat.scrollDown();
                } else if (enterDown) {
                    chat.handleEnter();
                } else if (delDown) {
                    chat.handleBackspace();
                } else if (charDown && !ks.fn) {
                    chat.handleKey(ks.word[0]);
                }
            }

            if (chat.hasPendingMessage() && !Agent::isBusy()) {
                if (offlineMode) {
                    String msg = chat.takePendingMessage();
                    chat.appendAIToken("[Offline] No network");
                    chat.onAIResponseComplete();
                } else {
                    String msg = chat.takePendingMessage();
                    Serial.printf("[CHAT] Sending: %s\n", msg.c_str());
                    companion.triggerTalk();

                    s_hasStreamedTokens = false;
                    s_playTtsForNextLocalReply = false;
                    Agent::sendMessage(msg.c_str(), onAgentResponse, onAgentToken);

                    M5Cardputer.update();
                    ks = M5Cardputer.Keyboard.keysState();
                    pFn = ks.fn; pEnter = ks.enter; pDel = ks.del; pTab = ks.tab;
                    pAlt = ks.alt; pCtrl = ks.ctrl;
                    pWordChar = (ks.word.size() > 0) ? ks.word[0] : 0;
                    enterDown = delDown = tabDown = altDown = ctrlDown = charDown = false;
                }
            }

            chat.update(canvas);
            if (voiceRecording) {
                float dur = (float)(millis() - recordingStartMs) / 1000.0f;
                canvas.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, rgb565(200, 50, 50));
                canvas.setTextColor(Color::WHITE);
                canvas.setTextSize(1);
                char recLabel[40];
                snprintf(recLabel, sizeof(recLabel), "Recording... %.1fs", dur);
                canvas.drawString(recLabel, 62, SCREEN_H - 12);
            } else if (Agent::isBusy()) {
                canvas.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, rgb565(80, 80, 80));
                canvas.setTextColor(rgb565(200, 200, 200));
                canvas.setTextSize(1);
                canvas.drawString("Processing...", 78, SCREEN_H - 12);
            }
            canvas.pushSprite(0, 0);
            break;
        }

        case AppMode::WECHAT_STATUS: {
            static bool pairStarted = false;
            static unsigned long lastPollMs = 0;
            static const char* qrContent = nullptr;

            auto ks = M5Cardputer.Keyboard.keysState();
            bool keyPressed = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();

            if (keyPressed) {
                if (ks.tab) {
                    WechatBot::cancelPairing();
                    pairStarted = false;
                    qrContent = nullptr;
                    playTransitionVertical(canvas, false);
                    enterCompanionMode();
                    break;
                }
                if (!WechatBot::isPaired() && ks.enter) {
                    pairStarted = false;
                    qrContent = nullptr;
                }
            }

            if (!WechatBot::isPaired() && !pairStarted) {
                qrContent = WechatBot::startPairing();
                pairStarted = true;
                lastPollMs = millis();
            }

            if (WechatBot::getPairState() == WechatBot::PAIR_WAITING
                && millis() - lastPollMs > 3000) {
                if (WechatBot::pollPairing()) {
                    WechatBot::start();
                    pairStarted = false;
                    qrContent = nullptr;
                }
                lastPollMs = millis();
            }

            canvas.fillScreen(Color::BG_DAY);
            canvas.setTextSize(1);

            constexpr int BAR_H = 16;
            canvas.fillRect(0, 0, SCREEN_W, BAR_H, Color::INPUT_BG);
            canvas.drawFastHLine(0, BAR_H - 1, SCREEN_W, Color::GROUND_TOP);
            canvas.setTextColor(Color::STATUS_DIM);
            canvas.drawString("[Tab]back", 3, 2);
            if (!WechatBot::isPaired() && WechatBot::getPairState() != WechatBot::PAIR_IDLE) {
                canvas.drawString("[Enter]retry", SCREEN_W - 78, 2);
            }

            int cy = BAR_H + 2;

            if (WechatBot::isPaired()) {
                constexpr int LINES = 5;
                constexpr int LH = 16;
                int contentH = LINES * LH;
                int areaH = SCREEN_H - BAR_H;
                cy = BAR_H + (areaH - contentH) / 2;
                int cx = SCREEN_W / 2;
                canvas.setTextDatum(TC_DATUM);

                canvas.setTextColor(rgb565(60, 200, 80));
                canvas.fillCircle(cx - 30, cy + 5, 4, rgb565(60, 200, 80));
                canvas.drawString("Online", cx + 4, cy);
                cy += LH;

                canvas.setTextColor(Color::CLOCK_TEXT);
                char cronBuf[24];
                snprintf(cronBuf, sizeof(cronBuf), "Cron  %d jobs", CronService::getJobCount());
                canvas.drawString(cronBuf, cx, cy);
                cy += LH;

                unsigned long hbRemain = Heartbeat::getRemainingMs();
                int hbMin = hbRemain / 60000;
                int hbSec = (hbRemain % 60000) / 1000;
                char hbBuf[24];
                snprintf(hbBuf, sizeof(hbBuf), "Heartbeat  %02d:%02d", hbMin, hbSec);
                canvas.drawString(hbBuf, cx, cy);
                cy += LH;

                canvas.setTextColor(Color::STATUS_DIM);
                canvas.drawString(WechatBot::getApiHost(), cx, cy);
                cy += LH;
                canvas.drawString(WiFi.localIP().toString().c_str(), cx, cy);

                canvas.setTextDatum(TL_DATUM);

            } else if (qrContent && qrContent[0]) {
                constexpr int QR_SZ = SCREEN_H - BAR_H - 6;
                constexpr int QR_X = 4;
                int qrY = BAR_H + 3;
                canvas.qrcode(qrContent, QR_X, qrY, QR_SZ, 6);

                int tx = QR_X + QR_SZ + 8;
                int qrMidY = qrY + QR_SZ / 2;

                canvas.setTextColor(Color::CLOCK_TEXT);
                canvas.drawString("Scan with", tx, qrMidY - 20);
                canvas.drawString("WeChat", tx, qrMidY - 6);

                canvas.setTextColor(Color::STATUS_DIM);
                canvas.drawString("Waiting...", tx, qrMidY + 14);
            } else {
                canvas.setTextColor(rgb565(200, 80, 60));
                canvas.drawString("Pairing failed", 70, cy + 10);

                canvas.setTextColor(Color::CLOCK_TEXT);
                canvas.drawString("Serial:", 30, cy + 34);
                canvas.setTextColor(rgb565(120, 180, 220));
                canvas.drawString("set_wechat <tok> <host>", 30, cy + 50);
            }

            canvas.pushSprite(0, 0);
            break;
        }
    }

    processSerialCommands();
    delay(16);
}

static void onAgentResponse(const char* text) {
    agentResponseText = strdup(text);
    agentResponseReady = true;
}

static void onAgentToken(const char* token) {
    if (!s_tokenQueue) return;
    char* copy = strdup(token);
    if (copy) {
        s_hasStreamedTokens = true;
        if (xQueueSend(s_tokenQueue, &copy, 0) != pdTRUE) {
            free(copy);
        }
    }
}

static void writeWavHeader(File& f, uint32_t pcmBytes, uint32_t sampleRate) {
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t riffSize = 36 + pcmBytes;

    f.seek(0);
    f.write((const uint8_t*)"RIFF", 4);
    f.write((const uint8_t*)&riffSize, 4);
    f.write((const uint8_t*)"WAVEfmt ", 8);
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;
    f.write((const uint8_t*)&fmtSize, 4);
    f.write((const uint8_t*)&audioFormat, 2);
    f.write((const uint8_t*)&channels, 2);
    f.write((const uint8_t*)&sampleRate, 4);
    f.write((const uint8_t*)&byteRate, 4);
    f.write((const uint8_t*)&blockAlign, 2);
    f.write((const uint8_t*)&bitsPerSample, 2);
    f.write((const uint8_t*)"data", 4);
    f.write((const uint8_t*)&pcmBytes, 4);
}

static bool writeVoiceBytes(const void* data, size_t len) {
    if (!s_voiceFile || !data || len == 0) return false;
    size_t written = s_voiceFile.write((const uint8_t*)data, len);
    if (written != len) {
        s_voiceWriteFailed = true;
        Serial.printf("[VOICE] File write short: %u/%u bytes, spiffs used=%u total=%u\n",
                      (unsigned)written, (unsigned)len,
                      (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    }
    s_voicePcmBytes += written;
    return written == len;
}

void startVoiceRecording() {
    if (WechatBot::isRunning()) {
        WechatBot::requestPause();
        s_wechatPausedForVoice = true;
        Serial.printf("[VOICE] WeChat paused for audio capture, heap=%d\n", ESP.getFreeHeap());
    }

    M5Cardputer.Speaker.stop();

    if (!sttChunkBuf) {
        sttChunkBuf = (int16_t*)malloc(M5CLAW_AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
    }
    if (!sttChunkBuf) {
        Serial.println("[VOICE] Failed to alloc chunk buffer");
        if (s_wechatPausedForVoice) { WechatBot::resume(); s_wechatPausedForVoice = false; }
        return;
    }

    auto micCfg = M5Cardputer.Mic.config();
    micCfg.sample_rate = M5CLAW_AUDIO_RECORD_SAMPLE_RATE;
    micCfg.magnification = 64;
    micCfg.noise_filter_level = 64;
    micCfg.task_priority = 1;
    M5Cardputer.Mic.config(micCfg);
    M5Cardputer.Mic.begin();

    if (s_voiceFile) s_voiceFile.close();
    SPIFFS.remove(M5CLAW_AUDIO_TEMP_FILE);
    s_voiceFile = SPIFFS.open(M5CLAW_AUDIO_TEMP_FILE, "w");
    if (!s_voiceFile) {
        Serial.println("[VOICE] Failed to open temp wav file");
        M5Cardputer.Mic.end();
        free(sttChunkBuf); sttChunkBuf = nullptr;
        if (s_wechatPausedForVoice) { WechatBot::resume(); s_wechatPausedForVoice = false; }
        return;
    }
    uint8_t blankHeader[44] = {0};
    s_voiceWriteFailed = false;
    if (s_voiceFile.write(blankHeader, sizeof(blankHeader)) != sizeof(blankHeader)) {
        s_voiceWriteFailed = true;
        Serial.printf("[VOICE] Header write failed, spiffs used=%u total=%u\n",
                      (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    }
    s_voicePcmBytes = 0;

    chat.update(canvas);
    canvas.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, rgb565(50, 50, 200));
    canvas.setTextColor(Color::WHITE);
    canvas.setTextSize(1);
    canvas.drawString("Recording...", 82, SCREEN_H - 12);
    canvas.pushSprite(0, 0);

    memset(sttChunkBuf, 0, M5CLAW_AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
    M5Cardputer.Mic.record(sttChunkBuf, M5CLAW_AUDIO_CHUNK_SAMPLES, M5CLAW_AUDIO_RECORD_SAMPLE_RATE);
    voiceRecording = true;
    recordingStartMs = millis();
    Serial.printf("[VOICE] Recording started, heap=%d\n", ESP.getFreeHeap());
}

void streamVoiceData() {
    if (!voiceRecording || !sttChunkBuf || !s_voiceFile) return;

    if (!M5Cardputer.Mic.isRecording()) {
        size_t chunkBytes = M5CLAW_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
        writeVoiceBytes(sttChunkBuf, chunkBytes);
        memset(sttChunkBuf, 0, chunkBytes);
        M5Cardputer.Mic.record(sttChunkBuf, M5CLAW_AUDIO_CHUNK_SAMPLES, M5CLAW_AUDIO_RECORD_SAMPLE_RATE);
    }
}

String stopVoiceRecording() {
    if (!voiceRecording) return "";
    voiceRecording = false;

    M5Cardputer.Mic.end();

    if (sttChunkBuf && s_voiceFile) {
        size_t chunkBytes = M5CLAW_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
        writeVoiceBytes(sttChunkBuf, chunkBytes);
        writeWavHeader(s_voiceFile, s_voicePcmBytes, M5CLAW_AUDIO_RECORD_SAMPLE_RATE);
        s_voiceFile.flush();
        s_voiceFile.close();
    }

    free(sttChunkBuf);
    sttChunkBuf = nullptr;

    if (s_wechatPausedForVoice) {
        WechatBot::resume();
        s_wechatPausedForVoice = false;
        Serial.printf("[VOICE] WeChat resumed, heap=%d\n", ESP.getFreeHeap());
    }

    float duration = (float)(millis() - recordingStartMs) / 1000.0f;
    if (duration < 0.3f || s_voicePcmBytes == 0) {
        SPIFFS.remove(M5CLAW_AUDIO_TEMP_FILE);
        Serial.printf("[VOICE] Discarded short recording %.2fs\n", duration);
        return "";
    }

    size_t savedFileBytes = 0;
    File verify = SPIFFS.open(M5CLAW_AUDIO_TEMP_FILE, "r");
    if (verify) {
        savedFileBytes = verify.size();
        verify.close();
    }

    Serial.printf("[VOICE] Saved %.2fs audio to %s (%u bytes pcm, file=%u, writeFailed=%d)\n",
                  duration, M5CLAW_AUDIO_TEMP_FILE, (unsigned)s_voicePcmBytes,
                  (unsigned)savedFileBytes, s_voiceWriteFailed);

    if (s_voiceWriteFailed || savedFileBytes <= 44 || savedFileBytes != s_voicePcmBytes + 44) {
        Serial.printf("[VOICE] Invalid temp wav, expected=%u actual=%u\n",
                      (unsigned)(s_voicePcmBytes + 44), (unsigned)savedFileBytes);
        SPIFFS.remove(M5CLAW_AUDIO_TEMP_FILE);
        return "";
    }

    return M5CLAW_AUDIO_TEMP_FILE;
}

void enterSetupMode() {
    appMode = AppMode::SETUP;
    setupStep = SetupStep::SSID;
    setupInput = "";
}

static void getDefaultHint(char* buf, int bufSize, const String& value, bool isPassword) {
    if (value.length() == 0) {
        snprintf(buf, bufSize, "(empty)");
    } else if (isPassword) {
        snprintf(buf, bufSize, "[%d chars set]", value.length());
    } else {
        int maxShow = bufSize - 3;
        if ((int)value.length() > maxShow) {
            snprintf(buf, bufSize, "[...%s]", value.c_str() + value.length() - maxShow + 4);
        } else {
            snprintf(buf, bufSize, "[%s]", value.c_str());
        }
    }
}

void updateSetupMode() {
    canvas.fillScreen(Color::BG_DAY);
    canvas.setTextColor(Color::CLOCK_TEXT);
    canvas.setTextSize(1);
    canvas.drawString("=== M5Claw Setup ===", 55, 4);

    char hint[64];
    const char* label = "";
    String currentVal;
    bool isPass = false;

    switch (setupStep) {
        case SetupStep::SSID:        label = "WiFi SSID:";     currentVal = Config::getSSID();         break;
        case SetupStep::PASSWORD:    label = "WiFi Password:";  currentVal = Config::getPassword();     isPass = true; break;
        case SetupStep::PROVIDER:    label = "Provider:";       currentVal = Config::getLlmProvider();  break;
        case SetupStep::LLM_KEY:     label = "LLM API Key:";   currentVal = Config::getLlmApiKey();    isPass = true; break;
        case SetupStep::LLM_MODEL:   label = "LLM Model:";     currentVal = Config::getLlmModel();     break;
        case SetupStep::CITY:        label = "City:";           currentVal = Config::getCity();          break;
        case SetupStep::CONNECTING:
            canvas.drawString("Connecting to WiFi...", 50, 55);
            canvas.pushSprite(0, 0);
            return;
    }

    canvas.drawString(label, 10, 25);
    getDefaultHint(hint, sizeof(hint), currentVal, isPass);
    canvas.setTextColor(Color::STATUS_DIM);
    int labelW = canvas.textWidth(label);
    canvas.drawString(hint, 10 + labelW + 4, 25);

    if (setupStep == SetupStep::PROVIDER) {
        canvas.setTextColor(Color::STATUS_DIM);
        canvas.drawString("Options: mimo / deepseek / openai / custom", 10, 55);
    }

    canvas.setTextColor(Color::WHITE);
    if (isPass && setupInput.length() > 0) {
        char masked[64];
        int len = setupInput.length();
        if (len > 62) len = 62;
        memset(masked, '*', len);
        masked[len] = '_';
        masked[len + 1] = '\0';
        canvas.drawString(masked, 10, 45);
    } else {
        String display = setupInput + "_";
        if (display.length() > 35) {
            display = "..." + display.substring(display.length() - 32);
        }
        canvas.drawString(display.c_str(), 10, 45);
    }

    canvas.setTextColor(Color::STATUS_DIM);
    bool wifiOnly = hasPreconfiguredOnlineSettings();
    if (wifiOnly) {
        canvas.drawString("[Enter] confirm  [Tab] skip", 10, 70);
        canvas.drawString("AI config already set", 10, 84);
    } else {
        canvas.drawString("[Enter] confirm  [Tab] skip/cancel", 10, 70);
    }

    int stepNum, totalSteps;
    if (wifiOnly) {
        stepNum = (setupStep == SetupStep::SSID) ? 1 : 2;
        totalSteps = 2;
    } else {
        stepNum = (int)setupStep + 1;
        totalSteps = 6;
    }
    char progress[16];
    snprintf(progress, sizeof(progress), "Step %d/%d", stepNum, totalSteps);
    canvas.drawString(progress, SCREEN_W - 60, 4);

    canvas.pushSprite(0, 0);
}

void handleSetupKey(char key, bool enter, bool backspace, bool tab) {
    if (tab) {
        if (WiFi.status() != WL_CONNECTED) offlineMode = true;
        Config::save();
        enterCompanionMode();
        return;
    }

    if (backspace && setupInput.length() > 0) {
        setupInput.remove(setupInput.length() - 1);
        return;
    }

    if (key && !enter) {
        setupInput += key;
        return;
    }

    if (!enter) return;

    switch (setupStep) {
        case SetupStep::SSID:
            if (setupInput.length() > 0) Config::setSSID(setupInput);
            if (Config::getSSID().length() == 0) break;
            setupInput = ""; setupStep = SetupStep::PASSWORD; break;
        case SetupStep::PASSWORD:
            if (setupInput.length() > 0) Config::setPassword(setupInput);
            setupInput = "";
            setupStep = SetupStep::PROVIDER;
            break;
        case SetupStep::PROVIDER: {
            String provChoice = setupInput;
            if (provChoice.length() == 0) provChoice = Config::getLlmProvider();
            if (provChoice.length() == 0) provChoice = M5CLAW_DEFAULT_PROVIDER;
            const LlmProviderInfo* info = llm_provider_by_id(provChoice.c_str());
            if (info) {
                Config::setLlmProvider(provChoice);
                if (info->default_model[0]) Config::setLlmModel(String(info->default_model));
                setupInput = "";
                // Skip API key step if built-in key exists
                if (Config::getLlmApiKey().length() > 0 || getBuiltInKeyForProvider(provChoice.c_str())) {
                    if (getBuiltInKeyForProvider(provChoice.c_str())) {
                        Config::setTransientLlmApiKey(String(getBuiltInKeyForProvider(provChoice.c_str())));
                    }
                    if (hasPreconfiguredOnlineSettings()) {
                        if (Config::getCity().length() == 0) Config::setCity("Beijing");
                        Config::save();
                        setupStep = SetupStep::CONNECTING;
                        connectWiFi();
                    } else {
                        setupStep = SetupStep::LLM_MODEL;
                    }
                } else {
                    setupStep = SetupStep::LLM_KEY;
                }
            }
            // If unknown provider, stay on PROVIDER step
            break;
        }
        case SetupStep::LLM_KEY:
            if (setupInput.length() > 0) Config::setLlmApiKey(setupInput);
            setupInput = ""; setupStep = SetupStep::LLM_MODEL; break;
        case SetupStep::LLM_MODEL:
            if (setupInput.length() > 0) Config::setLlmModel(setupInput);
            if (Config::getLlmModel().length() == 0) {
                const LlmProviderInfo* pi = llm_provider_by_id(Config::getLlmProvider().c_str());
                if (pi && pi->default_model[0]) Config::setLlmModel(String(pi->default_model));
            }
            setupInput = ""; setupStep = SetupStep::CITY; break;
        case SetupStep::CITY:
            if (setupInput.length() > 0) Config::setCity(setupInput);
            if (Config::getCity().length() == 0) Config::setCity("Beijing");
            Config::save();
            setupInput = "";
            setupStep = SetupStep::CONNECTING;
            connectWiFi();
            break;
        default: break;
    }
}

bool tryConnect(const String& ssid, const String& pass) {
    Serial.printf("[WIFI] Trying %s...\n", ssid.c_str());
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
        canvas.fillScreen(Color::BG_DAY);
        canvas.setTextColor(Color::CLOCK_TEXT);
        canvas.setTextSize(1);
        char msg[64];
        static const char* dots[] = {".", "..", "...", "...."};
        snprintf(msg, sizeof(msg), "Connecting to %s%s", ssid.c_str(), dots[attempts % 4]);
        if (strlen(msg) > 38) msg[38] = '\0';
        canvas.drawString(msg, 10, 55);
        canvas.pushSprite(0, 0);
    }

    bool ok = (WiFi.status() == WL_CONNECTED);
    Serial.printf("[WIFI] %s: %s\n", ssid.c_str(), ok ? "OK" : "FAILED");
    return ok;
}

void connectWiFi() {
    offlineMode = false;

    while (true) {
        bool connected = tryConnect(Config::getSSID(), Config::getPassword());

        if (!connected && Config::getSSID2().length() > 0) {
            connected = tryConnect(Config::getSSID2(), Config::getPassword2());
        }

        if (connected) {
            initOnlineServices();
            enterCompanionMode();
            return;
        }

        canvas.fillScreen(Color::BG_DAY);
        canvas.setTextColor(rgb565(220, 80, 80));
        canvas.setTextSize(1);
        canvas.drawString("WiFi failed!", 80, 20);
        canvas.setTextColor(Color::CLOCK_TEXT);
        canvas.drawString("[Enter]  Retry", 60, 48);
        canvas.drawString("[Fn+R]   Change WiFi", 60, 63);
        canvas.drawString("[Tab]    Offline mode", 60, 78);
        canvas.pushSprite(0, 0);

        while (true) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter) break;
                if (ks.fn && ks.word.size() > 0 && ks.word[0] == 'r') {
                    Config::setSSID("");
                    Config::setPassword("");
                    Config::save();
                    enterSetupMode();
                    return;
                }
                if (ks.tab) {
                    offlineMode = true;
                    enterCompanionMode();
                    return;
                }
            }
            delay(50);
        }
    }
}

void initOnlineServices() {
    configTime(M5CLAW_GMT_OFFSET_SEC, M5CLAW_DAYLIGHT_OFFSET_SEC, M5CLAW_NTP_SERVER);

    canvas.fillScreen(Color::BG_DAY);
    canvas.setTextColor(Color::CHAT_AI);
    canvas.setTextSize(1);
    canvas.drawString("WiFi connected!", 70, 40);
    canvas.drawString(WiFi.localIP().toString().c_str(), 80, 55);
    canvas.drawString("Initializing services...", 55, 75);
    canvas.pushSprite(0, 0);

    llm_client_init(Config::getLlmApiKey().c_str(),
                    Config::getLlmModel().c_str(),
                    Config::getLlmProvider().c_str(),
                    nullptr, nullptr);

    tts_client_init(Config::getTtsProvider().c_str(),
                    Config::getTtsApiKey().c_str(),
                    Config::getTtsModel().c_str(),
                    Config::getTtsVoice().c_str());

    stt_client_init(Config::getSttProvider().c_str(),
                    Config::getSttApiKey().c_str(),
                    Config::getSttModel().c_str());

    weatherClient.begin(Config::getCity());
    Agent::start();

    // Start WeChat bot (lazy: only if credentials configured)
    WechatBot::start();

    // Start background services
    CronService::start();
    Heartbeat::start();

    delay(500);
}

void enterCompanionMode() {
    appMode = AppMode::COMPANION;
    companion.begin(canvas);
}

void enterChatMode() {
    appMode = AppMode::CHAT;
    chat.begin(canvas);
}
