#include "MotionHaptic.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "DisplayEngine.h"
#include <Preferences.h>
#include <time.h>

static bool drvReady = false;
static bool sensorOnline = false;
static MotionHaptic::WristWakeCallback wristWakeCb = nullptr;

// ---- activity state (persisted in NVS namespace "kronos_fit") ----
static uint32_t dayKey = 0;            // local yyyymmdd of the tracked day
static uint32_t stepsToday = 0;
static uint32_t history[6] = {0};      // completed days, oldest first
static bool goalNotified = false;
static bool dirty = false;
static uint32_t lifetimePending = 0;   // flushed to the lifetime counter on persist
static unsigned long lastPersist = 0;
static unsigned long lastPoll = 0;

// values handed over from the sensor callbacks (run inside sensor.update())
static uint32_t lastSensorCount = 0;
static volatile uint32_t pendingCount = 0;
static volatile bool countUpdated = false;
static volatile bool wristEvent = false;
static volatile int sPendingHaptic = -1;

static void playHapticOnI2c(HapticPattern pattern) {
    if (!drvReady || !SettingsStorage::get().hapticEnabled) return;
    if (!PowerManager::lockI2C(15)) return;
    instance.drv.selectLibrary(1);
    instance.drv.setMode(SensorDRV2605::MODE_INTTRIG);
    switch (pattern) {
        case HAPTIC_TICK:         instance.drv.setWaveform(0, 14); instance.drv.setWaveform(1, 0); break;
        case HAPTIC_CLICK:        instance.drv.setWaveform(0, 1);  instance.drv.setWaveform(1, 0); break;
        case HAPTIC_DOUBLE_CLICK: instance.drv.setWaveform(0, 1);  instance.drv.setWaveform(1, 10); instance.drv.setWaveform(2, 0); break;
        case HAPTIC_ALERT_SHORT:  instance.drv.setWaveform(0, 58); instance.drv.setWaveform(1, 0); break;
        case HAPTIC_ALERT_LONG:   instance.drv.setWaveform(0, 47); instance.drv.setWaveform(1, 0); break;
        case HAPTIC_SUCCESS:      instance.drv.setWaveform(0, 14); instance.drv.setWaveform(1, 15); instance.drv.setWaveform(2, 0); break;
        case HAPTIC_HEARTBEAT:    instance.drv.setWaveform(0, 82); instance.drv.setWaveform(1, 0); break;
    }
    instance.drv.run();
    PowerManager::unlockI2C();
}

static void persist() {
    Preferences p;
    if (!p.begin("kronos_fit", false)) return;
    p.putUInt("day", dayKey);
    p.putUInt("steps", stepsToday);
    p.putBytes("hist", history, sizeof(history));
    p.putBool("goal", goalNotified);
    p.end();
    if (lifetimePending) { SettingsStorage::addSteps(lifetimePending); lifetimePending = 0; }
    dirty = false;
}

static uint32_t todayKey(time_t* outDays = nullptr) {
    struct tm lt;
    RtcManager::getLocalTimeInfo(&lt);
    if (outDays) *outDays = RtcManager::epochFromUtc(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, 0, 0, 0) / 86400;
    return (uint32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday);
}

static time_t keyToDays(uint32_t key) {
    return RtcManager::epochFromUtc(key / 10000, (key / 100) % 100, key % 100, 0, 0, 0) / 86400;
}

static void rolloverIfNeeded() {
    if (!RtcManager::isTimeValid()) return;
    time_t today;
    uint32_t key = todayKey(&today);
    if (key == dayKey) return;
    if (dayKey != 0) {
        long gap = (long)(today - keyToDays(dayKey));     // days elapsed
        if (gap < 1) gap = 1;
        for (long i = 0; i < gap && i < 7; i++) {
            memmove(history, history + 1, sizeof(history) - sizeof(history[0]));
            history[5] = (i == 0) ? stepsToday : 0;
        }
    }
    Serial.printf("[FIT] new day %lu (previous day %lu steps)\n", (unsigned long)key, (unsigned long)stepsToday);
    dayKey = key;
    stepsToday = 0;
    goalNotified = false;
    persist();
}

static void onStepCounter(uint8_t, uint8_t* data, uint32_t, uint64_t*, void*) {
    pendingCount = bhy2_parse_step_counter(data);
    countUpdated = true;
}

static void onWristTilt(uint8_t, uint8_t*, uint32_t, uint64_t*, void*) {
    wristEvent = true;
}

bool MotionHaptic::init() {
    Serial.println("[HAPTIC] DRV2605 via instance.drv");
    instance.drv.selectLibrary(1);
    instance.drv.setMode(SensorDRV2605::MODE_INTTRIG);
    drvReady = true;

    Preferences p;
    if (p.begin("kronos_fit", false)) {
        dayKey = p.getUInt("day", 0);
        stepsToday = p.getUInt("steps", 0);
        p.getBytes("hist", history, sizeof(history));
        goalNotified = p.getBool("goal", false);
        p.end();
    }

    sensorOnline = (instance.getDeviceProbe() & HW_BHI260AP_ONLINE) != 0;
    if (sensorOnline) {
        instance.sensor.configure(SensorBHI260AP::STEP_COUNTER, 1.0f, 0);
        instance.sensor.onResultEvent(SensorBHI260AP::STEP_COUNTER, onStepCounter);
        instance.sensor.configure(SensorBHI260AP::WRIST_TILT_GESTURE, 1.0f, 0);
        instance.sensor.onResultEvent(SensorBHI260AP::WRIST_TILT_GESTURE, onWristTilt);
    }
    Serial.printf("[FIT] BHI260AP %s, restored %lu steps for day %lu\n",
                  sensorOnline ? "online" : "OFFLINE", (unsigned long)stepsToday, (unsigned long)dayKey);
    return true;
}

void MotionHaptic::updateMotion(unsigned long now) {
    int hp = sPendingHaptic;
    if (hp >= 0) {
        sPendingHaptic = -1;
        playHapticOnI2c((HapticPattern)hp);
    }

    if (sensorOnline && now - lastPoll >= 40) {
        lastPoll = now;
        if (PowerManager::lockI2C(15)) {
            instance.sensor.update();              // drains the hub FIFO -> callbacks
            PowerManager::unlockI2C();
        }
    }

    rolloverIfNeeded();

    if (countUpdated) {
        countUpdated = false;
        uint32_t c = pendingCount;
        uint32_t delta = (c >= lastSensorCount) ? c - lastSensorCount : c;   // hub reset -> restart
        lastSensorCount = c;
        if (delta > 0 && delta < 2000) {           // ignore implausible jumps
            stepsToday += delta;
            lifetimePending += delta;
            dirty = true;
            uint32_t goal = SettingsStorage::get().dailyStepGoal;
            if (!goalNotified && goal && stepsToday >= goal) {
                goalNotified = true;
                triggerHaptic(HAPTIC_SUCCESS);
                char msg[64];
                snprintf(msg, sizeof(msg), "%lu steps today. Daily goal reached!", (unsigned long)stepsToday);
                DisplayEngine::showNotification("Goal reached", msg, 6000);
                persist();
            }
        }
    }
    if (dirty && now - lastPersist > 60000) { lastPersist = now; persist(); }

    if (wristEvent) {
        wristEvent = false;
        if (SettingsStorage::get().wristWakeEnabled && PowerManager::getPowerState() != POWER_ACTIVE) {
            PowerManager::handleActivity();
            triggerHaptic(HAPTIC_TICK);
            if (wristWakeCb) wristWakeCb();
        }
    }
}

void MotionHaptic::triggerHaptic(HapticPattern pattern) {
    if (!drvReady || !SettingsStorage::get().hapticEnabled) return;
    // Queue haptic pattern for Core 0 so UI thread (Core 1) never touches I2C Wire
    sPendingHaptic = (int)pattern;
}

void MotionHaptic::getAccel(float &ax, float &ay, float &az) { ax = 0; ay = 0; az = 1; }
uint32_t MotionHaptic::getStepsToday() { return stepsToday; }
void MotionHaptic::resetSteps() { stepsToday = 0; dirty = true; }
float MotionHaptic::getDistanceKm() { return (stepsToday * 0.75f) / 1000.0f; }
uint32_t MotionHaptic::getCalories() { return (uint32_t)(stepsToday * 0.042f); }
bool MotionHaptic::isSensorOnline() { return sensorOnline; }

void MotionHaptic::getHistory(uint32_t out[7]) {
    for (int i = 0; i < 6; i++) out[i] = history[i];
    out[6] = stepsToday;
}

void MotionHaptic::setWristWakeCallback(WristWakeCallback cb) { wristWakeCb = cb; }
