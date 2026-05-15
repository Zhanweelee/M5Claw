#pragma once
#include <Arduino.h>

namespace Config {
    bool load();
    void save();
    void reset();
    bool importBootstrapFile();
    bool applyDefaults();

    const String& getSSID();
    const String& getPassword();
    const String& getSSID2();
    const String& getPassword2();
    const String& getCity();

    // LLM config
    const String& getLlmProvider();
    const String& getLlmApiKey();
    const String& getLlmModel();

    // TTS config
    const String& getTtsProvider();
    const String& getTtsApiKey();
    const String& getTtsVoice();

    // WeChat
    const String& getWechatToken();
    const String& getWechatApiHost();

    void setSSID(const String& ssid);
    void setPassword(const String& password);
    void setSSID2(const String& ssid);
    void setPassword2(const String& password);
    void setCity(const String& city);
    void setLlmProvider(const String& provider);
    void setLlmApiKey(const String& key);
    void setLlmModel(const String& model);
    void setTtsProvider(const String& provider);
    void setTtsApiKey(const String& key);
    void setTtsVoice(const String& voice);
    void setWechatToken(const String& token);
    void setWechatApiHost(const String& host);
    void setTransientLlmApiKey(const String& key);

    bool isValid();
}
