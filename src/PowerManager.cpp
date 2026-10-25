#include "PowerManager.h"
#include "SettingsStorage.h"
#include "MotionHaptic.h"
#include <Wire.h>
#include <esp32-hal-cpu.h>
#include <LilyGoLib.h>      // global `instance` (AXP2101 pmu + backlight)
#include "DisplayEngine.h"
#include "ui/Icons.h"

static volatile int sKeyEvent = 0;
static volatile uint8_t sTargetBrightness = 220;
static uint8_t sAppliedHwBrightness = 0xFF;
static SemaphoreHandle_t sI2cMutex = nullptr;

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
            DisplayEngine::showNotification("Charging", "USB power connected", 2500, ICO_BOLT);
            break;
        case PMU_EVENT_USBC_REMOVE:
            DisplayEngine::showNotification("On battery", "USB power disconnected", 2500, ICO_BATTERY_FULL);
            break;
        case PMU_EVENT_CHARGE_FINISH:
            DisplayEngine::showNotification("Fully charged", "Battery at 100 %", 4000, ICO_BATTERY_FULL);
            break;
        case PMU_EVENT_LOW_VOLTAGE_LEVEL1:
            DisplayEngine::showNotification("Battery low", "Connect a charger soon", 6000, ICO_BATTERY_ALERT);
            break;
        case PMU_EVENT_LOW_VOLTAGE_LEVEL2:
            DisplayEngine::showNotification("Battery critical", "The watch will shut down soon", 8000, ICO_BATTERY_ALERT);
            break;
        default: break;
    }
}

int PowerManager::takeKeyEvent() {
    int e = sKeyEvent;
    sKeyEvent = 0;
    return e;
}

XPowersAXP2101 PowerManager::pmu;
bool PowerManager::pmuReady = false;
uint8_t PowerManager::currentBrightness = 220;
PowerState PowerManager::currentState = POWER_ACTIVE;
unsigned long PowerManager::lastActivityTime = 0;
unsigned long PowerManager::lastTelemetryUpdate = 0;

float PowerManager::cachedVolt = 4.0f;
int PowerManager::cachedPercent = 85;
bool PowerManager::cachedCharging = false;
bool PowerManager::cachedVbus = false;
float PowerManager::cachedCurrent = 0.0f;
float PowerManager::cachedTemp = 28.0f;

bool PowerManager::init() {
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
    Serial.printf("[PMU] Battery %d%%  %.2fV  vbus=%d charging=%d\n",
                  cachedPercent, cachedVolt, cachedVbus, cachedCharging);
    return true;
}

void PowerManager::setBrightness(uint8_t level) {
    SettingsStorage::get().brightness = level;
    currentBrightness = level;
    sTargetBrightness = level;
    lastActivityTime = millis();
    if (currentState != POWER_ACTIVE) {
        currentState = POWER_ACTIVE;
    }
}

uint8_t PowerManager::getBrightness() {
    return currentBrightness;
}

void PowerManager::setPowerState(PowerState state) {
    if (currentState == state) return;
    currentState = state;

    // NOTE: Never call setCpuFrequencyMhz(80/40) while WiFi/BLE/QSPI DMA are active
    // (causes APB clock divider glitches and watchdog reboots on ESP32-S3), and never
    // call instance.setBrightness() directly from Core 0 (causes QSPI bus collision
    // with Core 1 pushColorsAsync DMA). Target brightness is applied on Core 1.
    switch (state) {
        case POWER_ACTIVE:
            currentBrightness = SettingsStorage::get().brightness;
            sTargetBrightness = currentBrightness;
            break;
        case POWER_DIM:
            currentBrightness = (uint8_t)max(24, (int)SettingsStorage::get().brightness / 3);
            sTargetBrightness = currentBrightness;
            break;
        case POWER_AOD:
            currentBrightness = 18; // Ultra-low ambient glow
            sTargetBrightness = currentBrightness;
            break;
        case POWER_SLEEP:
            currentBrightness = 0;
            sTargetBrightness = 0;
            break;
    }
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
    lastActivityTime = millis();
    if (currentState != POWER_ACTIVE || currentBrightness != SettingsStorage::get().brightness) {
        currentState = POWER_ACTIVE;
        currentBrightness = SettingsStorage::get().brightness;
        sTargetBrightness = currentBrightness;
    }
}

void PowerManager::updateTelemetry() {
    if (!pmuReady) return;
    cachedVolt = instance.pmu.getBattVoltage() / 1000.0f;
    cachedCharging = instance.pmu.isCharging();
    cachedVbus = instance.pmu.isVbusIn();

    // High accuracy voltage to SoC curve for 1S LiPo (3.4V empty to 4.2V full)
    int pct = (int)((cachedVolt - 3.40f) / (4.20f - 3.40f) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (cachedVbus && !cachedCharging && cachedVolt >= 4.10f) pct = 100;
    cachedPercent = pct;

    cachedTemp = instance.pmu.getTemperature();
}

float PowerManager::getBatteryVoltage() { return cachedVolt; }
int PowerManager::getBatteryPercent() { return cachedPercent; }
bool PowerManager::isCharging() { return cachedCharging; }
bool PowerManager::isVbusConnected() { return cachedVbus; }
float PowerManager::getChargeCurrentMa() { return cachedCharging ? 300.0f : 0.0f; }
float PowerManager::getTempCelsius() { return cachedTemp; }

void PowerManager::loop(unsigned long currentMillis) {
    if (lockI2C(30)) {
        instance.loop();                       // PMU / RTC / sensor interrupts -> events
        if (currentMillis - lastTelemetryUpdate > 1000) {
            lastTelemetryUpdate = currentMillis;
            updateTelemetry();
        }
        unlockI2C();
    }

    // Auto Power State transitions (with Smart Sleep power-saving override)
    bool userSleeping = MotionHaptic::isSleeping();
    uint32_t sleepThresholdMs = userSleeping ? 5000UL : (SettingsStorage::get().autoSleepSec * 1000UL);
    if (sleepThresholdMs == 0 && !userSleeping) {
        // Auto-dim / auto-sleep disabled: always keep active
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
        // Dim after sleepThresholdMs, then enter AOD (if enabled) or deep sleep after 2x threshold
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

