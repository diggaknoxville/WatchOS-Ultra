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

// Activity + Adaptive Step Learning + Sleep Detection + 6DoF Euler Orientation + Haptics
class MotionHaptic {
public:
    static bool init();
    static void triggerHaptic(HapticPattern pattern);
    static void updateMotion(unsigned long now);

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

    static void resetSteps();
    static float getDistanceKm();
    static uint32_t getCalories();
    static void getHistory(uint32_t out[7]);   // oldest .. today
    static bool isSensorOnline();

    typedef void (*WristWakeCallback)();
    static void setWristWakeCallback(WristWakeCallback cb);
};
