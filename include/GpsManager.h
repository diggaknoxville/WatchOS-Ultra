#pragma once
#include "Config.h"
#include <time.h>

struct GpsSatInfo {
    uint8_t prn;
    uint8_t elevation;  // 0..90 deg
    uint16_t azimuth;   // 0..359 deg
    uint8_t snr;        // 0..99 dB-Hz
    bool used;
};

// u-blox GNSS on the T-Watch Ultra (instance.gps, Serial1 @ 38400).
//
// Power policy (GNSS draws ~25 mA):
//   * acquisition window at boot and every 6 h -> disciplines the RTC from
//     satellite UTC, then powers down again
//   * continuous while the POSITION screen is open (setLive)
// Last fix and last GNSS time sync are persisted in NVS.
class GpsManager {
public:
    static void init();
    static void loop(unsigned long now);     // background task
    static void setLive(bool on);            // thread-safe, from UI task
    static void requestFix();                // open an acquisition window now (thread-safe)
    static void setEnabled(bool en);         // master power enable/disable
    static bool isEnabled();

    static bool isOnline();                  // module detected at boot
    static bool isPowered();
    static bool hasFix();
    static uint8_t satellites();
    static double latitude();
    static double longitude();
    static float altitudeM();
    static float speedKmh();
    static float courseDeg();
    static float hdop();
    static bool hasLastFix();                // persisted position available
    static time_t lastFixEpoch();
    static time_t lastTimeSyncEpoch();
    static uint32_t searchSeconds();         // time since power-on without fix

    // GNSS Skyplot satellite table (parsed from $GPGSV / $GLGSV / $GAGSV / $GBGSV)
    static uint8_t getSatellitesInView(GpsSatInfo* out, uint8_t maxOut);
};
