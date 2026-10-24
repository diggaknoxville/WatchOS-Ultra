#pragma once
#include <Arduino.h>

class BleCompanion {
public:
    static bool init();
    static void loop(unsigned long now);
    static bool isConnected();
    static void setEnabled(bool en);
    static bool isEnabled();

    // Trigger update notification to client
    static void notifyStatus();
};

