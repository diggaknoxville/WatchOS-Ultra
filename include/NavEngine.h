#pragma once
#include "Config.h"

// =============================================================================
//  NavEngine - offline navigation (map rendering, place search, A* routing,
//  GPX logging) on top of the KNAV map container (see include/MapFormat.h).
//
//  Threading model
//    * Every public function is thread-safe: all shared state is guarded by
//      one recursive mutex. Callers from the UI task and the background task
//      may interleave freely.
//    * loop() must be called from the background task (main.cpp). It performs
//      the 1 Hz position / guidance tick, GPX logging, the incremental map
//      table loading (places, POIs, routing graph -> PSRAM), the deferred
//      boot work (flash -> SD mirror copy, GPX repair) and the asynchronous
//      route computation.
//    * renderMap() holds the mutex while it draws (a few ms, up to ~100 ms
//      on the first draw of a region when cells have to be read from SD).
//      During that time loop() on the background task blocks. Call it at
//      most once per UI frame.
//    * status() returns a consistent snapshot (copy).
//
//  Position model (NavStatus::posSource, strict priority)
//    POS_SRC_GPS_LIVE   live GNSS fix
//    POS_SRC_SAVED_FIX  own last stored fix (any age; posAgeSec tells how old)
//    POS_SRC_LORA_MESH  position of a mesh peer received < 30 min ago
//    POS_SRC_DEFAULT    nothing known: curLat/curLon = 0, posSourceTag
//                       = "KEINE POSITION". renderMap() centres on the map
//                       bounding box (or Germany) when asked to render (0,0)
//                       and draws a "KEINE POSITION" hint. GPX logging only
//                       happens with POS_SRC_GPS_LIVE.
// =============================================================================

enum NavMode : uint8_t {
    NAV_MODE_CAR  = 0,   // Auto: motorway / primary preferred, only car-routable edges
    NAV_MODE_PED  = 1,   // Fuss / Rad: only pedestrian-routable edges, big roads penalised
    NAV_MODE_MESH = 2,   // LoRa / Meshtastic peer tracking: straight line to the peer
};

enum PosSource : uint8_t {
    POS_SRC_GPS_LIVE  = 0,  // live GNSS fix
    POS_SRC_LORA_MESH = 1,  // position of a nearby mesh peer (< 30 min old)
    POS_SRC_SAVED_FIX = 2,  // own last stored GNSS fix
    POS_SRC_DEFAULT   = 3,  // no position at all
};

struct NavPlace {
    char name[36];
    double lat;
    double lon;
    uint32_t pop;
    uint8_t rank;        // 0 = LoRa/Meshtastic peer, 1 = city, 2 = town, 3 = suburb, 4 = village, 5 = saved waypoint
};

struct NavRoutePoint {
    int32_t lat_e5;
    int32_t lon_e5;
};

struct NavStatus {
    bool sdReady;
    bool sdWritable;          // only true after a successful write test
    uint32_t sdTotalMB;
    uint32_t sdUsedMB;        // filled by the background task shortly after boot
    bool mapLoaded;           // a valid KNAV header was found (SD or flash)
    bool ultraLoaded;         // the map carries the Stuttgart ultra tier
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
    const char* nextTurnIcon; // points to a static ICO_* literal, never null after init()

    bool gpxRecording;
    uint32_t gpxPoints;
    uint16_t savedWaypoints;

    // ---- added in the 1.1.0 overhaul ----
    bool routingBusy;         // a route request is queued or being computed on the background task
    bool routeStraightLine;   // no road route -> straight line shown, nextTurnCmd = "LUFTLINIE", no turns
    bool routeFailed;         // last request could not be routed (no map / no graph / no position)
    bool graphReady;          // routing graph resident in PSRAM
    bool placesReady;         // place table resident in PSRAM (searchPlaces works without SD)
    uint8_t mapLoadPct;       // 0..100 progress of the background table loading
    uint32_t posAgeSec;       // age of a saved / mesh position, 0 for live GPS or unknown
};

class NavEngine {
public:
    static void init();                      // UI task (from the position page builder); fast, no bulk SD copying
    static void loop(unsigned long now);     // background task only
    static bool reloadMap();                 // re-detect SD + map; returns true if a map header was accepted

    static NavStatus status();               // consistent snapshot (copy)

    // Mode & route control
    static void setMode(NavMode m);
    static NavMode getMode();
    // ASYNC: returns true if the request was accepted. The route is computed on
    // the background task; while it runs status().routingBusy is true and a
    // provisional straight line is shown. Without a known position the request
    // is kept and routed automatically as soon as a position becomes available.
    static bool startNavigation(const char* name, double destLat, double destLon);
    static void stopNavigation();
    static bool isNavigating();
    static bool isRoutingBusy();

    // Place / mesh peer / waypoint search. Searches the in-PSRAM place table
    // (case-insensitive prefix first, then substring), never touches the SD
    // card. Special queries: "@mesh" (peers only), "@poi" (saved waypoints).
    // An empty query lists peers, waypoints and the places nearest to the
    // current position (or the most important ones when no position is known).
    static int searchPlaces(const char* query, NavPlace* out, int maxOut);

    // GPX track logger (SD /tracks/YYYY-MM-DD.gpx, only with a live GPS fix)
    // and one-tap waypoints (NVS + SD /waypoints.csv).
    static void setTrackRecording(bool rec);
    static bool isTrackRecording();
    static bool markWaypoint(const char* label = nullptr);   // false without a known position

    // Renders the vector map (landuse, buildings, roads, POIs, labels, route,
    // GPX trail, peers, own position, scale bar) into an RGB565 buffer.
    // Holds the engine mutex for the duration (see header comment).
    // viewLat/viewLon == (0,0) means "no position": the map centre is used.
    static void renderMap(uint16_t* buf, int w, int h, double viewLat, double viewLon, int zoom);

    // Centre of the loaded map's bounding box, or Germany's centre when no map
    // (or a V2 map without bbox) is loaded. Returns true if a map is loaded.
    static bool getMapCenter(double& lat, double& lon);

    // Streaming upload of a new map file to the SD card (serial console / WiFi
    // portal). beginSdUpload() unloads the current map (mapLoaded = false,
    // caches and tables freed); endSdUpload() closes the file and reloads.
    static bool beginSdUpload(const char* path, uint32_t totalSize);
    static bool writeSdUploadChunk(const uint8_t* data, size_t len);
    static bool endSdUpload();
};
