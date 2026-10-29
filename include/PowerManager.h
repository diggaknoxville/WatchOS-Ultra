#pragma once
#include "Config.h"
#include <XPowersLib.h>

// AXP2101 PMU telemetry, display brightness, the ACTIVE/DIM/AOD/SLEEP state
// machine and the CPU power policy. The PMU itself is brought up by LilyGoLib
// (instance.pmu); this class only configures limits, reads telemetry and
// drives brightness, CPU clock and light sleep.
//
// Power policy (applied on the background task):
//   ACTIVE 240 MHz | DIM 160 MHz | AOD / SLEEP 80 MHz
//   floors: hotspot running 160 MHz, USB mass storage or route computation 240 MHz
//   light sleep in AOD / SLEEP when no radio needs the CPU (see canLightSleep()):
//   the UI task is parked first, wake sources are touch, side key, action
//   button, wrist/sensor hub, RTC alarm, LoRa packet and a 1 s timer.
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
    static void getHourlyBattery(uint8_t out[24]); // 24-hour discharge history with NVS persistence
    static bool isBatteryConnected();    // false if the fuel gauge reports no battery
    static bool isCharging();
    static bool isVbusConnected();
    static float getChargeCurrentMa();   // always 0.0f: the AXP2101 exposes no charge-current ADC
    static float getTempCelsius();

    static void handleActivity();
    static void loop(unsigned long currentMillis);
    static void applyBrightnessOnUiThread();
    static uint8_t appliedHwBrightness();   // what the panel currently gets (0 = dark)
    static uint8_t hwBrightnessFor(uint8_t uiLevel);   // UI 0..255 -> safe panel level

    // CPU clock: raise to full speed right now (display wake path, any task)
    static void boostCpu();
    static uint32_t cpuMhz();

    // UI task: idle wait used while the display is off / in AOD. Returns after
    // `ms` or earlier; parks the UI task while the background task light-sleeps.
    static void uiIdle(uint32_t ms);
    static uint32_t lightSleepCount();      // diagnostics
    static uint32_t lightSleepMs();         // total time spent in light sleep

    // Energiesparmodus (SystemSettings::powerSaver): Bluetooth, LoRa and AOD
    // off so light sleep can engage; turning it off restores what it changed.
    static void setPowerSaver(bool on);
    static bool powerSaver();

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
