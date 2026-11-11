#pragma once
// Pure position / step math, shared by GpsManager and MotionHaptic and tested
// on the host (test/test_geo). No Arduino dependencies.
#include <math.h>
#include <stdint.h>

namespace geo {

static const double kPi = 3.14159265358979323846;
static const double kEarthM = 6371000.0;

inline double haversineM(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180.0;
    double dLat = (lat2 - lat1) * r, dLon = (lon2 - lon1) * r;
    double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * r) * cos(lat2 * r) * sin(dLon / 2) * sin(dLon / 2);
    return 2.0 * kEarthM * asin(sqrt(a));
}

// Move distM metres along bearingDeg (0 = north, clockwise). Flat-earth step,
// exact enough for the few hundred metres of dead reckoning.
inline void advance(double& lat, double& lon, double bearingDeg, double distM) {
    const double r = kPi / 180.0;
    lat += distM * cos(bearingDeg * r) / kEarthM / r;
    lon += distM * sin(bearingDeg * r) / (kEarthM * cos(lat * r)) / r;
}

// Circular low-pass for angles in degrees; returns the new value in 0..360.
inline float smoothAngle(float cur, float target, float alpha) {
    float d = fmodf(target - cur + 540.0f, 360.0f) - 180.0f;
    return fmodf(cur + alpha * d + 360.0f, 360.0f);
}

// Smooths GNSS fixes: heavy averaging while standing (kills the jitter of a
// stationary receiver), light while moving, and single outliers are dropped.
struct PosFilter {
    double lat = 0, lon = 0;
    uint32_t lastMs = 0;
    bool valid = false;
    uint8_t rejects = 0;

    // Returns false when the fix was rejected as a jump (state unchanged).
    bool update(double newLat, double newLon, float hdop, float speedKmh, uint32_t nowMs) {
        float dt = valid ? (nowMs - lastMs) / 1000.0f : 0.0f;
        if (!valid || dt > 10.0f) {                 // first fix or long gap: take it as is
            lat = newLat; lon = newLon; lastMs = nowMs; valid = true; rejects = 0;
            return true;
        }
        double d = haversineM(lat, lon, newLat, newLon);
        // farthest plausible move: 3x the reported speed (at least walking pace) plus the fix uncertainty
        double maxMove = fmax(speedKmh / 3.6, 2.0) * fmax(dt, 1.0f) * 3.0 + hdop * 10.0;
        if (d > maxMove && rejects < 3) {           // three in a row: the receiver really is somewhere else
            rejects++;
            return false;
        }
        float alpha = rejects >= 3 ? 1.0f : speedKmh < 1.5f ? 0.15f : speedKmh < 6.0f ? 0.5f : 0.8f;
        lat += alpha * (newLat - lat);
        lon += alpha * (newLon - lon);
        lastMs = nowMs;
        rejects = 0;
        return true;
    }
    void reset() { valid = false; rejects = 0; }
};

// Ascent / descent from a noisy altitude: only changes beyond the threshold count.
struct ClimbTracker {
    float ref = 0, ascent = 0, descent = 0;
    bool valid = false;
    void update(float altM, float thresholdM = 5.0f) {
        if (!valid) { ref = altM; valid = true; return; }
        float d = altM - ref;
        if (d >= thresholdM) { ascent += d; ref = altM; }
        else if (d <= -thresholdM) { descent -= d; ref = altM; }
    }
};

// Stride length from one GPS segment. Returns 0 when the segment is no usable
// walking / running sample (too short, or an implausible stride).
inline float strideFromSegment(float distM, uint32_t steps, bool running) {
    if (steps < 60 || distM < 30.0f) return 0.0f;
    float s = distM / (float)steps;
    float lo = running ? 0.6f : 0.4f, hi = running ? 1.8f : 1.1f;
    return (s >= lo && s <= hi) ? s : 0.0f;
}

}  // namespace geo
