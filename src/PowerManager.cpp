#include "PowerManager.h"
#include "SettingsStorage.h"
#include <Wire.h>
#include <esp32-hal-cpu.h>
#include <LilyGoLib.h>      // global `instance` (AXP2101 pmu + backlight)
#include "DisplayEngine.h"
#include "ui/Icons.h"

static volatile int sKeyEvent = 0;

// PMU events from LilyGoLib (dispatched inside instance.loop() on the bg task)
static void onPmuEvent(DeviceEvent_t, void* params, void*) {
    switch (instance.getPMUEventType(params)) {
        case PMU_EVENT_KEY_CLICKED:       sKeyEvent = 1; break;
        case PMU_EVENT_KEY_LONG_PRESSED:  sKeyEvent = 2; break;
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
    // On the T-Watch Ultra the AXP2101 PMU, power rails, charger and the display
    // backlight are all brought up by LilyGoLib's instance.begin() (called in
    // setup() before this). We simply adopt that state here.
    Serial.println("[PMU] Using LilyGoLib AXP2101 (brought up by instance.begin)...");
    pmuReady = true;
    instance.onEvent(onPmuEvent, POWER_EVENT, nullptr);
    setBrightness(SettingsStorage::get().brightness);
    updateTelemetry();
    lastActivityTime = millis();
    Serial.printf("[PMU] Battery %d%%  %.2fV  vbus=%d charging=%d\n",
                  cachedPercent, cachedVolt, cachedVbus, cachedCharging);
    return true;
}

void PowerManager::setBrightness(uint8_t level) {
    currentBrightness = level;
    instance.setBrightness(level);   // AMOLED brightness via LilyGoLib
}

uint8_t PowerManager::getBrightness() {
    return currentBrightness;
}

void PowerManager::setPowerState(PowerState state) {
    if (currentState == state) return;
    currentState = state;

    switch (state) {
        case POWER_ACTIVE:
            setCpuFrequencyMhz(240);
            setBrightness(SettingsStorage::get().brightness);
            break;
        case POWER_DIM:
            setCpuFrequencyMhz(80);
            setBrightness(SettingsStorage::get().brightness / 3);
            break;
        case POWER_AOD:
            setCpuFrequencyMhz(80);
            setBrightness(18); // Ultra-low ambient glow
            break;
        case POWER_SLEEP:
            setBrightness(0);
            setCpuFrequencyMhz(40);
            break;
    }
}

PowerState PowerManager::getPowerState() {
    return currentState;
}

void PowerManager::handleActivity() {
    lastActivityTime = millis();
    if (currentState != POWER_ACTIVE) {
        setPowerState(POWER_ACTIVE);
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
    instance.loop();                       // PMU / RTC / sensor interrupts -> events
    if (currentMillis - lastTelemetryUpdate > 1000) {
        lastTelemetryUpdate = currentMillis;
        updateTelemetry();
    }

    // Auto Power State transitions
    unsigned long idleTime = currentMillis - lastActivityTime;
    uint32_t sleepThresholdMs = SettingsStorage::get().autoSleepSec * 1000UL;

    if (cachedVbus) {
        // While charging on USB, keep in active or AOD mode without deep sleeping
        if (idleTime > sleepThresholdMs && currentState == POWER_ACTIVE) {
            setPowerState(POWER_AOD);
        }
    } else {
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

