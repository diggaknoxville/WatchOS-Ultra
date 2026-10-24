#include "SettingsStorage.h"
#include <nvs_flash.h>

Preferences SettingsStorage::prefs;
SystemSettings SettingsStorage::settings;

void SettingsStorage::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    prefs.begin("vortex_sys", false);
    if (!prefs.isKey("init_flag")) {
        resetDefaults();
    } else {
        settings.watchfaceIndex = prefs.getUChar("wf_idx", FACE_MODULAR);
        settings.accentColor = prefs.getUShort("accent", THEME_CYBER_CYAN);
        settings.brightness = prefs.getUChar("bright", 220);
        settings.autoSleepSec = prefs.getUShort("sleep_s", 25);
        settings.wristWakeEnabled = prefs.getBool("wrist_wake", true);
        settings.hapticEnabled = prefs.getBool("haptic", true);
        settings.aodEnabled = prefs.getBool("aod", true);
        settings.soundEnabled = prefs.getBool("sound", true);
        settings.wifiApEnabled = prefs.getBool("wifi_ap", true);
        settings.loraBeaconEnabled = prefs.getBool("lora_bcn", false);
        settings.timezoneOffsetSec = prefs.getInt("tz_sec", 7200); // Default UTC+2
        settings.dailyStepGoal = prefs.getUInt("step_goal", 10000);
        String tzp = prefs.getString("tz_posix", "CET-1CEST,M3.5.0,M10.5.0/3");
        strncpy(settings.tzPosix, tzp.c_str(), sizeof(settings.tzPosix) - 1);
        settings.tzPosix[sizeof(settings.tzPosix) - 1] = '\0';
        String title = prefs.getString("title", "KRONOS ULTRA");
        strncpy(settings.customTitle, title.c_str(), sizeof(settings.customTitle) - 1);
        settings.customTitle[sizeof(settings.customTitle) - 1] = '\0';
    }
}

SystemSettings& SettingsStorage::get() {
    return settings;
}

void SettingsStorage::save() {
    prefs.putUChar("wf_idx", settings.watchfaceIndex);
    prefs.putUShort("accent", settings.accentColor);
    prefs.putUChar("bright", settings.brightness);
    prefs.putUShort("sleep_s", settings.autoSleepSec);
    prefs.putBool("wrist_wake", settings.wristWakeEnabled);
    prefs.putBool("haptic", settings.hapticEnabled);
    prefs.putBool("aod", settings.aodEnabled);
    prefs.putBool("sound", settings.soundEnabled);
    prefs.putBool("wifi_ap", settings.wifiApEnabled);
    prefs.putBool("lora_bcn", settings.loraBeaconEnabled);
    prefs.putInt("tz_sec", settings.timezoneOffsetSec);
    prefs.putUInt("step_goal", settings.dailyStepGoal);
    prefs.putString("title", settings.customTitle);
    prefs.putString("tz_posix", settings.tzPosix);
}

void SettingsStorage::resetDefaults() {
    settings.watchfaceIndex = FACE_MODULAR;
    settings.accentColor = THEME_CYBER_CYAN;
    settings.brightness = 220;
    settings.autoSleepSec = 25;
    settings.wristWakeEnabled = true;
    settings.hapticEnabled = true;
    settings.aodEnabled = true;
    settings.soundEnabled = true;
    settings.wifiApEnabled = true;
    settings.loraBeaconEnabled = false;
    settings.timezoneOffsetSec = 7200;
    settings.dailyStepGoal = 10000;
    strncpy(settings.customTitle, "KRONOS ULTRA", sizeof(settings.customTitle));
    strncpy(settings.tzPosix, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(settings.tzPosix));

    prefs.putBool("init_flag", true);
    save();
}

uint32_t SettingsStorage::getLifetimeSteps() {
    return prefs.getUInt("life_steps", 0);
}

void SettingsStorage::addSteps(uint32_t steps) {
    uint32_t cur = getLifetimeSteps();
    prefs.putUInt("life_steps", cur + steps);
}

void SettingsStorage::saveLastKnownTime(time_t epoch) {
    if (epoch > 1700000000) {
        prefs.putUInt("last_epoch", (uint32_t)epoch);
    }
}

time_t SettingsStorage::getLastKnownTime() {
    return (time_t)prefs.getUInt("last_epoch", 1792753000);
}

