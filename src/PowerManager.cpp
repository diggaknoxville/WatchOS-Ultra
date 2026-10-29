#include "PowerManager.h"
#include "SettingsStorage.h"
#include "MotionHaptic.h"
#include "GpsManager.h"
#include "BleCompanion.h"
#include "CompanionServer.h"
#include "RadioLoRa.h"
#include "NavEngine.h"
#include "AudioEngine.h"
#include "UsbMscManager.h"
#include "RtcManager.h"
#include <Preferences.h>
#include <Wire.h>
#include <LilyGoLib.h>      // global `instance` (AXP2101 pmu + backlight)
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "DisplayEngine.h"
#include "ui/Icons.h"
#include "ui/Navigator.h"

static volatile int sKeyEvent = 0;
static volatile uint8_t sTargetBrightness = 220;
static volatile uint8_t sAppliedHwBrightness = 0;   // setup() leaves the panel dark
static SemaphoreHandle_t sI2cMutex = nullptr;

// torch override (not persisted)
static volatile bool sTorchOn = false;

// requestSleep() bookkeeping
static const uint32_t SLEEP_GRACE_MS = 800;
static volatile bool sSleepRequested = false;          // user asked for SLEEP; cleared by real activity
static volatile unsigned long sSleepRequestedAt = 0;
static volatile unsigned long sStateSince = 0;         // millis() of the last state change

// CPU clock
static SemaphoreHandle_t sClkMutex = nullptr;

// 24h battery percentage history (persisted in NVS)
static uint8_t sHourlyBattery[24] = {0};

// light sleep: UI task park handshake
static SemaphoreHandle_t sParkReqSem = nullptr;        // bg -> UI: please park
static SemaphoreHandle_t sParkedSem = nullptr;         // UI -> bg: parked, display DMA idle
static SemaphoreHandle_t sResumeSem = nullptr;         // bg -> UI: continue
static uint32_t sLightSleeps = 0;
static uint32_t sLightSleepMs = 0;
static const uint32_t LIGHT_SLEEP_CHUNK_MS = 1000;     // timer wake: clock, steps, GPS windows
static const uint32_t LIGHT_SLEEP_SETTLE_MS = 1500;    // stay awake this long after entering AOD/SLEEP

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

uint8_t PowerManager::hwBrightnessFor(uint8_t uiLevel) { return toHardwareBrightness(uiLevel); }

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
    if (!sI2cMutex) sI2cMutex = xSemaphoreCreateRecursiveMutex();
    if (!sClkMutex) sClkMutex = xSemaphoreCreateMutex();
    if (!sParkReqSem) sParkReqSem = xSemaphoreCreateBinary();
    if (!sParkedSem) sParkedSem = xSemaphoreCreateBinary();
    if (!sResumeSem) sResumeSem = xSemaphoreCreateBinary();
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
    // The panel brightness is NOT touched here: the UI task owns the QSPI bus
    // (boot splash fades in, applyBrightnessOnUiThread() takes over afterwards).
    updateTelemetry();
    lastActivityTime = millis();
    sStateSince = lastActivityTime;

    Preferences p;
    if (p.begin("kronos_pwr", false)) {
        if (p.isKey("hourly_bat")) {
            p.getBytes("hourly_bat", sHourlyBattery, sizeof(sHourlyBattery));
        } else {
            uint8_t cur = (uint8_t)(cachedPercent >= 0 ? cachedPercent : 100);
            for (int i = 0; i < 24; i++) sHourlyBattery[i] = cur;
            p.putBytes("hourly_bat", sHourlyBattery, sizeof(sHourlyBattery));
        }
        p.end();
    }
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
    sStateSince = millis();
    if (state == POWER_ACTIVE) sSleepRequested = false;

    // Never call instance.setBrightness() directly from Core 0 (QSPI bus
    // collision with the Core 1 pushColorsAsync DMA): the target brightness is
    // applied on Core 1 in applyBrightnessOnUiThread(). The CPU clock follows
    // in loop() (cpuPolicy).
    recomputeTarget();

    if (state == POWER_SLEEP && prev != POWER_SLEEP) {
        // Stop continuous GNSS tracking (~25 mA). Pages that need live GPS
        // re-enable it in onShow().
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

uint8_t PowerManager::appliedHwBrightness() { return sAppliedHwBrightness; }

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
        if (currentState != POWER_ACTIVE) sStateSince = now;
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

    // Record hourly battery level
    struct tm lt;
    RtcManager::getLocalTimeInfo(&lt);
    int h = lt.tm_hour % 24;
    if (sHourlyBattery[h] != (uint8_t)cachedPercent) {
        sHourlyBattery[h] = (uint8_t)cachedPercent;
        static uint32_t sLastBatSave = 0;
        if (millis() - sLastBatSave > 30000) {
            sLastBatSave = millis();
            Preferences p;
            if (p.begin("kronos_pwr", false)) {
                p.putBytes("hourly_bat", sHourlyBattery, sizeof(sHourlyBattery));
                p.end();
            }
        }
    }
}

float PowerManager::getBatteryVoltage() { return cachedVolt; }
int PowerManager::getBatteryPercent() { return cachedPercent; }
void PowerManager::getHourlyBattery(uint8_t out[24]) {
    memcpy(out, sHourlyBattery, sizeof(sHourlyBattery));
}
bool PowerManager::isBatteryConnected() { return cachedBattery; }
bool PowerManager::isCharging() { return cachedCharging; }
bool PowerManager::isVbusConnected() { return cachedVbus; }
// The AXP2101 has no charge-current ADC; only the configured charge-current
// limit is readable. Callers show the charge state (isCharging) instead.
float PowerManager::getChargeCurrentMa() { return 0.0f; }
float PowerManager::getTempCelsius() { return cachedTemp; }

// ---------------------------------------------------------------- CPU clock
// 80/160/240 MHz all keep the ESP32-S3 APB at 80 MHz, so SPI/QSPI, I2C, UART,
// I2S and the radios keep their timing; only the cores slow down.
static void setCpuMhz(uint32_t mhz) {
    if (getCpuFrequencyMhz() == mhz || !sClkMutex) return;
    if (xSemaphoreTake(sClkMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
    xSemaphoreGive(sClkMutex);
}

void PowerManager::boostCpu() { setCpuMhz(240); }
uint32_t PowerManager::cpuMhz() { return getCpuFrequencyMhz(); }

static uint32_t cpuPolicy(PowerState st) {
    uint32_t mhz = (st == POWER_ACTIVE) ? 240 : (st == POWER_DIM) ? 160 : 80;
    if (CompanionServer::isApActive() && mhz < 160) mhz = 160;          // HTTP + DNS responsiveness
    if (UsbMscManager::isMounted() || NavEngine::isRoutingBusy()) mhz = 240;
    return mhz;
}

// ---------------------------------------------------------------- light sleep
void PowerManager::uiIdle(uint32_t ms) {
    if (!sParkReqSem) { vTaskDelay(pdMS_TO_TICKS(ms)); return; }
    if (xSemaphoreTake(sParkReqSem, pdMS_TO_TICKS(ms)) != pdTRUE) return;
    instance.waitPushComplete();                       // no display DMA during sleep
    xSemaphoreGive(sParkedSem);
    // A sleep chunk is <= LIGHT_SLEEP_CHUNK_MS; the timeout is only a safety net.
    xSemaphoreTake(sResumeSem, pdMS_TO_TICKS(LIGHT_SLEEP_CHUNK_MS * 4));
}

uint32_t PowerManager::lightSleepCount() { return sLightSleeps; }
uint32_t PowerManager::lightSleepMs() { return sLightSleepMs; }

struct WakePin {
    gpio_num_t pin;
    gpio_int_type_t level;       // wake level
    gpio_int_type_t edge;        // interrupt type to restore afterwards
    bool hasIsr;                 // LilyGoLib / RadioLib attached an ISR to it
};

static bool pinActive(const WakePin& w) {
    int v = gpio_get_level(w.pin);
    return w.level == GPIO_INTR_LOW_LEVEL ? v == 0 : v == 1;
}

// Conditions under which the CPU may stop between events. Everything that
// needs the CPU or a clocked peripheral continuously keeps it awake.
static bool canLightSleep(PowerState st, unsigned long now) {
    if (st != POWER_SLEEP && st != POWER_AOD) return false;
    if (now - sStateSince < LIGHT_SLEEP_SETTLE_MS) return false;
    if (!DisplayEngine::isReady() || nav::transitioning()) return false;
    if (sTorchOn || PowerManager::isVbusConnected()) return false;   // USB: console / MSC / charging UI
    if (BleCompanion::isStackRunning()) return false;   // controller has no modem sleep in this build
    if (CompanionServer::isApActive()) return false;
    if (GpsManager::isPowered()) return false;          // UART NMEA stream
    if (NavEngine::isRoutingBusy()) return false;
    if (AudioEngine::isBusy()) return false;
    return true;
}

static void lightSleepOnce(uint32_t ms) {
    WakePin pins[] = {
        {(gpio_num_t)TP_INT,     GPIO_INTR_LOW_LEVEL,  GPIO_INTR_NEGEDGE, true},
        {(gpio_num_t)PMU_INT,    GPIO_INTR_LOW_LEVEL,  GPIO_INTR_NEGEDGE, true},
        {GPIO_NUM_0,             GPIO_INTR_LOW_LEVEL,  GPIO_INTR_DISABLE, false},
        {(gpio_num_t)SENSOR_INT, GPIO_INTR_HIGH_LEVEL, GPIO_INTR_POSEDGE, true},
        {(gpio_num_t)RTC_INT,    GPIO_INTR_LOW_LEVEL,  GPIO_INTR_NEGEDGE, true},
        {(gpio_num_t)LORA_IRQ,   GPIO_INTR_HIGH_LEVEL, GPIO_INTR_POSEDGE, true},
    };
    const int n = RadioLoRa::isEnabled() ? 6 : 5;       // LoRa IRQ only while receiving

    // A pending event (line already active) is handled first, no sleep.
    for (int i = 0; i < n; i++) if (pinActive(pins[i])) return;

    // Nobody may be inside an I2C transfer when the clocks stop.
    if (!PowerManager::lockI2C(20)) return;

    for (int i = 0; i < n; i++) {
        if (pins[i].hasIsr) gpio_intr_disable(pins[i].pin);
        gpio_wakeup_enable(pins[i].pin, pins[i].level);
    }
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);

    const int64_t t0 = esp_timer_get_time();
    esp_light_sleep_start();
    const uint32_t slept = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    for (int i = 0; i < n; i++) {
        gpio_wakeup_disable(pins[i].pin);
        gpio_set_intr_type(pins[i].pin, pins[i].edge);
        if (pins[i].hasIsr) gpio_intr_enable(pins[i].pin);
    }
    PowerManager::unlockI2C();

    // Edge interrupts are not latched while the GPIO matrix is unclocked:
    // replay every line that is still active so no event gets stuck.
    if (pinActive(pins[0])) xEventGroupSetBits(LilyGoUltra::_event, HW_IRQ_TOUCHPAD);
    if (pinActive(pins[1])) xEventGroupSetBits(LilyGoUltra::_event, HW_IRQ_POWER);
    if (pinActive(pins[3])) xEventGroupSetBits(LilyGoUltra::_event, HW_IRQ_SENSOR);
    if (pinActive(pins[4])) xEventGroupSetBits(LilyGoUltra::_event, HW_IRQ_RTC);
    if (n > 5 && pinActive(pins[5])) RadioLoRa::notifyIrq();

    sLightSleeps++;
    sLightSleepMs += slept;
}

// bg task: park the UI task, sleep one chunk, resume it
static void tryLightSleep(PowerState st, unsigned long now) {
    if (!sParkReqSem || !canLightSleep(st, now)) return;
    xSemaphoreGive(sParkReqSem);
    if (xSemaphoreTake(sParkedSem, pdMS_TO_TICKS(400)) != pdTRUE) {
        // the UI task is busy (not idling): withdraw the request if it is still unseen
        if (xSemaphoreTake(sParkReqSem, 0) == pdTRUE) return;
        // it was taken right now: the UI task is about to park, let it continue
        if (xSemaphoreTake(sParkedSem, pdMS_TO_TICKS(200)) == pdTRUE) xSemaphoreGive(sResumeSem);
        return;
    }
    // re-check: a touch / key may have arrived while we waited for the UI task
    if (canLightSleep(PowerManager::getPowerState(), millis())) lightSleepOnce(LIGHT_SLEEP_CHUNK_MS);
    xSemaphoreGive(sResumeSem);
}

// ---------------------------------------------------------------- Energiesparmodus
bool PowerManager::powerSaver() { return SettingsStorage::get().powerSaver; }

void PowerManager::setPowerSaver(bool on) {
    SystemSettings& s = SettingsStorage::get();
    if (s.powerSaver == on) return;
    if (on) {
        uint8_t mask = 0;
        if (BleCompanion::isEnabled()) { mask |= 1; BleCompanion::setEnabled(false); }
        if (RadioLoRa::isEnabled())    { mask |= 2; RadioLoRa::setEnabled(false); }
        if (s.aodEnabled)              { mask |= 4; s.aodEnabled = false; }
        if (s.wifiApEnabled || CompanionServer::isApActive()) {
            s.wifiApEnabled = false;
            CompanionServer::requestAp(false);
        }
        s.saverRestore = mask;
        s.powerSaver = true;
    } else {
        uint8_t mask = s.saverRestore;
        s.powerSaver = false;
        s.saverRestore = 0;
        if (mask & 4) s.aodEnabled = true;
        if (mask & 2) RadioLoRa::setEnabled(true);
        if (mask & 1) BleCompanion::setEnabled(true);
    }
    SettingsStorage::save();
    nav::markDirty(nav::PG_CONTROL);
    nav::markDirty(nav::PG_APP_POWER);
    nav::markDirty(nav::PG_APP_SETTINGS);
    nav::markDirty(nav::PG_SYSTEM);
}

// ---------------------------------------------------------------- loop
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

    // Telemetry: 1 s while the screen is on, 10 s while it is off (fewer PMU ADC reads)
    const uint32_t telemetryMs = isLowPowerState() ? 10000UL : 1000UL;
    if (lockI2C(30)) {
        instance.loop();                       // PMU / RTC / sensor interrupts -> events
        if (currentMillis - lastTelemetryUpdate > telemetryMs) {
            lastTelemetryUpdate = currentMillis;
            updateTelemetry();
        }
        unlockI2C();
    }

    // ---- state machine ----
    // A user-requested sleep stays in effect until the next real activity,
    // regardless of the auto-sleep setting or USB power.
    if (sSleepRequested) {
        if (currentState != POWER_SLEEP) setPowerState(POWER_SLEEP);
    } else {
        // Auto Power State transitions (with Smart Sleep power-saving override)
        bool userSleeping = MotionHaptic::isSleeping();
        uint32_t sleepThresholdMs = userSleeping ? 5000UL : (SettingsStorage::get().autoSleepSec * 1000UL);
        unsigned long idleTime = currentMillis - lastActivityTime;
        const bool aod = SettingsStorage::get().aodEnabled && !SettingsStorage::get().powerSaver;

        if (sleepThresholdMs == 0 && !userSleeping) {
            // Auto-dim / auto-sleep disabled ("Nie"): keep active
            if (currentState != POWER_ACTIVE) setPowerState(POWER_ACTIVE);
        } else if (userSleeping) {
            // While the user is asleep: display off after 6 s, even with AOD enabled
            if (idleTime > 6000UL && currentState != POWER_SLEEP) {
                setPowerState(POWER_SLEEP);
            } else if (idleTime > 3000UL && currentState == POWER_ACTIVE) {
                setPowerState(POWER_DIM);
            }
        } else if (idleTime > (sleepThresholdMs + 5000UL)) {
            // Dim after the threshold, then AOD / display off 5 s later. On USB
            // power without AOD the display only dims (charging screen).
            PowerState want = aod ? POWER_AOD : (cachedVbus ? POWER_DIM : POWER_SLEEP);
            if (currentState != want) setPowerState(want);
        } else if (idleTime > sleepThresholdMs && currentState == POWER_ACTIVE) {
            setPowerState(POWER_DIM);
        }
    }

    // ---- CPU clock ----
    setCpuMhz(cpuPolicy(currentState));

    // ---- light sleep (display off / AOD, no radio needs the CPU) ----
    tryLightSleep(currentState, currentMillis);
}
