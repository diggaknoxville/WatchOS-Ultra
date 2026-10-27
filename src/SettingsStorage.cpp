#include "SettingsStorage.h"
#include "RtcManager.h"      // MIN_VALID_EPOCH
#include <nvs_flash.h>

Preferences SettingsStorage::prefs;
SystemSettings SettingsStorage::settings;
static SystemSettings sCommitted;
static volatile bool sDirty = false;
static volatile unsigned long sDirtyAt = 0;
static bool sNvsOk = false;
static SemaphoreHandle_t sFlushMutex = nullptr;

static const char* DEFAULT_TZ_POSIX = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char* DEFAULT_TITLE = "WATCHOS ULTRA";

static bool lockPrefs(uint32_t timeoutMs) {
    if (!sFlushMutex) return true;
    return xSemaphoreTake(sFlushMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
static void unlockPrefs() {
    if (sFlushMutex) xSemaphoreGive(sFlushMutex);
}

void SettingsStorage::init() {
    if (!sFlushMutex) sFlushMutex = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        Serial.printf("[NVS] nvs_flash_init failed: %d\n", (int)err);
    }
    sNvsOk = prefs.begin("vortex_sys", false);
    if (!sNvsOk) {
        Serial.println("[NVS] settings namespace unavailable - running on defaults, nothing persisted");
        resetDefaults();
        return;
    }
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
        // The hotspot is a session (CompanionServer switches it off when idle):
        // it is never restored at boot, whatever was stored before.
        settings.wifiApEnabled = false;
        settings.bleEnabled = prefs.getBool("ble_en", true);
        settings.gpsEnabled = prefs.getBool("gps_en", true);
        settings.loraEnabled = prefs.getBool("lora_en", true);
        settings.loraBeaconEnabled = prefs.getBool("lora_bcn", false);
        settings.powerSaver = prefs.getBool("pwr_saver", false);
        settings.saverRestore = prefs.getUChar("saver_rs", 0);
        settings.timezoneOffsetSec = prefs.getInt("tz_sec", 7200); // Default UTC+2
        settings.dailyStepGoal = prefs.getUInt("step_goal", 10000);
        String tzp = prefs.getString("tz_posix", DEFAULT_TZ_POSIX);
        strncpy(settings.tzPosix, tzp.c_str(), sizeof(settings.tzPosix) - 1);
        settings.tzPosix[sizeof(settings.tzPosix) - 1] = '\0';
        String title = prefs.getString("title", DEFAULT_TITLE);
        strncpy(settings.customTitle, title.c_str(), sizeof(settings.customTitle) - 1);
        settings.customTitle[sizeof(settings.customTitle) - 1] = '\0';
        if (settings.watchfaceIndex >= FACE_COUNT) settings.watchfaceIndex = FACE_MODULAR;
        if (settings.brightness < 10) settings.brightness = 10;
        sCommitted = settings;
    }
}

bool SettingsStorage::isNvsReady() { return sNvsOk; }

SystemSettings& SettingsStorage::get() {
    return settings;
}

void SettingsStorage::save() {
    // Non-blocking on UI thread: schedule background diff-flush on Core 0
    sDirty = true;
    sDirtyAt = millis();
}

void SettingsStorage::flushNow() {
    sDirty = false;
    if (!sNvsOk) return;
    if (!lockPrefs(200)) { sDirty = true; return; }      // retried by loop()
    if (settings.watchfaceIndex != sCommitted.watchfaceIndex) prefs.putUChar("wf_idx", settings.watchfaceIndex);
    if (settings.accentColor != sCommitted.accentColor)       prefs.putUShort("accent", settings.accentColor);
    if (settings.brightness != sCommitted.brightness)         prefs.putUChar("bright", settings.brightness);
    if (settings.autoSleepSec != sCommitted.autoSleepSec)     prefs.putUShort("sleep_s", settings.autoSleepSec);
    if (settings.wristWakeEnabled != sCommitted.wristWakeEnabled) prefs.putBool("wrist_wake", settings.wristWakeEnabled);
    if (settings.hapticEnabled != sCommitted.hapticEnabled)   prefs.putBool("haptic", settings.hapticEnabled);
    if (settings.aodEnabled != sCommitted.aodEnabled)         prefs.putBool("aod", settings.aodEnabled);
    if (settings.soundEnabled != sCommitted.soundEnabled)     prefs.putBool("sound", settings.soundEnabled);
    if (settings.bleEnabled != sCommitted.bleEnabled)         prefs.putBool("ble_en", settings.bleEnabled);
    if (settings.gpsEnabled != sCommitted.gpsEnabled)         prefs.putBool("gps_en", settings.gpsEnabled);
    if (settings.loraEnabled != sCommitted.loraEnabled)       prefs.putBool("lora_en", settings.loraEnabled);
    if (settings.loraBeaconEnabled != sCommitted.loraBeaconEnabled) prefs.putBool("lora_bcn", settings.loraBeaconEnabled);
    if (settings.powerSaver != sCommitted.powerSaver)         prefs.putBool("pwr_saver", settings.powerSaver);
    if (settings.saverRestore != sCommitted.saverRestore)     prefs.putUChar("saver_rs", settings.saverRestore);
    if (settings.timezoneOffsetSec != sCommitted.timezoneOffsetSec) prefs.putInt("tz_sec", settings.timezoneOffsetSec);
    if (settings.dailyStepGoal != sCommitted.dailyStepGoal)   prefs.putUInt("step_goal", settings.dailyStepGoal);
    if (strcmp(settings.customTitle, sCommitted.customTitle) != 0) prefs.putString("title", settings.customTitle);
    if (strcmp(settings.tzPosix, sCommitted.tzPosix) != 0)    prefs.putString("tz_posix", settings.tzPosix);
    sCommitted = settings;
    unlockPrefs();
}

void SettingsStorage::loop(unsigned long now) {
    if (sDirty && (now - sDirtyAt >= 400)) {
        flushNow();
    }
}

void SettingsStorage::resetDefaults() {
    memset(&sCommitted, 0xFF, sizeof(sCommitted));
    settings.watchfaceIndex = FACE_MODULAR;
    settings.accentColor = THEME_CYBER_CYAN;
    settings.brightness = 220;
    settings.autoSleepSec = 25;
    settings.wristWakeEnabled = true;
    settings.hapticEnabled = true;
    settings.aodEnabled = true;
    settings.soundEnabled = true;
    settings.wifiApEnabled = false;
    settings.bleEnabled = true;
    settings.gpsEnabled = true;
    settings.loraEnabled = true;
    settings.loraBeaconEnabled = false;
    settings.powerSaver = false;
    settings.saverRestore = 0;
    settings.timezoneOffsetSec = 7200;
    settings.dailyStepGoal = 10000;
    strncpy(settings.customTitle, DEFAULT_TITLE, sizeof(settings.customTitle) - 1);
    settings.customTitle[sizeof(settings.customTitle) - 1] = '\0';
    strncpy(settings.tzPosix, DEFAULT_TZ_POSIX, sizeof(settings.tzPosix) - 1);
    settings.tzPosix[sizeof(settings.tzPosix) - 1] = '\0';

    if (!sNvsOk) { sCommitted = settings; return; }
    if (lockPrefs(200)) {
        prefs.putBool("init_flag", true);
        unlockPrefs();
    }
    flushNow();
}

uint32_t SettingsStorage::getLifetimeSteps() {
    if (!sNvsOk) return 0;
    uint32_t v = 0;
    if (lockPrefs(100)) { v = prefs.getUInt("life_steps", 0); unlockPrefs(); }
    return v;
}

void SettingsStorage::addSteps(uint32_t steps) {
    if (!sNvsOk || steps == 0) return;
    if (lockPrefs(100)) {
        uint32_t cur = prefs.getUInt("life_steps", 0);
        prefs.putUInt("life_steps", cur + steps);
        unlockPrefs();
    }
}

void SettingsStorage::saveLastKnownTime(time_t epoch) {
    if (!sNvsOk || epoch < RtcManager::MIN_VALID_EPOCH) return;
    if (lockPrefs(100)) {
        // only rewrite if it moved by at least a minute (either direction): NVS wear
        uint32_t cur = prefs.getUInt("last_epoch", 0);
        uint32_t e = (uint32_t)epoch;
        uint32_t diff = (e > cur) ? (e - cur) : (cur - e);
        if (diff >= 60) prefs.putUInt("last_epoch", e);
        unlockPrefs();
    }
}

time_t SettingsStorage::getLastKnownTime() {
    if (!sNvsOk) return 0;
    uint32_t v = 0;
    if (lockPrefs(100)) { v = prefs.getUInt("last_epoch", 0); unlockPrefs(); }
    return (time_t)v;
}
