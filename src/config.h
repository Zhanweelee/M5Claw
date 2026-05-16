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
    const String& getTtsModel();
    const String& getTtsVoice();

    // STT config
    const String& getSttProvider();
    const String& getSttApiKey();
    const String& getSttModel();

    // Assistant identity
    const String& getAssistantName();

    // Mute TTS
    bool getMuteTts();

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
    void setTtsModel(const String& model);
    void setTtsVoice(const String& voice);
    void setSttProvider(const String& provider);
    void setSttApiKey(const String& key);
    void setSttModel(const String& model);
    void setAssistantName(const String& name);
    void setMuteTts(bool mute);
    void setWechatToken(const String& token);
    void setWechatApiHost(const String& host);
    void setTransientLlmApiKey(const String& key);

    bool isValid();
}
