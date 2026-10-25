#pragma once
#include "Config.h"

// SX1262 LoRa (shared LilyGoLib `radio`). Supports both KRONOS Direct (868.0 MHz)
// and Meshtastic EU_868 LongFast (869.525 MHz, AES-128 CTR, Position/NodeInfo/Text).
struct LoraMessage {
    char text[72];
    float rssi;
    float snr;
    uint32_t epoch;     // UTC receive time
};

struct LoraPeerNode {
    uint32_t nodeId;
    char name[20];
    double lat;
    double lon;
    float rssi;
    float snr;
    uint32_t epoch;
    bool hasPos;
    bool isSos;
    bool isMeshtastic;
};

class RadioLoRa {
public:
    static bool init(float freq = 868.0f);
    static void loop(unsigned long now);          // background task only

    static bool sendPacket(const String& msg);    // thread-safe (queued)
    static bool sendSosBeacon();                  // thread-safe (queued)
    static bool sendWaypoint(const char* name, double lat, double lon);
    static bool sendPositionBeacon();
    static bool pollPacket(String& outMsg, float& outRssi, float& outSnr);

    static bool isReady();
    static void setEnabled(bool en);
    static bool isEnabled();

    // Switch between KRONOS Direct (868.0 MHz SF9) and Meshtastic EU_868 LongFast (869.525 MHz SF11)
    static void setMeshtasticMode(bool mesh);
    static bool isMeshtasticMode();

    static uint32_t getSentCount();
    static uint32_t getRecvCount();
    static float getLastRssi();
    static float getLastSnr();
    static String getLastReceivedMessage();
    static uint8_t getInbox(LoraMessage* out, uint8_t max);   // newest first

    // Mesh Peer Table & Mesh-Assisted Navigation Positioning
    static uint8_t getPeers(LoraPeerNode* out, uint8_t max);
    static uint8_t getPeerCount();
    static bool getMeshFallbackPosition(double& outLat, double& outLon, char* outSourceName = nullptr, size_t nameLen = 0);
    static void injectDemoPeers(); // Seeds realistic nearby mesh peers if none received yet for testing
};
