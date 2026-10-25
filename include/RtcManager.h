#pragma once
#include "Config.h"
#include <LilyGoLib.h>   // T-Watch Ultra RTC is instance.rtc (PCF85063)
#include <time.h>

// Time model:
//  * The battery-backed PCF85063 RTC is the master clock and stores UTC.
//  * The ESP32 system clock also runs in UTC and is re-seeded from the RTC.
//  * Local time is derived through a POSIX TZ rule (automatic DST), persisted
//    in NVS. The RTC is only written on an external sync (phone, GPS, serial).
//  * Time is "valid" only if the RTC read was plausible at boot or an external
//    sync happened since boot. There is no fake default date.
class RtcManager {
public:
    // Earliest epoch accepted as a real time (2025-01-01T00:00:00Z). Shared by
    // SettingsStorage (last_epoch) and all sync sources.
    static constexpr time_t MIN_VALID_EPOCH = 1735689600;

    // Hardware probe mask from instance.begin(); must be set before init().
    // Without it (0) the mask is taken from instance.getDeviceProbe().
    static void setHardwareMask(uint32_t mask);

    static bool init();
    static void updateSystemTimeFromRtc();
    static void syncFromEpoch(time_t utcEpoch, int32_t tzOffsetSec = 0, const char* ianaZone = nullptr);
    static void getLocalTimeInfo(struct tm* info);
    static time_t getEpoch();
    static bool isRtcOnline();
    static bool isTimeValid();
    static void periodicMaintenance();                  // hourly: RTC re-seed (drift > 1 s) + NVS last_epoch

    static time_t epochFromUtc(int y, int mo, int d, int h, int mi, int s);
    static time_t lastSyncEpoch();                      // last external sync (UTC), 0 if never
    static const char* lastSyncSource();                // "PHONE", "GPS", "USB", "RTC"
    static void noteSyncSource(const char* src);

    static void applyTimezone();                       // from settings.tzPosix
    static void setTimezone(const char* ianaZone, int32_t fallbackOffsetSec);
    static const char* posixForIana(const char* ianaZone);
    static const char* timezoneName();                 // IANA name, e.g. "Europe/Berlin"
    static long utcOffsetSec();                        // current local - UTC (incl. DST)

private:
    static bool rtcReady;
    static unsigned long lastMaintenance;
    static time_t utcFromRtc(const RTC_DateTime& dt);
    static void writeRtcUtc(time_t utcEpoch);
};
