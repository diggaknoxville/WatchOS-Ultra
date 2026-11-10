#pragma once
// Astronomy helpers (sunrise / sunset, moon phase). Pure logic without
// Arduino dependencies so it can be unit-tested on the host (pio test -e native).
#include <math.h>
#include <time.h>

namespace astro {

static const double kPi = 3.14159265358979323846;

// Local UTC offset in seconds (mktime interprets a gmtime() breakdown as local time)
inline long localUtcOffsetSec() {
    time_t t = time(nullptr);
    struct tm g;
#ifdef _WIN32
    g = *gmtime(&t);
#else
    gmtime_r(&t, &g);
#endif
    g.tm_isdst = -1;
    time_t asLocal = mktime(&g);
    return (long)(t - asLocal);
}

// NOAA sunrise equation, zenith 90.833 deg (refraction). Returns false in polar
// day / night. Minutes are local time of the given day of year.
inline bool sunTimes(double lat, double lon, int yday, long tzSec, int& riseMin, int& setMin) {
    const double rad = kPi / 180.0;
    double gamma = 2.0 * kPi / 365.0 * (yday + 0.5);
    double eqTime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma)
                              - 0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));
    double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma) - 0.006758 * cos(2 * gamma)
                  + 0.000907 * sin(2 * gamma) - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);
    double cosHa = (cos(90.833 * rad) / (cos(lat * rad) * cos(decl))) - tan(lat * rad) * tan(decl);
    if (cosHa < -1.0 || cosHa > 1.0) return false;
    double ha = acos(cosHa) / rad;                                  // degrees
    double riseUtc = 720.0 - 4.0 * (lon + ha) - eqTime;             // minutes UTC
    double setUtc  = 720.0 - 4.0 * (lon - ha) - eqTime;
    riseMin = (int)lround(riseUtc + tzSec / 60.0);
    setMin  = (int)lround(setUtc + tzSec / 60.0);
    while (riseMin < 0) riseMin += 1440;
    while (setMin < 0) setMin += 1440;
    riseMin %= 1440;
    setMin %= 1440;
    return true;
}

// Synodic month from the new moon of 2000-01-06 18:14 UTC. Returns the phase
// age in days (0 = new moon) and the illuminated fraction 0..1.
inline void moonPhase(time_t now, double& ageDays, double& illum) {
    const double synodic = 29.530588853;
    double days = ((double)now - 947182440.0) / 86400.0;
    ageDays = fmod(days, synodic);
    if (ageDays < 0) ageDays += synodic;
    illum = (1.0 - cos(2.0 * kPi * ageDays / synodic)) / 2.0;
}

} // namespace astro
