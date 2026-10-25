#pragma once
#include "Config.h"
#include <XPowersLib.h>

class PowerManager {
public:
    static bool init();
    static void updateTelemetry();
    static void setBrightness(uint8_t level);
    static uint8_t getBrightness();
    static void setPowerState(PowerState state);
    static PowerState getPowerState();

    static float getBatteryVoltage();
    static int getBatteryPercent();
    static bool isCharging();
    static bool isVbusConnected();
    static float getChargeCurrentMa();
    static float getTempCelsius();

    static void handleActivity();
    static void loop(unsigned long currentMillis);
    static void applyBrightnessOnUiThread();

    // Shared recursive mutex protecting the I2C bus (Wire) across Core 0 and Core 1
    static bool lockI2C(uint32_t timeoutMs = 50);
    static void unlockI2C();

    // side (power) key: 0 = none, 1 = click, 2 = long press. Consumed by the UI task.
    static int takeKeyEvent();

private:
    static XPowersAXP2101 pmu;
    static bool pmuReady;
    static uint8_t currentBrightness;
    static PowerState currentState;
    static unsigned long lastActivityTime;
    static unsigned long lastTelemetryUpdate;

    static float cachedVolt;
    static int cachedPercent;
    static bool cachedCharging;
    static bool cachedVbus;
    static float cachedCurrent;
    static float cachedTemp;
};

