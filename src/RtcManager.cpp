#include "RtcManager.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include <Wire.h>
#include <sys/time.h>
#include <Preferences.h>

bool RtcManager::rtcReady = false;
unsigned long RtcManager::lastMaintenance = 0;

static uint32_t sHwMask = 0;
static bool sRtcValidAtBoot = false;                // first RTC read at boot was plausible
static bool sSyncedSinceBoot = false;               // syncFromEpoch() succeeded since boot
static char sTzName[40] = "Europe/Berlin";          // IANA name of the active zone

// IANA zone -> POSIX TZ rule (automatic DST). Unknown zones fall back to a
// fixed offset taken from the phone at sync time.
struct TzMap { const char* iana; const char* posix; };
static const TzMap TZ_TABLE[] = {
    {"Europe/Berlin",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris",     "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels",  "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen","CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London",    "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin",    "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Europe/Lisbon",    "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Helsinki",  "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Athens",    "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul",  "<+03>-3"},
    {"Europe/Moscow",    "MSK-3"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",  "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",   "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"},
    {"Asia/Tokyo",       "JST-9"},
    {"Asia/Shanghai",    "CST-8"},
    {"Asia/Dubai",       "<+04>-4"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"UTC",              "UTC0"},
};

const char* RtcManager::posixForIana(const char* ianaZone) {
    if (!ianaZone || !*ianaZone) return nullptr;
    for (const TzMap& m : TZ_TABLE) {
        if (strcmp(m.iana, ianaZone) == 0) return m.posix;
    }
    return nullptr;
}

void RtcManager::applyTimezone() {
    const char* tz = SettingsStorage::get().tzPosix;
    if (!tz[0]) tz = "CET-1CEST,M3.5.0,M10.5.0/3";
    setenv("TZ", tz, 1);
    tzset();
}

void RtcManager::setTimezone(const char* ianaZone, int32_t fallbackOffsetSec) {
    SystemSettings& s = SettingsStorage::get();
    const char* posix = posixForIana(ianaZone);
    char fixedRule[24];
    if (!posix) {
        // POSIX sign is inverted: UTC+2 -> "<+02>-2"
        int32_t h = fallbackOffsetSec / 3600;
        int32_t m = abs(fallbackOffsetSec % 3600) / 60;
        if (m) snprintf(fixedRule, sizeof(fixedRule), "<%+03ld%02ld>%ld:%02ld",
                        (long)h, (long)m, (long)-h, (long)m);
        else   snprintf(fixedRule, sizeof(fixedRule), "<%+03ld>%ld", (long)h, (long)-h);
        posix = fixedRule;
    }
    if (ianaZone && *ianaZone) {
        strncpy(sTzName, ianaZone, sizeof(sTzName) - 1);
        sTzName[sizeof(sTzName) - 1] = 0;
        Preferences p;
        if (p.begin("kronos_time", false)) { p.putString("tz_name", sTzName); p.end(); }
    }
    strncpy(s.tzPosix, posix, sizeof(s.tzPosix) - 1);
    s.tzPosix[sizeof(s.tzPosix) - 1] = '\0';
    s.timezoneOffsetSec = fallbackOffsetSec;
    SettingsStorage::save();
    applyTimezone();
    Serial.printf("[RTC] Timezone %s -> %s\n", ianaZone ? ianaZone : "(offset)", s.tzPosix);
}

// UTC calendar fields -> epoch without any TZ influence (days-from-civil).
time_t RtcManager::epochFromUtc(int y, int m, int d, int h, int mi, int s) {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    // 64-bit arithmetic: time_t is 64 bit on ESP-IDF 5, no 2038 overflow
    return (time_t)(days * 86400LL + h * 3600LL + mi * 60LL + s);
}

time_t RtcManager::utcFromRtc(const RTC_DateTime& dt) {
    return epochFromUtc(dt.getYear(), dt.getMonth(), dt.getDay(),
                        dt.getHour(), dt.getMinute(), dt.getSecond());
}

static time_t sLastSync = 0;
static char sLastSrc[8] = "RTC";
time_t RtcManager::lastSyncEpoch() { return sLastSync; }
const char* RtcManager::lastSyncSource() { return sLastSrc; }
static void persistSync() {
    Preferences p;
    if (p.begin("kronos_time", false)) {
        p.putUInt("last_sync", (uint32_t)sLastSync);
        p.putString("sync_src", sLastSrc);
        p.end();
    }
}

void RtcManager::noteSyncSource(const char* src) {
    strncpy(sLastSrc, src, sizeof(sLastSrc) - 1);
    sLastSrc[sizeof(sLastSrc) - 1] = 0;
    persistSync();
}

void RtcManager::writeRtcUtc(time_t utcEpoch) {
    if (!rtcReady) return;
    struct tm g;
    gmtime_r(&utcEpoch, &g);
    RTC_DateTime dt(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                    g.tm_hour, g.tm_min, g.tm_sec, g.tm_wday);
    if (PowerManager::lockI2C(30)) {
        instance.rtc.setDateTime(dt);
        PowerManager::unlockI2C();
    } else {
        Serial.println("[RTC] write skipped (I2C busy)");
    }
}

void RtcManager::setHardwareMask(uint32_t mask) { sHwMask = mask; }

bool RtcManager::init() {
    {
        Preferences p;
        if (p.begin("kronos_time", false)) {
            sLastSync = (time_t)p.getUInt("last_sync", 0);
            String src = p.getString("sync_src", "RTC");
            String tzn = p.getString("tz_name", "Europe/Berlin");
            strncpy(sTzName, tzn.c_str(), sizeof(sTzName) - 1);
            strncpy(sLastSrc, src.c_str(), sizeof(sLastSrc) - 1);
            p.end();
        }
    }
    const uint32_t mask = sHwMask ? sHwMask : instance.getDeviceProbe();
    rtcReady = (mask & HW_RTC_ONLINE) != 0;
    Serial.printf("[RTC] PCF85063 (UTC master) via instance.rtc: %s\n", rtcReady ? "online" : "NOT FOUND");
    applyTimezone();
    if (rtcReady) {
        updateSystemTimeFromRtc();
    } else {
        // No RTC: the only possible source is an external sync later on.
        // Seed the system clock with the last persisted time so logs and
        // relative calculations are sane, but keep isTimeValid() false.
        time_t last = SettingsStorage::getLastKnownTime();
        if (last >= MIN_VALID_EPOCH) {
            struct timeval tv = { .tv_sec = last, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
    }
    return rtcReady;
}

void RtcManager::updateSystemTimeFromRtc() {
    if (!rtcReady) return;
    // A transient I2C lock timeout must not be mistaken for an invalid RTC:
    // return without touching the system clock, the RTC or NVS.
    if (!PowerManager::lockI2C(30)) {
        Serial.println("[RTC] read skipped (I2C busy)");
        return;
    }
    RTC_DateTime dt = instance.rtc.getDateTime();
    PowerManager::unlockI2C();

    const time_t ep = utcFromRtc(dt);
    const bool plausible = dt.getYear() >= 2025 && dt.getYear() <= 2099 && ep >= MIN_VALID_EPOCH;
    if (plausible) {
        const time_t sys = time(NULL);
        const long long drift = (long long)ep - (long long)sys;
        if (!sRtcValidAtBoot || drift > 1 || drift < -1) {
            struct timeval tv = { .tv_sec = ep, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
        sRtcValidAtBoot = true;
        SettingsStorage::saveLastKnownTime(ep);
        struct tm lt; localtime_r(&ep, &lt);
        Serial.printf("[RTC] UTC %04d-%02d-%02d %02d:%02d:%02d -> local %02d:%02d (%s) drift %+llds\n",
                      dt.getYear(), dt.getMonth(), dt.getDay(), dt.getHour(), dt.getMinute(),
                      dt.getSecond(), lt.tm_hour, lt.tm_min, SettingsStorage::get().tzPosix, drift);
        return;
    }

    // The read succeeded but the RTC content is implausible (lost backup power
    // / never set). Restore the last persisted UTC time if we have one so the
    // RTC keeps counting from a sane value; the time stays "invalid" until a
    // real sync source (phone, GPS, USB) arrives.
    const time_t last = SettingsStorage::getLastKnownTime();
    if (last >= MIN_VALID_EPOCH) {
        struct timeval tv = { .tv_sec = last, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        writeRtcUtc(last);
        Serial.println("[RTC] RTC invalid -> restored last known time from NVS (time not trusted until synced)");
    } else {
        Serial.println("[RTC] RTC invalid and no last known time -> waiting for sync");
    }
}

void RtcManager::syncFromEpoch(time_t utcEpoch, int32_t tzOffsetSec, const char* ianaZone) {
    if (utcEpoch < MIN_VALID_EPOCH) return;
    if (ianaZone && *ianaZone) setTimezone(ianaZone, tzOffsetSec);

    struct timeval tv = { .tv_sec = utcEpoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    writeRtcUtc(utcEpoch);
    SettingsStorage::saveLastKnownTime(utcEpoch);
    sLastSync = utcEpoch;
    sSyncedSinceBoot = true;
    persistSync();

    struct tm lt; localtime_r(&utcEpoch, &lt);
    Serial.printf("[RTC] Synced: local %04d-%02d-%02d %02d:%02d:%02d\n",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
}

void RtcManager::getLocalTimeInfo(struct tm* info) {
    time_t now = time(NULL);
    localtime_r(&now, info);
}

time_t RtcManager::getEpoch() { return time(NULL); }
const char* RtcManager::timezoneName() { return sTzName; }
long RtcManager::utcOffsetSec() {
    time_t now = time(NULL);
    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
    return (long)(epochFromUtc(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec) -
                  epochFromUtc(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday, gt.tm_hour, gt.tm_min, gt.tm_sec));
}
bool RtcManager::isRtcOnline() { return rtcReady; }

bool RtcManager::isTimeValid() {
    // Valid only with a real source: plausible RTC content at boot (battery
    // backed, previously synced) or an external sync since boot. A restored
    // NVS timestamp alone never makes the time valid.
    if (!sRtcValidAtBoot && !sSyncedSinceBoot) return false;
    return time(NULL) >= MIN_VALID_EPOCH;
}

void RtcManager::periodicMaintenance() {
    unsigned long now = millis();
    if (now - lastMaintenance > 3600000UL) {   // hourly
        lastMaintenance = now;
        if (rtcReady) {
            // RTC crystal is the reference: re-seed the system clock only if it
            // drifted by more than 1 s, and persist a recovery timestamp.
            updateSystemTimeFromRtc();
        } else if (isTimeValid()) {
            SettingsStorage::saveLastKnownTime(time(NULL));
        }
    }
}
