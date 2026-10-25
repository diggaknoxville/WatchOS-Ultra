#pragma once
#include "Config.h"

enum NavMode : uint8_t {
    NAV_MODE_CAR  = 0,   // Auto (Autobahn / Bundesstraße preferred)
    NAV_MODE_PED  = 1,   // Fußgänger / Rad (no motorway, direct paths)
    NAV_MODE_MESH = 2,   // LoRa / Meshtastic Peer Tracking
};

enum PosSource : uint8_t {
    POS_SRC_GPS_LIVE  = 0,  // Live GNSS 3D Fix
    POS_SRC_LORA_MESH = 1,  // Assisted position from nearest LoRa/Meshtastic node
    POS_SRC_SAVED_FIX = 2,  // Last stored GNSS fix
    POS_SRC_DEFAULT   = 3,  // Default Stuttgart center
};

struct NavPlace {
    char name[36];
    double lat;
    double lon;
    uint32_t pop;
    uint8_t rank;        // 0 = LoRa/Meshtastic Peer, 1 = City, 2 = Town, 3 = Suburb, 4 = Village, 5 = Saved POI
};

struct NavRoutePoint {
    int32_t lat_e5;
    int32_t lon_e5;
};

struct NavStatus {
    bool sdReady;
    bool sdWritable;
    uint32_t sdTotalMB;
    uint32_t sdUsedMB;
    bool mapLoaded;
    bool ultraLoaded;
    uint32_t mapPlaces;
    uint32_t mapRouteNodes;
    uint32_t mapPois;

    PosSource posSource;
    char posSourceTag[24];
    double curLat;
    double curLon;
    float headingDeg;

    bool activeRoute;
    NavMode mode;
    char destName[36];
    double destLat;
    double destLon;
    float totalDistKm;
    float remDistKm;
    uint32_t nextTurnDistM;
    uint16_t etaMinutes;
    float bearingDeg;
    char nextTurnCmd[24];
    const char* nextTurnIcon;

    bool gpxRecording;
    uint32_t gpxPoints;
    uint16_t savedWaypoints;
};

class NavEngine {
public:
    static void init();
    static void loop(unsigned long now);
    static bool reloadMap();

    static const NavStatus& status();

    // Mode & Route Control
    static void setMode(NavMode m);
    static NavMode getMode();
    static bool startNavigation(const char* name, double destLat, double destLon);
    static void stopNavigation();
    static bool isNavigating();

    // Place, Street, POI & City Search (searches germany.knav + saved POIs + live LoRa/Meshtastic peers)
    static int searchPlaces(const char* query, NavPlace* out, int maxOut);

    // GPX Track Logger & 1-Click Waypoint POI marking (stored on SD card + NVS)
    static void setTrackRecording(bool rec);
    static bool isTrackRecording();
    static bool markWaypoint(const char* label = nullptr);

    // Renders the Google-Maps-style vector map + buildings + forests + farmland + POIs + route into RGB565 buffer
    static void renderMap(uint16_t* buf, int w, int h, double viewLat, double viewLon, int zoom);

    // High-speed SD file upload helper (for streaming germany.knav over Serial or WiFi)
    static bool beginSdUpload(const char* path, uint32_t totalSize);
    static bool writeSdUploadChunk(const uint8_t* data, size_t len);
    static bool endSdUpload();
};
