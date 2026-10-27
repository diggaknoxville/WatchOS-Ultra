#include "MotionHaptic.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "DisplayEngine.h"
#include "ui/Navigator.h"
#include <Preferences.h>
#include <time.h>
#include <math.h>

static uint32_t sHwMask = 0;
static bool drvReady = false;
static bool sensorOnline = false;
static MotionHaptic::WristWakeCallback wristWakeCb = nullptr;

// ---- activity state (persisted in NVS namespace "kronos_fit") ----
static uint32_t dayKey = 0;            // local yyyymmdd of the tracked day
static uint32_t stepsToday = 0;
static uint32_t filteredFalseSteps = 0;
static uint16_t learnedCadenceSpm = 106; // EMA of the wearer's walking cadence (steps/min), see header
static uint16_t cadenceSamples = 0;    // number of EMA updates; the window is used from 20 on
static uint32_t history[6] = {0};      // completed days, oldest first
static bool goalNotified = false;
static bool dirty = false;
static uint32_t lifetimePending = 0;   // flushed to the lifetime counter on persist
static unsigned long lastPersist = 0;
static unsigned long lastPoll = 0;

// ---- haptic queue (producers: any task; consumer: updateMotion on the bg task) ----
static QueueHandle_t sHapticQueue = nullptr;          // 4 entries of int (HapticPattern)
static unsigned long sLastHapticMs = 0;
static const unsigned long HAPTIC_MIN_GAP_MS = 70;    // let one effect finish before the next

// ---- BHI260AP callbacks & real-time 6DoF Euler orientation ----
static uint32_t lastSensorCount = 0;
static volatile uint32_t pendingCount = 0;
static volatile bool countUpdated = false;
static volatile bool wristEvent = false;
static volatile uint16_t pendingActivityBitmap = 0;
static volatile bool activityUpdated = false;

static float sRawRoll = 0.0f, sRawPitch = 0.0f, sRawYaw = 0.0f;
static float sRollZero = 0.0f, sPitchZero = 0.0f, sYawZero = 0.0f;
static float sPrevRoll = 0.0f, sPrevPitch = 0.0f, sPrevYaw = 0.0f;
static float sWristJitterDeg = 0.0f; // smoothed angular velocity/jitter (detects hand shaking/waving)

// ---- Step cadence gate & sleep classifier ----
static ActivityState sActState = ACT_STILL;
static bool sBhiWalking = false;
static bool sBhiRunning = false;
static bool sBhiCycling = false;
static bool sBhiVehicle = false;
static bool sBhiTilting = false;

static uint32_t sBufferedSteps = 0;          // 4-step confirmation buffer
static uint8_t  sRhythmicStreak = 0;         // consecutive rhythmic step batches
static unsigned long sLastStepBatchMs = 0;
static unsigned long sLastConfirmedWalkMs = 0;
static unsigned long sStillStartMs = 0;
static bool sSleeping = false;

// Absolute plausibility limits for the interval between two steps.
static const float CAD_ABS_MIN_MS = 240.0f;          // 250 spm: faster than any human step
static const float CAD_ABS_MAX_MS = 1450.0f;         // ~41 spm: slower than a shuffle
static const unsigned long BOUT_GAP_MS = 3000;       // a longer gap = new bout, interval not a cadence
static const uint16_t CAD_LEARN_MIN_SAMPLES = 20;    // learned window only after this many updates

static void playHapticOnI2c(HapticPattern pattern) {
    if (!drvReady || !SettingsStorage::get().hapticEnabled) return;
    if (!PowerManager::lockI2C(15)) {
        // bus busy: put it back at the head of the queue and retry next loop
        int p = (int)pattern;
        if (sHapticQueue) xQueueSendToFront(sHapticQueue, &p, 0);
        return;
    }
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
    p.putUInt("fsteps", filteredFalseSteps);
    p.putUShort("cadence", learnedCadenceSpm);
    p.putUShort("cad_n", cadenceSamples);
    p.putFloat("r0", sRollZero);
    p.putFloat("p0", sPitchZero);
    p.putFloat("y0", sYawZero);
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
        long gap = (long)(today - keyToDays(dayKey));
        if (gap < 1) gap = 1;
        for (long i = 0; i < gap && i < 7; i++) {
            memmove(history, history + 1, sizeof(history) - sizeof(history[0]));
            history[5] = (i == 0) ? stepsToday : 0;
        }
    }
    Serial.printf("[FIT] new day %lu (previous day %lu steps, %lu filtered)\n",
                  (unsigned long)key, (unsigned long)stepsToday, (unsigned long)filteredFalseSteps);
    dayKey = key;
    stepsToday = 0;
    filteredFalseSteps = 0;
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

static void onActivityRec(uint8_t, uint8_t* data, uint32_t, uint64_t*, void*) {
    pendingActivityBitmap = (uint16_t)((data[1] << 8) | data[0]);
    activityUpdated = true;
}

static void onGameRotVec(uint8_t, uint8_t* data, uint32_t, uint64_t*, void*) {
    float r = 0, p = 0, y = 0;
    bhy2_quaternion_to_euler(data, &r, &p, &y);
    if (isfinite(r) && isfinite(p) && isfinite(y)) {
        float dr = fabsf(r - sPrevRoll);
        float dp = fabsf(p - sPrevPitch);
        if (dr > 180.0f) dr = 360.0f - dr;
        if (dp > 180.0f) dp = 360.0f - dp;
        sWristJitterDeg = sWristJitterDeg * 0.80f + (dr + dp) * 0.20f;
        sPrevRoll = sRawRoll = r;
        sPrevPitch = sRawPitch = p;
        sPrevYaw = sRawYaw = y;
    }
}

void MotionHaptic::setHardwareMask(uint32_t mask) { sHwMask = mask; }

static volatile bool sGameModeReq = false;
static bool sGameModeApplied = false;
void MotionHaptic::setGameMode(bool on) { sGameModeReq = on; }

bool MotionHaptic::init() {
    const uint32_t mask = sHwMask ? sHwMask : instance.getDeviceProbe();
    drvReady = (mask & HW_DRV_ONLINE) != 0;
    sensorOnline = (mask & HW_BHI260AP_ONLINE) != 0;

    if (!sHapticQueue) sHapticQueue = xQueueCreate(4, sizeof(int));
    Serial.printf("[HAPTIC] DRV2605 via instance.drv: %s\n", drvReady ? "online" : "NOT FOUND");
    if (drvReady) {
        instance.drv.selectLibrary(1);
        instance.drv.setMode(SensorDRV2605::MODE_INTTRIG);
    }

    Preferences p;
    if (p.begin("kronos_fit", false)) {
        dayKey = p.getUInt("day", 0);
        stepsToday = p.getUInt("steps", 0);
        filteredFalseSteps = p.getUInt("fsteps", 0);
        learnedCadenceSpm = p.getUShort("cadence", 106);
        cadenceSamples = p.getUShort("cad_n", 0);
        if (learnedCadenceSpm < 70 || learnedCadenceSpm > 175) { learnedCadenceSpm = 106; cadenceSamples = 0; }
        sRollZero = p.getFloat("r0", 0.0f);
        sPitchZero = p.getFloat("p0", 0.0f);
        sYawZero = p.getFloat("y0", 0.0f);
        p.getBytes("hist", history, sizeof(history));
        goalNotified = p.getBool("goal", false);
        p.end();
    }

    sStillStartMs = millis();

    if (sensorOnline) {
        instance.sensor.configure(SensorBHI260AP::STEP_COUNTER, 1.0f, 0);
        instance.sensor.onResultEvent(SensorBHI260AP::STEP_COUNTER, onStepCounter);
        instance.sensor.configure(SensorBHI260AP::WRIST_TILT_GESTURE, 1.0f, 0);
        instance.sensor.onResultEvent(SensorBHI260AP::WRIST_TILT_GESTURE, onWristTilt);
        // Hardware Activity Recognition DSP (Walking, Running, Cycling, Vehicle, Still, Tilting)
        instance.sensor.configure(SensorBHI260AP::ACTIVITY_RECOGNITION, 1.0f, 0);
        instance.sensor.onResultEvent(SensorBHI260AP::ACTIVITY_RECOGNITION, onActivityRec);
        // 6DoF Game Rotation Vector (20 Hz) for bubble level, gyro compass & hand-waving rejection
        instance.sensor.configure(SensorBHI260AP::GAME_ROTATION_VECTOR, 20.0f, 0);
        instance.sensor.onResultEvent(SensorBHI260AP::GAME_ROTATION_VECTOR, onGameRotVec);
    }
    Serial.printf("[FIT] BHI260AP %s, restored %lu steps (%lu filtered, cadence %u SPM from %u samples)\n",
                  sensorOnline ? "online" : "OFFLINE", (unsigned long)stepsToday,
                  (unsigned long)filteredFalseSteps, (unsigned)learnedCadenceSpm, (unsigned)cadenceSamples);
    return drvReady || sensorOnline;
}

static void commitVerifiedSteps(uint32_t count, unsigned long now) {
    if (count == 0) return;
    stepsToday += count;
    lifetimePending += count;
    sLastConfirmedWalkMs = now;
    sStillStartMs = now;
    if (sSleeping) {
        sSleeping = false;
        Serial.println("[FIT] Smart Sleep ended by confirmed walking steps");
    }
    dirty = true;
    nav::markDirty(nav::PG_ACTIVITY);
    nav::markDirty(nav::PG_FACE);

    uint32_t goal = SettingsStorage::get().dailyStepGoal;
    if (!goalNotified && goal && stepsToday >= goal) {
        goalNotified = true;
        MotionHaptic::triggerHaptic(HAPTIC_SUCCESS);
        char msg[64];
        snprintf(msg, sizeof(msg), "%lu Schritte heute. Tagesziel erreicht!", (unsigned long)stepsToday);
        DisplayEngine::showNotification("Tagesziel erreicht", msg, 6000);
        persist();
    }
}

void MotionHaptic::updateMotion(unsigned long now) {
    // Haptic queue: one effect per pass, with a minimum gap so none is swallowed
    if (sHapticQueue && now - sLastHapticMs >= HAPTIC_MIN_GAP_MS) {
        int hp;
        if (xQueueReceive(sHapticQueue, &hp, 0) == pdTRUE) {
            sLastHapticMs = now;
            playHapticOnI2c((HapticPattern)hp);
        }
    }

    // Throttle sensor FIFO polling during Smart Sleep to save power; a running
    // game polls at 50 Hz and gets the orientation at 50 Hz.
    if (sGameModeReq != sGameModeApplied && sensorOnline && PowerManager::lockI2C(15)) {
        sGameModeApplied = sGameModeReq;
        instance.sensor.configure(SensorBHI260AP::GAME_ROTATION_VECTOR, sGameModeApplied ? 50.0f : 20.0f, 0);
        PowerManager::unlockI2C();
    }
    uint32_t pollInterval = sSleeping ? 200 : (sGameModeApplied ? 20 : 40);
    if (sensorOnline && now - lastPoll >= pollInterval) {
        lastPoll = now;
        if (PowerManager::lockI2C(15)) {
            instance.sensor.update();
            PowerManager::unlockI2C();
        }
    }

    rolloverIfNeeded();

    // 1. Decode BHI260AP hardware activity recognition events
    if (activityUpdated) {
        activityUpdated = false;
        uint16_t bm = pendingActivityBitmap;
        // Bit 0..6: ended (Still=0, Walk=1, Run=2, Bike=3, Vehicle=4, Tilt=5)
        // Bit 8..14: started (Still=8, Walk=9, Run=10, Bike=11, Vehicle=12, Tilt=13)
        if (bm & (1 << 1)) sBhiWalking = false;
        if (bm & (1 << 2)) sBhiRunning = false;
        if (bm & (1 << 3)) sBhiCycling = false;
        if (bm & (1 << 4)) sBhiVehicle = false;
        if (bm & (1 << 5)) sBhiTilting = false;

        if (bm & (1 << 9))  { sBhiWalking = true; sStillStartMs = now; }
        if (bm & (1 << 10)) { sBhiRunning = true; sStillStartMs = now; }
        if (bm & (1 << 11)) { sBhiCycling = true; sStillStartMs = now; }
        if (bm & (1 << 12)) { sBhiVehicle = true; }
        if (bm & (1 << 13)) { sBhiTilting = true; }
    }

    // 2. Step cadence gate & hand-waving rejection
    if (countUpdated) {
        countUpdated = false;
        uint32_t c = pendingCount;
        uint32_t delta = (c >= lastSensorCount) ? c - lastSensorCount : c;
        lastSensorCount = c;

        if (delta > 0 && delta < 500) {
            const unsigned long dt = sLastStepBatchMs ? (now - sLastStepBatchMs) : 0;
            sLastStepBatchMs = now;

            // The step counter arrives in batches. The interval between two
            // batches divided by the step delta approximates the cadence only
            // while walking continuously. After a pause (> BOUT_GAP_MS) the
            // interval spans the pause itself, so the first batch of a new bout
            // is NOT classified: it is buffered and confirmed by the next batch.
            const bool boutStart = (dt == 0 || dt > BOUT_GAP_MS);
            const float msPerStep = boutStart ? 0.0f : ((float)dt / (float)delta);
            const bool inBout = (now - sLastConfirmedWalkMs < 4000) || sBhiWalking || sBhiRunning;

            // Plausibility window: absolute limits inside a confirmed bout or
            // while the BHI reports running; the learned walking cadence +/- 35 %
            // (clamped to the absolute limits) when confirming a new bout.
            float minMs = CAD_ABS_MIN_MS, maxMs = CAD_ABS_MAX_MS;
            if (!inBout && !sBhiRunning && cadenceSamples >= CAD_LEARN_MIN_SAMPLES) {
                const float learnedMs = 60000.0f / (float)learnedCadenceSpm;
                minMs = fmaxf(CAD_ABS_MIN_MS, learnedMs * 0.65f);
                maxMs = fminf(CAD_ABS_MAX_MS, learnedMs * 1.35f);
            }
            const bool plausibleCadence = boutStart || (msPerStep >= minMs && msPerStep <= maxMs);

            // Hand-waving / wrist shaking:
            //  - high rotational wrist jitter (> 24 deg/sample) while NOT in confirmed walking state, OR
            //  - isolated 1-2 step spike with tilting and no rhythm so far
            const bool wristWaving = (sWristJitterDeg > 24.0f && !sBhiWalking && !sBhiRunning) ||
                                     (sBhiTilting && !sBhiWalking && delta <= 2 && sRhythmicStreak == 0);

            if (wristWaving || !plausibleCadence) {
                // Reject buffered & current steps as hand motion
                filteredFalseSteps += delta + sBufferedSteps;
                sBufferedSteps = 0;
                sRhythmicStreak = 0;
                sActState = ACT_HAND_MOTION;
                dirty = true;
                nav::markDirty(nav::PG_ACTIVITY);
            } else if (inBout) {
                // Active walking bout: commit immediately, adapt the learned cadence
                commitVerifiedSteps(sBufferedSteps + delta, now);
                sBufferedSteps = 0;
                if (!boutStart && !sBhiRunning && msPerStep >= 320.0f && msPerStep <= 1000.0f) {
                    const float instSpm = 60000.0f / msPerStep;
                    learnedCadenceSpm = (uint16_t)lroundf(learnedCadenceSpm * 0.90f + instSpm * 0.10f);
                    if (cadenceSamples < 60000) cadenceSamples++;
                }
                sActState = ((!boutStart && msPerStep < 430.0f) || sBhiRunning) ? ACT_RUNNING : ACT_WALKING;
            } else {
                // New bout: require 4 buffered steps or 3 rhythmic batches before committing
                sBufferedSteps += delta;
                if (!boutStart) sRhythmicStreak++;
                if (sBufferedSteps >= 4 || sRhythmicStreak >= 3) {
                    commitVerifiedSteps(sBufferedSteps, now);
                    sBufferedSteps = 0;
                    sActState = ACT_WALKING;
                }
            }
        }
    }

    // Expire buffered steps if no follow-up steps arrived within 4.5 seconds (isolated arm movement)
    if (sBufferedSteps > 0 && (now - sLastStepBatchMs > 4500)) {
        filteredFalseSteps += sBufferedSteps;
        sBufferedSteps = 0;
        sRhythmicStreak = 0;
        sActState = ACT_HAND_MOTION;
        dirty = true;
        nav::markDirty(nav::PG_ACTIVITY);
    }

    // 3. Smart Sleep / bedtime classifier
    struct tm lt;
    RtcManager::getLocalTimeInfo(&lt);
    bool isNightWindow = (lt.tm_hour >= 22 || lt.tm_hour < 7);
    unsigned long stillDurMs = now - sStillStartMs;
    bool flatBedPosture = (fabsf(sRawPitch) < 38.0f && sWristJitterDeg < 3.5f);

    if (!sBhiWalking && !sBhiRunning && (now - sLastConfirmedWalkMs > 8000)) {
        if ((isNightWindow && stillDurMs > 12UL * 60UL * 1000UL && flatBedPosture) ||
            (stillDurMs > 35UL * 60UL * 1000UL && flatBedPosture && !PowerManager::isVbusConnected())) {
            if (!sSleeping) {
                sSleeping = true;
                Serial.println("[FIT] Entered Smart Sleep Mode (wrist-wake suppressed, display sleep after 6 s)");
                nav::markDirty(nav::PG_ACTIVITY);
            }
            sActState = ACT_SLEEPING;
        } else if (sBhiCycling) {
            sActState = ACT_CYCLING;
        } else if (sBhiVehicle) {
            sActState = ACT_VEHICLE;
        } else if (sActState != ACT_HAND_MOTION || (now - sLastStepBatchMs > 3500)) {
            sActState = ACT_STILL;
        }
    }

    if (dirty && now - lastPersist > 60000) { lastPersist = now; persist(); }

    // 4. Wrist-wake with Smart Sleep suppression
    if (wristEvent) {
        wristEvent = false;
        // Do NOT wake the display from wrist turns in bed while sleeping
        if (!sSleeping && SettingsStorage::get().wristWakeEnabled && PowerManager::getPowerState() != POWER_ACTIVE) {
            PowerManager::handleActivity();
            triggerHaptic(HAPTIC_TICK);
            if (wristWakeCb) wristWakeCb();
        }
    }
}

void MotionHaptic::triggerHaptic(HapticPattern pattern) {
    if (!drvReady || !SettingsStorage::get().hapticEnabled || !sHapticQueue) return;
    int p = (int)pattern;
    xQueueSend(sHapticQueue, &p, 0);     // queue full (4) -> effect dropped
}

// Gravity direction derived from the orientation (Euler angles of the game
// rotation vector), not a raw accelerometer sample: no linear acceleration,
// always unit length. Sufficient for the bubble level.
void MotionHaptic::getAccel(float &ax, float &ay, float &az) {
    float r = (sRawRoll - sRollZero) * (float)M_PI / 180.0f;
    float p = (sRawPitch - sPitchZero) * (float)M_PI / 180.0f;
    ax = sinf(r);
    ay = sinf(p);
    az = cosf(r) * cosf(p);
}

void MotionHaptic::getEuler(float &roll, float &pitch, float &yaw) {
    roll = sRawRoll - sRollZero;
    pitch = sRawPitch - sPitchZero;
    float h = fmodf((sRawYaw - sYawZero) + 360.0f, 360.0f);
    if (h < 0) h += 360.0f;
    yaw = h;
}

float MotionHaptic::getHeadingDeg() {
    float r, p, y;
    getEuler(r, p, y);
    return y;
}

void MotionHaptic::calibrateLevelAndCompass() {
    sRollZero = sRawRoll;
    sPitchZero = sRawPitch;
    sYawZero = sRawYaw;
    dirty = true;
    persist();
}

uint32_t MotionHaptic::getStepsToday() { return stepsToday; }
uint32_t MotionHaptic::getFilteredFalseSteps() { return filteredFalseSteps; }
uint16_t MotionHaptic::getLearnedCadenceSpm() { return learnedCadenceSpm; }
ActivityState MotionHaptic::getActivityState() { return sActState; }
bool MotionHaptic::isSleeping() { return sSleeping; }

const char* MotionHaptic::getActivityLabel() {
    switch (sActState) {
        case ACT_WALKING:     return "GEHEN";
        case ACT_RUNNING:     return "LAUFEN";
        case ACT_CYCLING:     return "RADFAHREN";
        case ACT_VEHICLE:     return "FAHRZEUG";
        case ACT_HAND_MOTION: return "HAND-FILTER";
        case ACT_SLEEPING:    return "SCHLAF-MODUS";
        default:              return "RUHE / STEHEN";
    }
}

void MotionHaptic::resetSteps() {
    stepsToday = 0;
    filteredFalseSteps = 0;
    sBufferedSteps = 0;
    sRhythmicStreak = 0;
    goalNotified = false;          // the goal can be reached again today
    dirty = true;
    nav::markDirty(nav::PG_ACTIVITY);
    nav::markDirty(nav::PG_FACE);
}

// Approximations without a user profile: a fixed 0.75 m stride and
// ~0.042 kcal per step (about a 70 kg person walking). Shown as estimates.
float MotionHaptic::getDistanceKm() { return (stepsToday * 0.75f) / 1000.0f; }
uint32_t MotionHaptic::getCalories() { return (uint32_t)(stepsToday * 0.042f); }
bool MotionHaptic::isSensorOnline() { return sensorOnline; }

void MotionHaptic::getHistory(uint32_t out[7]) {
    for (int i = 0; i < 6; i++) out[i] = history[i];
    out[6] = stepsToday;
}

void MotionHaptic::setWristWakeCallback(WristWakeCallback cb) { wristWakeCb = cb; }
