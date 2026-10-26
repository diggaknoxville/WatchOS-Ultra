#pragma once
#include <Arduino.h>

// USB Mass Storage Class (TinyUSB MSC).
// Exposes the microSD card over USB-C to PC/Mac/Linux.
class UsbMscManager {
public:
    static bool init();
    static void loop(unsigned long now);
    static bool isAvailable();
    static bool isMounted();
    static bool isConnected();
    static void setEnabled(bool en);
    static bool isEnabled();
    static uint32_t getSectorCount();
};

