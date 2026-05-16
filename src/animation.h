#pragma once
#include <M5Cardputer.h>
#include "utils.h"

struct RainDrop {
    int16_t x, y;
    int16_t speed;
    uint8_t length;
    bool active;
};

class AnimationState {
public:
    void begin();
    void update(float dt, float accelX, float accelY, float accelZ);
    void drawRain(M5Canvas& canvas);
    bool isRaining() const { return rainActive; }

private:
    float stateTimer = 0;
    bool  shakeTriggered = false;

    // Shake detection
    static constexpr int   SHAKE_WINDOW = 32;
    float shakeHistory[SHAKE_WINDOW] = {};
    int   shakeIdx = 0;
    float shakeTimer = 0;
    static constexpr float SHAKE_VAR_THRESHOLD = 0.25f;
    static constexpr float SHAKE_HOLD_TIME = 0.4f;
    static constexpr float RAIN_MIN_DURATION = 5.0f;
    static constexpr float RAIN_FADE_TIME = 3.0f;

    // Rain
    static constexpr int MAX_RAINDROPS = 30;
    RainDrop drops[MAX_RAINDROPS];
    float rainIntensity = 0;
    bool rainActive = false;

    void spawnRainDrops();
    void updateRain(float dt);
    float accelMagnitude(float x, float y, float z);
};
