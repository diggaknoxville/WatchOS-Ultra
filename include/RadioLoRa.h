#pragma once
#include "Config.h"

enum MeshPreset : uint8_t {
    MESH_PRESET_LONG_FAST  = 0, // 869.525 MHz, BW 250 kHz, SF11, CR 4/5 (Meshtastic EU_868 Standard)
    MESH_PRESET_MED_FAST   = 1, // 869.525 MHz, BW 250 kHz, SF9,  CR 4/5
    MESH_PRESET_SHORT_FAST = 2, // 869.525 MHz, BW 250 kHz, SF7,  CR 4/5
    MESH_PRESET_KRONOS_DIR = 3, // 868.000 MHz, BW 125 kHz, SF9,  CR 4/7 (Direct ASCII mode)
};

struct LoraMessage {
    char text[72];
    float rssi;
    float snr;
    uint32_t epoch;     // UTC receive time
};

// Persisted as a raw NVS blob: do not reorder or resize fields.
struct LoraPeerNode {
    uint32_t nodeId;
    char name[24];
    char shortName[8];
    double lat;
    double lon;
    float rssi;
    float snr;
    uint32_t epoch;     // last heard (UTC)
    uint8_t chanHash;
    uint8_t batPct;
    bool hasPos;
    bool hasName;
    bool isSos;
    bool isMeshtastic;
};

struct BleThreatInfo {
    char name[24];
    char mac[18];
    char typeTag[16];   // "FINDMY", "TILE", "SMARTTAG", "CHIPOLO", "BLE GERÄT"
    int8_t rssi;
    bool isTracker;
    uint32_t lastSeenMs;
};

#define LORA_SPECTRUM_BINS 12   // 863.0 .. 870.0 MHz

// SX1262 LoRa / Meshtastic. All radio access happens on the background task
// inside loop(); the send*() functions only enqueue. Nothing here ever
// switches the radio on by itself: every send*() returns false while the
// user has LoRa disabled.
class RadioLoRa {
public:
    static bool init(float freq = 869.525f);
    static void loop(unsigned long now);          // background task only

    static bool sendPacket(const String& msg);    // false if !isEnabled()
    static bool sendSosBeacon();                  // false if !isEnabled()
    static bool sendWaypoint(const char* name, double lat, double lon);
    static bool sendPositionBeacon();             // false unless a real fix (< 30 min) exists
    static bool sendNodeInfoDiscovery(uint32_t targetNode = 0xFFFFFFFFUL); // max 1 per 120 s
    static bool pollPacket(String& outMsg, float& outRssi, float& outSnr);
    static uint32_t getLastTxMs();                // millis() of last TX, 0 if none

    static bool isReady();
    static void setEnabled(bool en);
    static bool isEnabled();

    // Switch between Meshtastic EU_868 Presets and Direct mode
    static void setMeshtasticMode(bool mesh);
    static bool isMeshtasticMode();
    static void cyclePreset();
    static MeshPreset getPreset();
    static const char* getPresetLabel();

    static uint32_t getSentCount();
    static uint32_t getRecvCount();
    static float getLastRssi();
    static float getLastSnr();
    static String getLastReceivedMessage();
    static uint8_t getInbox(LoraMessage* out, uint8_t max);   // newest first

    // Real Meshtastic Hub & Peer Table
    static uint8_t getPeers(LoraPeerNode* out, uint8_t max);
    static uint8_t getPeerCount();
    // Best (strongest) peer position younger than 30 min. ageSec = seconds since received.
    static bool getMeshFallbackPosition(double& outLat, double& outLon, char* outSourceName = nullptr,
                                        size_t nameLen = 0, uint32_t* ageSec = nullptr);

    // 868 MHz Sub-GHz RF Spectrum Scanner (863.0 .. 870.0 MHz, LORA_SPECTRUM_BINS bins).
    // Only sweeps while active; the UI must deactivate it when the page is hidden.
    static void setSpectrumScanActive(bool active);
    static bool isSpectrumScanActive();
    static void getSpectrumBins(int8_t* outDbm, int numBins, float& outPeakFreqMhz, int8_t& outPeakDbm);

    // BLE Anti-Tracker Threat Radar (async NimBLE scan, only while BLE is enabled)
    static void triggerThreatScan();
    static bool isThreatScanRunning();
    static uint8_t getThreats(BleThreatInfo* out, uint8_t maxOut);
};
