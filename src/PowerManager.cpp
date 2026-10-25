#include "PowerManager.h"
#include "SettingsStorage.h"
#include "MotionHaptic.h"
#include "GpsManager.h"
#include <Wire.h>
#include <LilyGoLib.h>      // global `instance` (AXP2101 pmu + backlight)
#include "DisplayEngine.h"
#include "ui/Icons.h"

static volatile int sKeyEvent = 0;
static volatile uint8_t sTargetBrightness = 220;
static uint8_t sAppliedHwBrightness = 0xFF;
static SemaphoreHandle_t sI2cMutex = nullptr;

// torch override (not persisted)
static volatile bool sTorchOn = false;

// requestSleep() bookkeeping
static const uint32_t SLEEP_GRACE_MS = 800;
static volatile bool sSleepRequested = false;          // user asked for SLEEP; cleared by real activity
static volatile unsigned long sSleepRequestedAt = 0;

bool PowerManager::lockI2C(uint32_t timeoutMs) {
    if (!sI2cMutex) return true;
    return xSemaphoreTakeRecursive(sI2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void PowerManager::unlockI2C() {
    if (sI2cMutex) xSemaphoreGiveRecursive(sI2cMutex);
}

static uint8_t toHardwareBrightness(uint8_t uiLevel) {
    if (uiLevel == 0) return 0;
    // Scale UI 1..255 -> 8..215 to prevent AXP2101 ALDO (300mA limit) overcurrent/brownout
    // on the 1.75" CO5300 AMOLED at full white/high duty while preserving full visual range.
    int hw = 8 + ((int)uiLevel * 207) / 255;
    if (hw < 8) hw = 8;
    if (hw > 215) hw = 215;
    return (uint8_t)hw;
}

// PMU events from LilyGoLib (dispatched inside instance.loop() on the bg task)
static void onPmuEvent(DeviceEvent_t, void* params, void*) {
    switch (instance.getPMUEventType(params)) {
        case PMU_EVENT_KEY_CLICKED:       sKeyEvent = 1; PowerManager::handleActivity(); break;
        case PMU_EVENT_KEY_LONG_PRESSED:  sKeyEvent = 2; PowerManager::handleActivity(); break;
        case PMU_EVENT_USBC_INSERT:
            PowerManager::handleActivity();
            DisplayEngine::showNotification("Laden", "USB-Stromversorgung verbunden", 2500, ICO_BOLT);
            break;
        case PMU_EVENT_USBC_REMOVE:
            DisplayEngine::showNotification("Akkubetrieb", "USB-Stromversorgung getrennt", 2500, ICO_BATTERY_FULL);
            break;
        case PMU_EVENT_CHARGE_FINISH:
            DisplayEngine::showNotification("Vollständig geladen", "Akku bei 100 %", 4000, ICO_BATTERY_FULL);
            break;
        case PMU_EVENT_LOW_VOLTAGE_LEVEL1:
            DisplayEngine::showNotification("Akku schwach", "Bitte bald laden", 6000, ICO_BATTERY_ALERT);
            break;
        case PMU_EVENT_LOW_VOLTAGE_LEVEL2:
            DisplayEngine::showNotification("Akku kritisch", "Die Uhr schaltet sich bald ab", 8000, ICO_BATTERY_ALERT);
            break;
        default: break;
    }
}

int PowerManager::takeKeyEvent() {
    // Atomic read-and-clear: the bg task writes, the UI task consumes. A plain
    // read-then-store could lose an event written in between.
    return __atomic_exchange_n(&sKeyEvent, 0, __ATOMIC_SEQ_CST);
}

bool PowerManager::pmuReady = false;
uint8_t PowerManager::currentBrightness = 220;
PowerState PowerManager::currentState = POWER_ACTIVE;
unsigned long PowerManager::lastActivityTime = 0;
unsigned long PowerManager::lastTelemetryUpdate = 0;

// Telemetry starts as "unknown" (0 %, 0 V) until the first PMU read in init().
float PowerManager::cachedVolt = 0.0f;
int PowerManager::cachedPercent = 0;
bool PowerManager::cachedBattery = false;
bool PowerManager::cachedCharging = false;
bool PowerManager::cachedVbus = false;
float PowerManager::cachedTemp = 0.0f;

bool PowerManager::init() {
    pinMode(0, INPUT_PULLUP); // Left Action / Boot button (GPIO 0)
    if (!sI2cMutex) {
        sI2cMutex = xSemaphoreCreateRecursiveMutex();
    }
    // On the T-Watch Ultra the AXP2101 PMU, power rails, charger and the display
    // backlight are all brought up by LilyGoLib's instance.begin() (called in
    // setup() before this). Configure safe VBUS/system limits and disable unused rails.
    Serial.println("[PMU] Using LilyGoLib AXP2101 (brought up by instance.begin)...");
    pmuReady = true;

    instance.pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    instance.pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    instance.pmu.setSysPowerDownVoltage(2600);
    // Keep DLDO1 (NFC rail) powered with NFC_CS HIGH so ST25R3916 does not clamp
    // the shared SPI bus (MISO=33/MOSI=34/SCK=35) used by microSD and SX1262 LoRa.
    pinMode(NFC_CS, OUTPUT);
    digitalWrite(NFC_CS, HIGH);

    instance.onEvent(onPmuEvent, POWER_EVENT, nullptr);
    currentBrightness = SettingsStorage::get().brightness;
    sTargetBrightness = currentBrightness;
    sAppliedHwBrightness = toHardwareBrightness(currentBrightness);
    instance.setBrightness(sAppliedHwBrightness);
    updateTelemetry();
    lastActivityTime = millis();
    Serial.printf("[PMU] Battery %d%% (%s)  %.2fV  vbus=%d charging=%d\n",
                  cachedPercent, cachedBattery ? "fuel gauge" : "no battery / voltage curve",
                  cachedVolt, cachedVbus, cachedCharging);
    return true;
}

// Derive the target brightness from power state + torch override.
void PowerManager::recomputeTarget() {
    if (sTorchOn) {
        sTargetBrightness = 255;
        return;
    }
    const uint8_t user = SettingsStorage::get().brightness;
    switch (currentState) {
        case POWER_ACTIVE: currentBrightness = user; break;
        case POWER_DIM:    currentBrightness = (uint8_t)max(24, (int)user / 3); break;
        case POWER_AOD:    currentBrightness = 18; break;   // ultra-low ambient glow
        case POWER_SLEEP:  currentBrightness = 0; break;
    }
    sTargetBrightness = currentBrightness;
}

void PowerManager::setBrightness(uint8_t level) {
    SettingsStorage::get().brightness = level;
    lastActivityTime = millis();
    sSleepRequested = false;
    currentState = POWER_ACTIVE;
    recomputeTarget();
}

uint8_t PowerManager::getBrightness() {
    return sTorchOn ? 255 : currentBrightness;
}

void PowerManager::setTorchOverride(bool on) {
    sTorchOn = on;
    if (on) {
        lastActivityTime = millis();
        sSleepRequested = false;
        currentState = POWER_ACTIVE;
    }
    recomputeTarget();
}

bool PowerManager::isTorchOverride() { return sTorchOn; }

void PowerManager::setPowerState(PowerState state) {
    if (currentState == state) return;
    const PowerState prev = currentState;
    currentState = state;
    if (state == POWER_ACTIVE) sSleepRequested = false;

    // NOTE: CPU frequency scaling is intentionally NOT used. Never call
    // setCpuFrequencyMhz(80/40) while WiFi/BLE/QSPI DMA are active (causes APB
    // clock divider glitches and watchdog reboots on ESP32-S3). Light sleep is
    // not used either: the SX1262 DIO1 / BLE / WiFi interrupts would be lost.
    // Never call instance.setBrightness() directly from Core 0 (QSPI bus
    // collision with the Core 1 pushColorsAsync DMA): the target brightness is
    // applied on Core 1 in applyBrightnessOnUiThread().
    recomputeTarget();

    if (state == POWER_SLEEP && prev != POWER_SLEEP) {
        // Real power saving that is safe without light sleep: stop continuous
        // GNSS tracking (~25 mA). The UI task throttles itself to 250 ms via
        // isLowPowerState(). Pages that need live GPS re-enable it in onShow().
        GpsManager::setLive(false);
    }
}

void PowerManager::requestSleep() {
    sSleepRequestedAt = millis();
    sSleepRequested = true;
    lastActivityTime = sSleepRequestedAt;
    setPowerState(POWER_SLEEP);
}

bool PowerManager::isLowPowerState() {
    return currentState == POWER_AOD || currentState == POWER_SLEEP;
}

void PowerManager::applyBrightnessOnUiThread() {
    uint8_t targetHw = toHardwareBrightness(sTargetBrightness);
    if (targetHw != sAppliedHwBrightness) {
        sAppliedHwBrightness = targetHw;
        instance.setBrightness(targetHw);
    }
}

PowerState PowerManager::getPowerState() {
    return currentState;
}

void PowerManager::handleActivity() {
    const unsigned long now = millis();
    if (sSleepRequested && (now - sSleepRequestedAt) < SLEEP_GRACE_MS) {
        // grace window after requestSleep(): the triggering touch must not wake us
        return;
    }
    sSleepRequested = false;
    lastActivityTime = now;
    if (currentState != POWER_ACTIVE ||
        (!sTorchOn && currentBrightness != SettingsStorage::get().brightness)) {
        currentState = POWER_ACTIVE;
        recomputeTarget();
    }
}

void PowerManager::updateTelemetry() {
    if (!pmuReady) return;
    cachedVolt = instance.pmu.getBattVoltage() / 1000.0f;
    cachedCharging = instance.pmu.isCharging();
    cachedVbus = instance.pmu.isVbusIn();

    // Primary source: AXP2101 fuel gauge (returns -1 without a battery).
    int pct = instance.pmu.getBatteryPercent();
    cachedBattery = (pct >= 0);
    if (pct < 0 || pct > 100) {
        // Fallback: rough linear 1S LiPo voltage interpolation (3.4 V empty .. 4.2 V full).
        // This is an approximation only and is wrong under load / while charging.
        pct = (int)((cachedVolt - 3.40f) / (4.20f - 3.40f) * 100.0f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    cachedPercent = pct;

    cachedTemp = instance.pmu.getTemperature();
}

float PowerManager::getBatteryVoltage() { return cachedVolt; }
int PowerManager::getBatteryPercent() { return cachedPercent; }
bool PowerManager::isBatteryConnected() { return cachedBattery; }
bool PowerManager::isCharging() { return cachedCharging; }
bool PowerManager::isVbusConnected() { return cachedVbus; }
// The AXP2101 has no charge-current ADC; only the configured charge-current
// limit is readable. Callers show the charge state (isCharging) instead.
float PowerManager::getChargeCurrentMa() { return 0.0f; }
float PowerManager::getTempCelsius() { return cachedTemp; }

void PowerManager::loop(unsigned long currentMillis) {
    // Software debouncing & gesture detection for GPIO 0 (Action/Boot button on left side)
    static int sLastBtnState = HIGH;
    static uint32_t sBtnDownMs = 0;
    static uint32_t sBtnUpMs = 0;
    static uint8_t sClickCount = 0;
    static bool sLongReported = false;

    int raw = digitalRead(0);
    if (raw == LOW && sLastBtnState == HIGH) {
        // Pressed
        sBtnDownMs = currentMillis;
        sLongReported = false;
        PowerManager::handleActivity();
    } else if (raw == LOW && sLastBtnState == LOW) {
        // Holding down
        if (!sLongReported && (currentMillis - sBtnDownMs >= 1000)) {
            sLongReported = true;
            sKeyEvent = 5; // Long press Action button
            sClickCount = 0;
            PowerManager::handleActivity();
        }
    } else if (raw == HIGH && sLastBtnState == LOW) {
        // Released
        if (!sLongReported) {
            uint32_t dur = currentMillis - sBtnDownMs;
            if (dur >= 30 && dur < 900) {
                sClickCount++;
                sBtnUpMs = currentMillis;
            }
        }
    }
    sLastBtnState = raw;

    if (sClickCount > 0 && (currentMillis - sBtnUpMs >= 240)) {
        if (sClickCount == 1) {
            sKeyEvent = 3; // Action button single click
        } else if (sClickCount >= 2) {
            sKeyEvent = 4; // Action button double click
        }
        sClickCount = 0;
    }

    if (lockI2C(30)) {
        instance.loop();                       // PMU / RTC / sensor interrupts -> events
        if (currentMillis - lastTelemetryUpdate > 1000) {
            lastTelemetryUpdate = currentMillis;
            updateTelemetry();
        }
        unlockI2C();
    }

    // A user-requested sleep stays in effect until the next real activity,
    // regardless of the auto-sleep setting or USB power.
    if (sSleepRequested) {
        if (currentState != POWER_SLEEP) setPowerState(POWER_SLEEP);
        return;
    }

    // Auto Power State transitions (with Smart Sleep power-saving override)
    bool userSleeping = MotionHaptic::isSleeping();
    uint32_t sleepThresholdMs = userSleeping ? 5000UL : (SettingsStorage::get().autoSleepSec * 1000UL);
    if (sleepThresholdMs == 0 && !userSleeping) {
        // Auto-dim / auto-sleep disabled ("Nie"): keep active
        if (currentState != POWER_ACTIVE) {
            setPowerState(POWER_ACTIVE);
        }
        return;
    }

    unsigned long idleTime = currentMillis - lastActivityTime;

    if (userSleeping) {
        // While user is sleeping in bed: enter full display-off sleep after 6s (even if AOD is normally on!)
        if (idleTime > 6000UL && currentState != POWER_SLEEP) {
            setPowerState(POWER_SLEEP);
        } else if (idleTime > 3000UL && currentState == POWER_ACTIVE) {
            setPowerState(POWER_DIM);
        }
        return;
    }

    if (cachedVbus) {
        // While charging on USB:
        // Dim first after sleepThresholdMs, then enter AOD if enabled or stay dimmed
        if (idleTime > (sleepThresholdMs * 2)) {
            if (SettingsStorage::get().aodEnabled && currentState != POWER_AOD) {
                setPowerState(POWER_AOD);
            } else if (!SettingsStorage::get().aodEnabled && currentState != POWER_DIM) {
                setPowerState(POWER_DIM);
            }
        } else if (idleTime > sleepThresholdMs && currentState == POWER_ACTIVE) {
            setPowerState(POWER_DIM);
        }
    } else {
        // On battery:
        // Dim after sleepThresholdMs, then enter AOD (if enabled) or display-off sleep after 2x threshold
        if (idleTime > (sleepThresholdMs * 2)) {
            if (SettingsStorage::get().aodEnabled && currentState != POWER_AOD) {
                setPowerState(POWER_AOD);
            } else if (!SettingsStorage::get().aodEnabled && currentState != POWER_SLEEP) {
                setPowerState(POWER_SLEEP);
            }
        } else if (idleTime > sleepThresholdMs && currentState == POWER_ACTIVE) {
            setPowerState(POWER_DIM);
        }
    }
}
