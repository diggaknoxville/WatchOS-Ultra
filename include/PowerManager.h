#pragma once
#include "Config.h"
#include <XPowersLib.h>

// AXP2101 PMU telemetry, display brightness and the ACTIVE/DIM/AOD/SLEEP
// state machine. The PMU itself is brought up by LilyGoLib (instance.pmu);
// this class only configures limits, reads telemetry and drives brightness.
//
// Threading: loop() and the PMU event callback run on the background task,
// handleActivity()/requestSleep() may be called from any task, the hardware
// brightness is applied on the UI task only (applyBrightnessOnUiThread) to
// avoid QSPI collisions with the display DMA.
class PowerManager {
public:
    static bool init();
    static void updateTelemetry();

    // User brightness (persisted in SystemSettings::brightness).
    static void setBrightness(uint8_t level);
    static uint8_t getBrightness();

    // Temporary full-brightness override for the torch. Not persisted: a
    // settings save while the torch is on keeps the user's brightness.
    static void setTorchOverride(bool on);
    static bool isTorchOverride();

    static void setPowerState(PowerState state);
    static PowerState getPowerState();

    // Enter POWER_SLEEP now. handleActivity() is ignored for the next 800 ms
    // (grace window) so the touch that triggered the request cannot wake the
    // display again. A requested sleep is honoured even with autoSleepSec == 0
    // until the next real activity (touch, key, wrist wake).
    static void requestSleep();
    static bool isLowPowerState();       // true in POWER_AOD or POWER_SLEEP

    static float getBatteryVoltage();
    static int getBatteryPercent();      // 0..100; AXP2101 fuel gauge, voltage curve as fallback
    static bool isBatteryConnected();    // false if the fuel gauge reports no battery
    static bool isCharging();
    static bool isVbusConnected();
    static float getChargeCurrentMa();   // always 0.0f: the AXP2101 exposes no charge-current ADC
    static float getTempCelsius();

    static void handleActivity();
    static void loop(unsigned long currentMillis);
    static void applyBrightnessOnUiThread();

    // Shared recursive mutex protecting the I2C bus (Wire) across Core 0 and Core 1
    static bool lockI2C(uint32_t timeoutMs = 50);
    static void unlockI2C();

    // Side (power) key / action button, consumed by the UI task (atomic swap):
    // 0 = none, 1 = side key click, 2 = side key long press,
    // 3 = action button click, 4 = action button double click, 5 = action button long press
    static int takeKeyEvent();

private:
    static bool pmuReady;
    static uint8_t currentBrightness;
    static PowerState currentState;
    static unsigned long lastActivityTime;
    static unsigned long lastTelemetryUpdate;

    static float cachedVolt;
    static int cachedPercent;
    static bool cachedBattery;
    static bool cachedCharging;
    static bool cachedVbus;
    static float cachedTemp;

    static void recomputeTarget();
};
