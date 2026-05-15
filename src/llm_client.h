#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "m5claw_config.h"

struct LlmToolCall {
    char id[64];
    char name[32];
    char* input;
    size_t input_len;
};

struct LlmResponse {
    char* text;
    size_t text_len;
    char* raw_content_json;
    LlmToolCall calls[M5CLAW_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;
};

struct LlmProviderInfo {
    const char* id;
    const char* name;
    const char* host;
    const char* chat_path;
    const char* default_model;
    bool has_tts;
    const char* tts_path;
    const char* tts_model;
    const char* tts_voice;
    int tts_sample_rate;
    bool has_web_search;
    int search_max_keyword;
    int search_limit;
};

void llm_response_free(LlmResponse* resp);

void llm_client_init(const char* api_key, const char* model, const char* provider,
                     const char* custom_host = nullptr, const char* custom_path = nullptr);

void llm_client_set_abort_flag(volatile bool* flag);

typedef void (*LlmPreReadFreeFn)();
void llm_client_set_pre_read_free(LlmPreReadFreeFn fn);

typedef void (*LlmStreamCallback)(const char* token);

bool llm_chat_tools(const char* system_prompt,
                    JsonDocument& messages,
                    const char* tools_json,
                    LlmResponse* resp,
                    LlmStreamCallback on_token = nullptr);

bool llm_speak_text(const char* text);

// Provider queries
int llm_provider_count();
const LlmProviderInfo* llm_provider_by_index(int idx);
const LlmProviderInfo* llm_provider_by_id(const char* id);
const char* llm_current_provider();
const char* llm_current_host();
const char* llm_current_model();
