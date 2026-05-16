#include "animation.h"
#include <math.h>

static float maxf(float a, float b) { return a > b ? a : b; }
static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float AnimationState::accelMagnitude(float x, float y, float z) {
    return sqrtf(x*x + y*y + z*z);
}

void AnimationState::begin() {
    stateTimer = 0;
    shakeTriggered = false;
    rainIntensity = 0;
    rainActive = false;
    shakeTimer = 0;
    shakeIdx = 0;
    for (int i = 0; i < SHAKE_WINDOW; i++) shakeHistory[i] = 1.0f;
    for (int i = 0; i < MAX_RAINDROPS; i++) drops[i].active = false;
}

void AnimationState::update(float dt, float accelX, float accelY, float accelZ) {
    if (dt <= 0) dt = 0.016f;
    stateTimer += dt;

    float mag = accelMagnitude(accelX, accelY, accelZ);

    // Rolling history for shake variance
    shakeHistory[shakeIdx] = mag;
    shakeIdx = (shakeIdx + 1) % SHAKE_WINDOW;

    float shakeMean = 0;
    for (int i = 0; i < SHAKE_WINDOW; i++) shakeMean += shakeHistory[i];
    shakeMean /= SHAKE_WINDOW;
    float shakeVar = 0;
    for (int i = 0; i < SHAKE_WINDOW; i++) {
        float d = shakeHistory[i] - shakeMean;
        shakeVar += d * d;
    }
    shakeVar /= SHAKE_WINDOW;

    bool shaking = shakeVar > SHAKE_VAR_THRESHOLD;

    if (!rainActive) {
        if (shaking) {
            shakeTimer += dt;
            if (shakeTimer >= SHAKE_HOLD_TIME && !shakeTriggered) {
                shakeTriggered = true;
                rainActive = true;
                rainIntensity = 0;
                stateTimer = 0;
                for (int i = 0; i < MAX_RAINDROPS; i++) drops[i].active = false;
            }
        } else {
            shakeTimer = maxf(0, shakeTimer - dt * 2.0f);
        }
    } else {
        // Ramp up intensity over first 1s
        if (stateTimer < 1.0f) {
            rainIntensity = stateTimer;
        } else {
            rainIntensity = 1.0f;
        }

        // When shaking stops, begin fade after minimum duration
        if (!shaking && stateTimer > RAIN_MIN_DURATION) {
            float elapsed = stateTimer - RAIN_MIN_DURATION;
            rainIntensity = 1.0f - clampf(elapsed / RAIN_FADE_TIME, 0, 1);
            if (rainIntensity <= 0) {
                rainIntensity = 0;
                rainActive = false;
                shakeTriggered = false;
                stateTimer = 0;
            }
        }

        updateRain(dt);
    }
}

// ══════════════════════════════════════════════════════════════
//  Rain
// ══════════════════════════════════════════════════════════════

void AnimationState::spawnRainDrops() {
    int target = (int)(MAX_RAINDROPS * rainIntensity);
    int active = 0;
    for (int i = 0; i < MAX_RAINDROPS; i++) {
        if (drops[i].active) active++;
    }
    int needed = target - active;
    for (int i = 0; i < MAX_RAINDROPS && needed > 0; i++) {
        if (!drops[i].active) {
            drops[i].x = esp_random() % SCREEN_W;
            drops[i].y = -(int)(esp_random() % 40);
            drops[i].speed = 60 + (esp_random() % 80);
            drops[i].length = 4 + (esp_random() % 6);
            drops[i].active = true;
            needed--;
        }
    }
}

void AnimationState::updateRain(float dt) {
    spawnRainDrops();
    for (int i = 0; i < MAX_RAINDROPS; i++) {
        if (!drops[i].active) continue;
        drops[i].y += (int)(drops[i].speed * dt);
        if (drops[i].y > SCREEN_H + 10) {
            drops[i].active = false;
        }
    }
}

void AnimationState::drawRain(M5Canvas& canvas) {
    if (!rainActive || rainIntensity <= 0) return;

    uint16_t rainColor = rgb565(140, 175, 220);

    for (int i = 0; i < MAX_RAINDROPS; i++) {
        if (!drops[i].active) continue;
        int x = drops[i].x;
        int y = drops[i].y;
        int len = drops[i].length;
        for (int dy = 0; dy < len; dy++) {
            int py = y - dy;
            if (py >= 0 && py < SCREEN_H) {
                uint16_t c = (dy == 0) ? rainColor : rgb565(100, 140, 190);
                canvas.drawPixel(x, py, c);
            }
        }
    }
}
