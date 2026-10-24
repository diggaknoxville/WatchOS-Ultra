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

// Activity + haptics on the T-Watch Ultra.
//  * Steps come from the BHI260AP hardware step counter (runs on the sensor
//    hub, so counting continues while the ESP32 is busy).
//  * Today's steps, the day key and a 7-day history are persisted in NVS and
//    survive reboots / power loss. Day rollover follows local time.
//  * The BHI260AP wrist-tilt gesture wakes the display.
class MotionHaptic {
public:
    static bool init();
    static void triggerHaptic(HapticPattern pattern);
    static void updateMotion(unsigned long now);

    static void getAccel(float &ax, float &ay, float &az);
    static uint32_t getStepsToday();
    static void resetSteps();
    static float getDistanceKm();
    static uint32_t getCalories();
    static void getHistory(uint32_t out[7]);   // oldest .. today
    static bool isSensorOnline();

    typedef void (*WristWakeCallback)();
    static void setWristWakeCallback(WristWakeCallback cb);
};
