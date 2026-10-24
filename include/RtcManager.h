#pragma once
#include "Config.h"
#include <LilyGoLib.h>   // T-Watch Ultra RTC is instance.rtc (PCF85063)
#include <time.h>

// Time model:
//  * The battery-backed PCF85063 RTC is the master clock and stores UTC.
//  * The ESP32 system clock also runs in UTC and is re-seeded from the RTC.
//  * Local time is derived through a POSIX TZ rule (automatic DST), persisted
//    in NVS. The RTC is only written on an external sync (phone, NTP, serial).
class RtcManager {
public:
    static bool init();
    static void updateSystemTimeFromRtc();
    static void syncFromEpoch(time_t utcEpoch, int32_t tzOffsetSec = 0, const char* ianaZone = nullptr);
    static void syncFromNtp(const char* ntpServer = "pool.ntp.org");
    static void getLocalTimeInfo(struct tm* info);
    static time_t getEpoch();
    static bool isRtcOnline();
    static bool isTimeValid();
    static void periodicMaintenance();

    static time_t epochFromUtc(int y, int mo, int d, int h, int mi, int s);
    static time_t lastSyncEpoch();                      // last external sync (UTC)
    static const char* lastSyncSource();                // "PHONE", "GPS", "NTP", "USB"
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
