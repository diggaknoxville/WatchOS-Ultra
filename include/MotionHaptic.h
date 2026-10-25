#pragma once
#include "Config.h"
#include <LilyGoLib.h>   // haptic (instance.drv) + sensor (instance.sensor)

enum HapticPattern {
    HAPTIC_TICK = 1,
    HAPTIC_CLICK = 2,
    HAPTIC_DOUBLE_CLICK = 3,
    HAPTIC_ALERT_SHORT = 4,
    HAPTIC_ALERT_LONG = 5,
    HAPTIC_SUCCESS = 6,
    HAPTIC_HEARTBEAT = 7
};

enum ActivityState : uint8_t {
    ACT_STILL       = 0, // Ruhe / Stehen
    ACT_WALKING     = 1, // Gehen (bestätigte Schritt-Kadenz)
    ACT_RUNNING     = 2, // Laufen / Joggen
    ACT_CYCLING     = 3, // Radfahren
    ACT_VEHICLE     = 4, // Fahrzeug
    ACT_HAND_MOTION = 5, // Handbewegung / Wackeln (herausgefiltert)
    ACT_SLEEPING    = 6  // Schlaf-Modus (Nacht-/Ruhe-Stromsparen)
};

// BHI260AP step counting with a cadence plausibility gate, BHI activity
// recognition, bedtime detection, 6DoF orientation and DRV2605 haptics.
//
// What is learned: `learnedCadenceSpm` is an exponential moving average of the
// wearer's walking cadence (steps/min) measured inside confirmed walking bouts.
// Once at least 20 samples exist it defines the plausibility window
// (learned +/- 35 %, clamped to 240..1450 ms per step) used to confirm the
// start of a NEW walking bout. Inside a confirmed bout and whenever the BHI
// reports running, only the absolute limits apply. Nothing else adapts.
class MotionHaptic {
public:
    // Hardware probe mask from instance.begin(); must be set before init().
    // Without it (0) the mask is taken from instance.getDeviceProbe().
    static void setHardwareMask(uint32_t mask);

    static bool init();
    static void triggerHaptic(HapticPattern pattern);   // queued (4 entries), played on the bg task
    static void updateMotion(unsigned long now);

    // Orientation-derived gravity vector (unit length), NOT a raw accelerometer
    // reading: computed from the game-rotation-vector Euler angles.
    static void getAccel(float &ax, float &ay, float &az);
    static void getEuler(float &roll, float &pitch, float &yaw);
    static float getHeadingDeg();
    static void calibrateLevelAndCompass();

    static uint32_t getStepsToday();
    static uint32_t getFilteredFalseSteps();
    static uint16_t getLearnedCadenceSpm();
    static ActivityState getActivityState();
    static const char* getActivityLabel();
    static bool isSleeping();

    static void resetSteps();                  // also re-arms the daily goal notification
    static float getDistanceKm();              // steps * 0.75 m (fixed stride, no user profile)
    static uint32_t getCalories();             // steps * 0.042 kcal (rough estimate, no user profile)
    static void getHistory(uint32_t out[7]);   // oldest .. today
    static bool isSensorOnline();

    typedef void (*WristWakeCallback)();
    static void setWristWakeCallback(WristWakeCallback cb);
};
