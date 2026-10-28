#pragma once
#include "Config.h"
#include <Arduino.h>

struct CalendarEvent {
    uint32_t startEpoch;
    uint32_t endEpoch;
    char title[44];
    char loc[24];
    bool allDay;
};

struct MediaState {
    bool isPlaying;
    char title[40];
    char artist[28];
    uint8_t volumePct;
};

// BLE companion link (NimBLE 2.x peripheral).
//
// GATT layout (service a100):
//   a101 data    WRITE | WRITE_NR, encrypted + authenticated (bonded, passkey)
//   a102 status  READ_ENC | NOTIFY, binary {0x01, topPct}
//   a103 command NOTIFY, ASCII "CMD:*" strings for the phone app
// Pairing uses a random passkey (getPasskey) shown on the watch.
// All payload parsing happens on the background task inside loop(); the
// NimBLE host task only copies the raw bytes into a small queue.
class BleCompanion {
public:
    static void preload();                   // setup(): cached calendar from NVS (before the UI is built)
    static bool init();
    static void loop(unsigned long now);     // background task
    static bool isStackRunning();            // NimBLE host + controller initialised (no light sleep)
    static bool isConnected();
    static void setEnabled(bool en);
    static bool isEnabled();
    static uint32_t getPasskey();            // 6-digit pairing passkey for the UI
    static void resetBonds();                // delete all saved smartphone pairings from NVS

    // Trigger update notification to client
    static void notifyStatus();

    // Media & YouTube Control commands to phone over BLE (queued, sent on loop)
    static void sendMediaCommand(const char* cmd);

    // Live Media Playback State (thread-safe copies)
    static MediaState getMediaState();
    static void setMediaState(bool isPlaying, const char* title, const char* artist, uint8_t volumePct = 50);
    static void setMediaPlaying(bool playing);   // local optimistic state only
    static void setMediaVolume(uint8_t pct);

    // Synced Google Calendar Events from Phone (BLE or WiFi)
    static uint8_t getUpcomingEvents(CalendarEvent* out, uint8_t maxOut);
    static bool hasCalendarEvents();
    static bool triggerCalendarSync();       // rate-limited (60 s); false if limited / not connected
    static uint32_t lastCalendarSyncMs();    // 0 if never
    static void updateCalendarFromJson(const char* json);
};
