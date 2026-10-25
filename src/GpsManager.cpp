#include "GpsManager.h"
#include "RtcManager.h"
#include "PowerManager.h"
#include "SettingsStorage.h"
#include "ui/Navigator.h"
#include <LilyGoLib.h>
#include <Preferences.h>
#include <math.h>

static const uint32_t WINDOW_MS   = 10UL * 60UL * 1000UL;   // acquisition window
static const uint32_t PERIOD_MS   = 6UL * 3600UL * 1000UL;  // re-sync interval
static const uint32_t PERSIST_MS  = 5UL * 60UL * 1000UL;    // fix persistence cadence
static const uint32_t SAT_AGE_MS  = 30UL * 1000UL;          // drop satellites not seen for 30 s
static const uint32_t GSA_HOLD_MS = 5UL * 1000UL;           // "used" valid for 5 s after a GSA
static const uint32_t FIX_HOLD_MS = 10UL * 1000UL;          // hasFix() grace after power-down

static bool sOnline = false;
static bool sPowered = false;
static volatile bool sLive = false;
static unsigned long sPowerOnAt = 0;
static unsigned long sNextWindowAt = 0;
static unsigned long sWindowEndAt = 0;
static unsigned long sLastPersist = 0;
static bool sSyncedThisWindow = false;

// No default position: 0,0 and sHaveLast == false until a verified fix exists.
static double sLat = 0.0, sLon = 0.0;
static float sAlt = 0.0f;
static time_t sFixEpoch = 0;
static time_t sTimeSyncEpoch = 0;
static bool sHaveLast = false;
static unsigned long sLastTrustedMs = 0;   // millis() of the last trusted live fix

static constexpr uint8_t MAX_SATS_VIEW = 20;
static GpsSatInfo sSatsView[MAX_SATS_VIEW];
static unsigned long sSatSeenMs[MAX_SATS_VIEW];
static unsigned long sSatUsedUntilMs[MAX_SATS_VIEW];
static uint8_t sNumSatsView = 0;
static portMUX_TYPE sSatMux = portMUX_INITIALIZER_UNLOCKED;
static char sNmeaLine[100];
static uint8_t sNmeaPos = 0;

static int tokenize(char* buf, char** fields, int maxFields) {
    int nf = 0;
    char* p = buf;
    while (p && nf < maxFields) {
        fields[nf++] = p;
        char* comma = strchr(p, ',');
        if (!comma) break;
        *comma = 0;
        p = comma + 1;
    }
    return nf;
}

static int findSatSlot(uint8_t prn) {
    for (int k = 0; k < sNumSatsView; k++) {
        if (sSatsView[k].prn == prn) return k;
    }
    return -1;
}

static bool isTalker(const char* line, const char* type) {
    // "$GPGSV," / "$GLGSV," / "$GAGSV," / "$GBGSV," / "$GNGSV," / "$GQGSV,"
    return line[0] == '$' && strncmp(line + 3, type, 3) == 0 && line[6] == ',';
}

static void parseGsvLine(const char* line, unsigned long now) {
    // $xxGSV,numMsgs,msgNum,numSats,prn1,el1,az1,snr1,prn2,el2,az2,snr2,...*CS
    char buf[100];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* star = strchr(buf, '*');
    if (star) *star = 0;

    char* fields[24] = {nullptr};
    int nf = tokenize(buf, fields, 24);
    if (nf < 4) return;

    portENTER_CRITICAL(&sSatMux);
    for (int i = 4; i + 3 < nf; i += 4) {
        int prn = atoi(fields[i]);
        int el  = atoi(fields[i + 1]);
        int az  = atoi(fields[i + 2]);
        int snr = atoi(fields[i + 3]);
        if (prn <= 0 || prn > 255) continue;

        int slot = findSatSlot((uint8_t)prn);
        if (slot < 0) {
            if (sNumSatsView < MAX_SATS_VIEW) {
                slot = sNumSatsView++;
            } else {
                // replace the entry not seen for the longest time
                slot = 0;
                for (int k = 1; k < MAX_SATS_VIEW; k++) {
                    if ((long)(sSatSeenMs[k] - sSatSeenMs[slot]) < 0) slot = k;
                }
            }
            sSatsView[slot].prn = (uint8_t)prn;
            sSatUsedUntilMs[slot] = 0;
        }
        sSatsView[slot].elevation = (uint8_t)constrain(el, 0, 90);
        sSatsView[slot].azimuth = (uint16_t)constrain(az, 0, 359);
        sSatsView[slot].snr = (uint8_t)constrain(snr, 0, 99);
        sSatSeenMs[slot] = now;
    }
    portEXIT_CRITICAL(&sSatMux);
}

static void parseGsaLine(const char* line, unsigned long now) {
    // $xxGSA,mode,fixType,prn1..prn12,PDOP,HDOP,VDOP[,systemId]*CS
    // Tokenized 0-based: [0]="$xxGSA", [1]=mode, [2]=fixType, [3..14]=active PRNs
    char buf[100];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* star = strchr(buf, '*');
    if (star) *star = 0;

    char* fields[24] = {nullptr};
    int nf = tokenize(buf, fields, 24);
    if (nf < 4) return;

    portENTER_CRITICAL(&sSatMux);
    for (int i = 3; i <= 14 && i < nf; i++) {
        int prn = atoi(fields[i]);
        if (prn <= 0 || prn > 255) continue;
        int slot = findSatSlot((uint8_t)prn);
        if (slot >= 0) sSatUsedUntilMs[slot] = now + GSA_HOLD_MS;
    }
    portEXIT_CRITICAL(&sSatMux);
}

static void ageOutSatellites(unsigned long now) {
    portENTER_CRITICAL(&sSatMux);
    for (int k = 0; k < sNumSatsView;) {
        if (now - sSatSeenMs[k] > SAT_AGE_MS) {
            int last = sNumSatsView - 1;
            if (k != last) {
                sSatsView[k] = sSatsView[last];
                sSatSeenMs[k] = sSatSeenMs[last];
                sSatUsedUntilMs[k] = sSatUsedUntilMs[last];
            }
            sNumSatsView--;
        } else {
            k++;
        }
    }
    portEXIT_CRITICAL(&sSatMux);
}

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
    if (PowerManager::lockI2C(30)) {
        instance.powerControl(POWER_GPS, on);
        PowerManager::unlockI2C();
    } else {
        Serial.println("[GPS] I2C busy, power change deferred");
        return;   // retry on the next loop() pass
    }
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
        sLat = p.getDouble("lat", 0.0);
        sLon = p.getDouble("lon", 0.0);
        sAlt = p.getFloat("alt", 0.0f);
        sFixEpoch = (time_t)p.getUInt("fix_t", 0);
        sTimeSyncEpoch = (time_t)p.getUInt("sync_t", 0);
        sHaveLast = sFixEpoch > 0 && !(sLat == 0.0 && sLon == 0.0);
        if (!sHaveLast) { sLat = 0.0; sLon = 0.0; sAlt = 0.0f; }
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
    sFixRequest = en;
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
    while (Serial1.available()) {
        char ch = (char)Serial1.read();
        instance.gps.encode(ch);
        if (ch == '$') {
            sNmeaPos = 0;
            sNmeaLine[sNmeaPos++] = ch;
        } else if (ch == '\r' || ch == '\n') {
            if (sNmeaPos > 6) {
                sNmeaLine[sNmeaPos] = 0;
                if (isTalker(sNmeaLine, "GSV"))      parseGsvLine(sNmeaLine, now);
                else if (isTalker(sNmeaLine, "GSA")) parseGsaLine(sNmeaLine, now);
            }
            sNmeaPos = 0;
        } else if (sNmeaPos > 0 && sNmeaPos < sizeof(sNmeaLine) - 1) {
            sNmeaLine[sNmeaPos++] = ch;
        }
    }

    static unsigned long lastAgeOut = 0;
    if (now - lastAgeOut > 5000) {
        lastAgeOut = now;
        ageOutSatellites(now);
    }

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

    double lat = g.location.lat();
    double lon = g.location.lng();
    if (lat == 0.0 && lon == 0.0) return;   // null island = no real solution
    sLat = lat;
    sLon = lon;
    if (g.altitude.isValid()) sAlt = (float)g.altitude.meters();
    sHaveLast = true;
    sLastTrustedMs = now;

    // --- discipline the RTC from satellite UTC ---
    if (g.date.isValid() && g.time.isValid() && g.time.age() <= 1500 && g.date.year() >= 2025) {
        time_t utc = RtcManager::epochFromUtc(g.date.year(), g.date.month(), g.date.day(),
                                              g.time.hour(), g.time.minute(), g.time.second());
        utc += (time_t)(g.time.age() / 1000);   // compensate the time since the sentence arrived
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
    } else if (sFixEpoch == 0 && RtcManager::isTimeValid()) {
        sFixEpoch = time(NULL);   // position without usable GNSS time: stamp with RTC
    }
    if (now - sLastPersist > PERSIST_MS) { sLastPersist = now; persist(); }
}

bool GpsManager::isOnline()  { return sOnline; }
bool GpsManager::isPowered() { return sPowered; }
bool GpsManager::hasFix() {
    if (sPowered) {
        TinyGPSPlus& g = instance.gps;
        return g.location.isValid() && g.location.age() < 3000 &&
               g.satellites.isValid() && g.satellites.value() >= 4;
    }
    // Keep a fresh fix valid for a few seconds after the module powered down so
    // the navigation engine does not instantly fall back to the stored fix.
    return sLastTrustedMs != 0 && (millis() - sLastTrustedMs) < FIX_HOLD_MS;
}
uint8_t GpsManager::satellites() {
    return (sPowered && instance.gps.satellites.isValid()) ? instance.gps.satellites.value() : 0;
}
double GpsManager::latitude()  { return sHaveLast ? sLat : 0.0; }
double GpsManager::longitude() { return sHaveLast ? sLon : 0.0; }
float GpsManager::altitudeM()  { return sHaveLast ? sAlt : 0.0f; }
float GpsManager::speedKmh()   { return (sPowered && hasFix() && instance.gps.speed.isValid()) ? instance.gps.speed.kmph() : 0; }
float GpsManager::courseDeg()  { return (sPowered && hasFix() && instance.gps.course.isValid()) ? instance.gps.course.deg() : 0; }
float GpsManager::hdop()       { return (sPowered && instance.gps.hdop.isValid()) ? instance.gps.hdop.hdop() : 99.9f; }
bool GpsManager::hasLastFix()  { return sHaveLast; }
time_t GpsManager::lastFixEpoch() { return sFixEpoch; }
time_t GpsManager::lastTimeSyncEpoch() { return sTimeSyncEpoch; }
uint32_t GpsManager::searchSeconds() { return sPowered ? (millis() - sPowerOnAt) / 1000 : 0; }

uint8_t GpsManager::getSatellitesInView(GpsSatInfo* out, uint8_t maxOut) {
    if (!out || maxOut == 0) return 0;
    unsigned long now = millis();
    portENTER_CRITICAL(&sSatMux);
    uint8_t n = (sNumSatsView < maxOut) ? sNumSatsView : maxOut;
    for (uint8_t i = 0; i < n; i++) {
        out[i] = sSatsView[i];
        out[i].used = sPowered && (long)(sSatUsedUntilMs[i] - now) > 0;
    }
    portEXIT_CRITICAL(&sSatMux);
    return n;
}
