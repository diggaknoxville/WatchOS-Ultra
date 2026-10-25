#pragma once
#include "Config.h"
#include <Preferences.h>

// User settings in NVS namespace "vortex_sys" (name kept for stored data).
// get() returns the live struct; save() marks it dirty and the background
// task diff-flushes it 400 ms later. flushNow() is mutex-guarded: the UI and
// the background task may both call it.
class SettingsStorage {
public:
    static void init();
    static SystemSettings& get();
    static void save();
    static void flushNow();
    static void loop(unsigned long now);
    static void resetDefaults();
    static bool isNvsReady();

    // Specific persistent counters
    static uint32_t getLifetimeSteps();
    static void addSteps(uint32_t steps);
    static void saveLastKnownTime(time_t epoch);   // ignored below RtcManager::MIN_VALID_EPOCH
    static time_t getLastKnownTime();              // 0 if never saved

private:
    static Preferences prefs;
    static SystemSettings settings;
};
