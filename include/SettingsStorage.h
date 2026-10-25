#pragma once
#include "Config.h"
#include <Preferences.h>

class SettingsStorage {
public:
    static void init();
    static SystemSettings& get();
    static void save();
    static void flushNow();
    static void loop(unsigned long now);
    static void resetDefaults();

    // Specific persistent counters
    static uint32_t getLifetimeSteps();
    static void addSteps(uint32_t steps);
    static void saveLastKnownTime(time_t epoch);
    static time_t getLastKnownTime();

private:
    static Preferences prefs;
    static SystemSettings settings;
};

