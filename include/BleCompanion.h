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

class BleCompanion {
public:
    static bool init();
    static void loop(unsigned long now);
    static bool isConnected();
    static void setEnabled(bool en);
    static bool isEnabled();

    // Trigger update notification to client
    static void notifyStatus();

    // Media & YouTube Control commands to phone over BLE
    static void sendMediaCommand(const char* cmd);

    // Live Media Playback State
    static const MediaState& getMediaState();
    static void setMediaState(bool isPlaying, const char* title, const char* artist, uint8_t volumePct = 50);

    // Synced Google Calendar Events from Phone (BLE or WiFi)
    static uint8_t getUpcomingEvents(CalendarEvent* out, uint8_t maxOut);
    static bool hasCalendarEvents();
    static void triggerCalendarSync();
    static void updateCalendarFromJson(const char* json);
};
