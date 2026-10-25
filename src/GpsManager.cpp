#include "GpsManager.h"
#include "RtcManager.h"
#include "SettingsStorage.h"
#include "ui/Navigator.h"
#include <LilyGoLib.h>
#include <Preferences.h>

static const uint32_t WINDOW_MS   = 10UL * 60UL * 1000UL;   // acquisition window
static const uint32_t PERIOD_MS   = 6UL * 3600UL * 1000UL;  // re-sync interval
static const uint32_t PERSIST_MS  = 5UL * 60UL * 1000UL;    // fix persistence cadence

static bool sOnline = false;
static bool sPowered = false;
static volatile bool sLive = false;
static unsigned long sPowerOnAt = 0;
static unsigned long sNextWindowAt = 0;
static unsigned long sWindowEndAt = 0;
static unsigned long sLastPersist = 0;
static bool sSyncedThisWindow = false;

static double sLat = 0, sLon = 0;
static float sAlt = 0;
static time_t sFixEpoch = 0;
static time_t sTimeSyncEpoch = 0;
static bool sHaveLast = false;

static void persist() {
    Preferences p;
    if (!p.begin("kronos_gps", false)) return;
    p.putDouble("lat", sLat);
    p.putDouble("lon", sLon);
    p.putFloat("alt", sAlt);
    p.putUInt("fix_t", (uint32_t)sFixEpoch);
    p.putUInt("sync_t", (uint32_t)sTimeSyncEpoch);
    p.putUChar("v", 2);          // v2 = only verified (>=4 sats) fixes are stored
    p.end();
}

static void power(bool on) {
    if (on == sPowered) return;
    instance.powerControl(POWER_GPS, on);
    sPowered = on;
    if (on) sPowerOnAt = millis();
    Serial.printf("[GPS] power %s\n", on ? "ON" : "OFF");
}

void GpsManager::init() {
    sOnline = (instance.getDeviceProbe() & HW_GPS_ONLINE) != 0;
    sPowered = sOnline;                      // LilyGoLib leaves it powered after probe
    sPowerOnAt = millis();
    Preferences p;
    if (p.begin("kronos_gps", false)) {
        if (p.getUChar("v", 0) != 2) p.clear();   // drop unverified legacy data
        sLat = p.getDouble("lat", 0);
        sLon = p.getDouble("lon", 0);
        sAlt = p.getFloat("alt", 0);
        sFixEpoch = (time_t)p.getUInt("fix_t", 0);
        sTimeSyncEpoch = (time_t)p.getUInt("sync_t", 0);
        sHaveLast = sFixEpoch > 0;
        p.end();
    }
    if (sOnline) {
        if (SettingsStorage::get().gpsEnabled) {
            sWindowEndAt = millis() + WINDOW_MS;   // boot acquisition window
            sNextWindowAt = millis() + PERIOD_MS;
        } else {
            power(false);
        }
    }
    Serial.printf("[GPS] u-blox %s (enabled=%d), last fix %s\n", sOnline ? "online" : "NOT FOUND",
                  (int)SettingsStorage::get().gpsEnabled, sHaveLast ? "stored" : "none");
}

static volatile bool sFixRequest = false;
void GpsManager::setLive(bool on) { sLive = on; }
void GpsManager::requestFix() {
    if (!SettingsStorage::get().gpsEnabled) {
        SettingsStorage::get().gpsEnabled = true;
        SettingsStorage::save();
    }
    sFixRequest = true;
}

void GpsManager::setEnabled(bool en) {
    SettingsStorage::get().gpsEnabled = en;
    SettingsStorage::save();
    if (en) {
        sFixRequest = true;
    } else {
        sFixRequest = false;
    }
    nav::markDirty(nav::PG_POSITION);
    nav::markDirty(nav::PG_CONTROL);
    nav::markDirty(nav::PG_SYSTEM);
}

bool GpsManager::isEnabled() {
    return SettingsStorage::get().gpsEnabled;
}

void GpsManager::loop(unsigned long now) {
    if (!sOnline) return;
    if (!SettingsStorage::get().gpsEnabled) {
        power(false);
        return;
    }

    // --- power scheduling ---
    if (sFixRequest) {
        sFixRequest = false;
        sWindowEndAt = now + WINDOW_MS;
        sSyncedThisWindow = false;
    }
    if ((long)(now - sNextWindowAt) >= 0) {
        sNextWindowAt = now + PERIOD_MS;
        sWindowEndAt = now + WINDOW_MS;
        sSyncedThisWindow = false;
    }
    bool inWindow = (long)(sWindowEndAt - now) > 0 && !sSyncedThisWindow;
    power(sLive || inWindow);
    if (!sPowered) return;

    // --- NMEA ingest ---
    while (Serial1.available()) instance.gps.encode(Serial1.read());

    TinyGPSPlus& g = instance.gps;
    static unsigned long lastLog = 0;
    if (now - lastLog > 10000) {
        lastLog = now;
        Serial.printf("[GPS] sats=%d hdop=%.1f loc=%d age=%lu chars=%lu sentences=%lu\n",
                      g.satellites.isValid() ? (int)g.satellites.value() : -1,
                      g.hdop.isValid() ? g.hdop.hdop() : -1.0, (int)g.location.isValid(),
                      (unsigned long)g.location.age(), (unsigned long)g.charsProcessed(),
                      (unsigned long)g.sentencesWithFix());
    }
    // Only trust a genuine multi-satellite solution; the module can replay a
    // cached time/position from its backup memory right after power-up.
    bool trusted = g.location.isValid() && g.location.age() < 2000 &&
                   g.satellites.isValid() && g.satellites.value() >= 4 &&
                   g.hdop.isValid() && g.hdop.hdop() > 0 && g.hdop.hdop() < 5.0;
    if (!trusted) return;

    sLat = g.location.lat();
    sLon = g.location.lng();
    if (g.altitude.isValid()) sAlt = (float)g.altitude.meters();
    sHaveLast = true;

    // --- discipline the RTC from satellite UTC ---
    if (g.date.isValid() && g.time.isValid() && g.time.age() < 1000 && g.date.year() >= 2025) {
        time_t utc = RtcManager::epochFromUtc(g.date.year(), g.date.month(), g.date.day(),
                                              g.time.hour(), g.time.minute(), g.time.second());
        sFixEpoch = utc;
        if (!sSyncedThisWindow) {
            long drift = (long)(utc - time(NULL));
            if (labs(drift) >= 1) RtcManager::syncFromEpoch(utc);
            RtcManager::noteSyncSource("GPS");
            sTimeSyncEpoch = utc;
            sSyncedThisWindow = true;
            persist();
            sLastPersist = now;
            Serial.printf("[GPS] time disciplined (drift %lds), %d sats, hdop %.1f\n", drift,
                          (int)g.satellites.value(), g.hdop.hdop());
        }
    }
    if (now - sLastPersist > PERSIST_MS) { sLastPersist = now; persist(); }
}

bool GpsManager::isOnline()  { return sOnline; }
bool GpsManager::isPowered() { return sPowered; }
bool GpsManager::hasFix() {
    TinyGPSPlus& g = instance.gps;
    return sPowered && g.location.isValid() && g.location.age() < 3000 &&
           g.satellites.isValid() && g.satellites.value() >= 4;
}
uint8_t GpsManager::satellites() {
    return (sPowered && instance.gps.satellites.isValid()) ? instance.gps.satellites.value() : 0;
}
double GpsManager::latitude()  { return sLat; }
double GpsManager::longitude() { return sLon; }
float GpsManager::altitudeM()  { return sAlt; }
float GpsManager::speedKmh()   { return hasFix() && instance.gps.speed.isValid() ? instance.gps.speed.kmph() : 0; }
float GpsManager::courseDeg()  { return hasFix() && instance.gps.course.isValid() ? instance.gps.course.deg() : 0; }
float GpsManager::hdop()       { return instance.gps.hdop.isValid() ? instance.gps.hdop.hdop() : 99.9f; }
bool GpsManager::hasLastFix()  { return sHaveLast; }
time_t GpsManager::lastFixEpoch() { return sFixEpoch; }
time_t GpsManager::lastTimeSyncEpoch() { return sTimeSyncEpoch; }
uint32_t GpsManager::searchSeconds() { return sPowered ? (millis() - sPowerOnAt) / 1000 : 0; }
