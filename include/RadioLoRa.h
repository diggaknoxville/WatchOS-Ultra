#pragma once
#include "Config.h"

// SX1262 LoRa (shared LilyGoLib `radio`). All radio I/O happens on the
// background task inside loop(); other tasks only enqueue packets, which keeps
// SPI access single-threaded. Reception is interrupt driven (DIO1).
struct LoraMessage {
    char text[72];
    float rssi;
    float snr;
    uint32_t epoch;     // UTC receive time
};

class RadioLoRa {
public:
    static bool init(float freq = 868.0f);
    static void loop(unsigned long now);          // background task only

    static bool sendPacket(const String& msg);    // thread-safe (queued)
    static bool sendSosBeacon();                  // thread-safe (queued)
    static bool pollPacket(String& outMsg, float& outRssi, float& outSnr);  // legacy no-op

    static bool isReady();
    static void setEnabled(bool en);
    static bool isEnabled();
    static uint32_t getSentCount();
    static uint32_t getRecvCount();
    static float getLastRssi();
    static float getLastSnr();
    static String getLastReceivedMessage();
    static uint8_t getInbox(LoraMessage* out, uint8_t max);   // newest first
};
