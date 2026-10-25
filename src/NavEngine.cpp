#include "NavEngine.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "RtcManager.h"
#include "DisplayEngine.h"
#include "ui/Icons.h"
#include "ui/Navigator.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <esp_partition.h>
#include <math.h>
#include <vector>
#include <queue>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr double DE_MIN_LAT = 47.20;
static constexpr double DE_MAX_LAT = 55.10;
static constexpr double DE_MIN_LON = 5.80;
static constexpr double DE_MAX_LON = 15.10;
static constexpr int COARSE_NX = 32;
static constexpr int COARSE_NY = 32;
static constexpr int FINE_NX   = 256;
static constexpr int FINE_NY   = 256;

struct KnavHeader {
    char magic[8];
    uint32_t numPlaces;
    uint32_t placesOffset;
    uint32_t numRouteNodes;
    uint32_t numRouteEdges;
    uint32_t routeNodesOffset;
    uint32_t routeEdgesOffset;
    uint32_t coarseDirOffset;
    uint32_t fineDirOffset;
};

struct DiskPlace {
    int32_t lat_e5;
    int32_t lon_e5;
    uint32_t pop;
    uint8_t rank;
    char name[35];
};

struct DiskRouteNode {
    int32_t lat_e5;
    int32_t lon_e5;
    uint32_t firstEdge;
    uint16_t edgeCount;
    uint16_t pad;
};

struct DiskRouteEdge {
    uint32_t toNode;
    uint16_t dist_hm;
    uint8_t flags;
    uint8_t pad;
};

struct CellDirEntry {
    uint32_t offset;
    uint32_t length;
};

// Built-in fallback cities so search and city labels work even if SD card is empty
static const NavPlace BUILTIN_CITIES[] = {
    {"Berlin",            52.5200, 13.4050, 3769962, 1},
    {"Hamburg",           53.5511,  9.9937, 1862565, 1},
    {"München",           48.1351, 11.5820, 1505036, 1},
    {"Köln",              50.9375,  6.9603, 1069200, 1},
    {"Frankfurt am Main", 50.1109,  8.6821,  773068, 1},
    {"Stuttgart",         48.7758,  9.1829,  632865, 1},
    {"Düsseldorf",        51.2277,  6.7735,  653253, 1},
    {"Leipzig",           51.3397, 12.3731,  628718, 1},
    {"Dortmund",          51.5136,  7.4653,  600933, 1},
    {"Essen",             51.4556,  7.0116,  586608, 1},
    {"Bremen",            53.0793,  8.8017,  569396, 1},
    {"Dresden",           51.0504, 13.7373,  572240, 1},
    {"Hannover",          52.3759,  9.7320,  555372, 1},
    {"Nürnberg",          49.4521, 11.0767,  531159, 1},
    {"Duisburg",          51.4344,  6.7623,  502211, 1},
    {"Bochum",            51.4818,  7.2162,  365742, 1},
    {"Wuppertal",         51.2562,  7.1508,  358876, 1},
    {"Bielefeld",         52.0302,  8.5325,  338332, 1},
    {"Bonn",              50.7374,  7.0982,  336465, 1},
    {"Münster",           51.9607,  7.6261,  320946, 1},
    {"Mannheim",          49.4875,  8.4660,  315554, 1},
    {"Karlsruhe",         49.0069,  8.4037,  309964, 1},
    {"Augsburg",          48.3705, 10.8978,  301033, 1},
    {"Wiesbaden",         50.0782,  8.2398,  283083, 1},
    {"Kiel",              54.3233, 10.1228,  247717, 1},
    {"Magdeburg",         52.1205, 11.6276,  239364, 1},
    {"Freiburg",          47.9990,  7.8421,  236140, 1},
    {"Lübeck",            53.8655, 10.6866,  218095, 1},
    {"Erfurt",            50.9848, 11.0299,  214969, 1},
    {"Rostock",           54.0924, 12.0991,  209920, 1},
    {"Mainz",             49.9929,  8.2473,  220552, 1},
    {"Kassel",            51.3127,  9.4797,  204202, 1},
};
static constexpr int N_BUILTIN_CITIES = sizeof(BUILTIN_CITIES) / sizeof(BUILTIN_CITIES[0]);

// Approximate Germany border polygon (lat_e5, lon_e5) for overview rendering even before SD load
static const int32_t DE_BORDER_PTS[][2] = {
    {5490000,  835000}, {5480000,  945000}, {5440000, 1020000}, {5400000, 1085000},
    {5445000, 1260000}, {5410000, 1380000}, {5390000, 1425000}, {5330000, 1440000},
    {5255000, 1465000}, {5195000, 1470000}, {5115000, 1498000}, {5085000, 1460000},
    {5070000, 1380000}, {5040000, 1290000}, {5020000, 1215000}, {4955000, 1265000},
    {4885000, 1365000}, {4855000, 1350000}, {4810000, 1285000}, {4765000, 1305000},
    {4750000, 1270000}, {4770000, 1210000}, {4750000, 1105000}, {4740000, 1020000},
    {4755000,  960000}, {4765000,  865000}, {4755000,  760000}, {4820000,  775000},
    {4895000,  820000}, {4915000,  700000}, {4945000,  638000}, {5010000,  615000},
    {5078000,  605000}, {5105000,  590000}, {5185000,  615000}, {5245000,  670000},
    {5320000,  720000}, {5365000,  715000}, {5375000,  810000}, {5390000,  865000},
    {5440000,  860000}, {5490000,  835000}
};
static constexpr int N_BORDER_PTS = sizeof(DE_BORDER_PTS) / sizeof(DE_BORDER_PTS[0]);

static NavStatus sStat{};
static KnavHeader sHdr{};
static char sMapPath[48] = "/kronos/germany.knav";
static const esp_partition_t* sFlashPart = nullptr;
static bool sUseFlashPart = false;
static std::vector<NavRoutePoint> sRoute;
static std::vector<NavRoutePoint> sGpxTrail;
static unsigned long sLastGpxMs = 0;

struct MapReader {
    File sdFile;
    const esp_partition_t* flashPart = nullptr;
    bool valid = false;

    bool open() {
        if (sUseFlashPart && sFlashPart) {
            flashPart = sFlashPart;
            valid = true;
            return true;
        }
        if (sStat.sdReady) {
            sdFile = SD.open(sMapPath, FILE_READ);
            valid = (bool)sdFile;
            return valid;
        }
        return false;
    }
    bool readAt(uint32_t offset, void* dst, size_t len) {
        if (!valid) return false;
        if (flashPart) {
            return esp_partition_read(flashPart, offset, dst, len) == ESP_OK;
        }
        if (!sdFile.seek(offset)) return false;
        return sdFile.read((uint8_t*)dst, len) == len;
    }
    void close() {
        if (sdFile) sdFile.close();
        flashPart = nullptr;
        valid = false;
    }
};

// LRU Cell Cache in PSRAM so map panning doesn't re-read SD card on every frame
struct CachedCell {
    int32_t key; // (isFine << 20) | cellIdx, -1 = unused
    uint32_t lastUsed;
    uint8_t* data;
    uint32_t len;
};
static constexpr int CELL_CACHE_SLOTS = 24;
static CachedCell sCellCache[CELL_CACHE_SLOTS];
static uint32_t sCacheClock = 0;

static File sUploadFile;
static uint32_t sUploadRemaining = 0;

static void clearCellCache() {
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].data) {
            heap_caps_free(sCellCache[i].data);
            sCellCache[i].data = nullptr;
        }
        sCellCache[i].key = -1;
        sCellCache[i].len = 0;
    }
}

static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat * 0.5) * sin(dLat * 0.5) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dLon * 0.5) * sin(dLon * 0.5);
    return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static float calcBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    double phi1 = lat1 * M_PI / 180.0;
    double phi2 = lat2 * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double y = sin(dLon) * cos(phi2);
    double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
    double brng = atan2(y, x) * 180.0 / M_PI;
    return (float)fmod(brng + 360.0, 360.0);
}

static void updateCurrentPosition() {
    if (GpsManager::hasFix()) {
        sStat.posSource = POS_SRC_GPS_LIVE;
        snprintf(sStat.posSourceTag, sizeof(sStat.posSourceTag), "3D GPS (%u SAT)", GpsManager::satellites());
        sStat.curLat = GpsManager::latitude();
        sStat.curLon = GpsManager::longitude();
        sStat.headingDeg = GpsManager::courseDeg();
        return;
    }
    // Check LoRa / Meshtastic Peer Position Fallback!
    double mLat = 0, mLon = 0;
    char meshName[20] = {0};
    if (RadioLoRa::getMeshFallbackPosition(mLat, mLon, meshName, sizeof(meshName))) {
        sStat.posSource = POS_SRC_LORA_MESH;
        snprintf(sStat.posSourceTag, sizeof(sStat.posSourceTag), "MESH: %s", meshName);
        sStat.curLat = mLat;
        sStat.curLon = mLon;
        return;
    }
    if (GpsManager::hasLastFix()) {
        sStat.posSource = POS_SRC_SAVED_FIX;
        strncpy(sStat.posSourceTag, "GPS GESPEICHERT", sizeof(sStat.posSourceTag));
        sStat.curLat = GpsManager::latitude();
        sStat.curLon = GpsManager::longitude();
        return;
    }
    sStat.posSource = POS_SRC_DEFAULT;
    strncpy(sStat.posSourceTag, "DE ZENTRUM (DEMO)", sizeof(sStat.posSourceTag));
    if (fabs(sStat.curLat) < 1.0) {
        sStat.curLat = 52.5200; // Berlin default
        sStat.curLon = 13.4050;
    }
}

bool NavEngine::reloadMap() {
    clearCellCache();
    sStat.sdReady = (instance.getDeviceProbe() & HW_SD_ONLINE) != 0 && SD.cardType() != CARD_NONE;
    if (!sStat.sdReady) {
        // Try mounting SD card again in case it was inserted at runtime
        if (instance.installSD()) {
            sStat.sdReady = true;
        }
    }
    if (sStat.sdReady) {
        sStat.sdTotalMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
        sStat.sdUsedMB  = (uint32_t)(SD.usedBytes() / (1024ULL * 1024ULL));
    } else {
        sStat.sdTotalMB = sStat.sdUsedMB = 0;
    }

    sStat.mapLoaded = false;
    sStat.mapPlaces = N_BUILTIN_CITIES;
    sStat.mapRouteNodes = 0;
    sUseFlashPart = false;

    if (sStat.sdReady) {
        const char* candidates[] = {"/kronos/germany.knav", "/germany.knav", "/map/germany.knav"};
        for (const char* path : candidates) {
            if (SD.exists(path)) {
                File f = SD.open(path, FILE_READ);
                if (f && f.size() > sizeof(KnavHeader)) {
                    KnavHeader h{};
                    if (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && memcmp(h.magic, "KNAVDE01", 8) == 0) {
                        sHdr = h;
                        strncpy(sMapPath, path, sizeof(sMapPath) - 1);
                        sStat.mapLoaded = true;
                        sStat.mapPlaces = h.numPlaces;
                        sStat.mapRouteNodes = h.numRouteNodes;
                        Serial.printf("[NAV] Loaded SD %s: %u places, %u route nodes, %u edges\n",
                                      path, h.numPlaces, h.numRouteNodes, h.numRouteEdges);
                        f.close();
                        return true;
                    }
                }
                if (f) f.close();
            }
        }
    }

    // Check internal 9.875 MB QSPI Flash storage partition (0x610000) for KNAVDE01 database
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (part && part->size > sizeof(KnavHeader)) {
        KnavHeader h{};
        if (esp_partition_read(part, 0, &h, sizeof(h)) == ESP_OK && memcmp(h.magic, "KNAVDE01", 8) == 0) {
            sHdr = h;
            sFlashPart = part;
            sUseFlashPart = true;
            sStat.mapLoaded = true;
            sStat.mapPlaces = h.numPlaces;
            sStat.mapRouteNodes = h.numRouteNodes;
            Serial.printf("[NAV] Loaded Flash KNAVDE01 (@0x%lx): %u places, %u route nodes, %u edges\n",
                          (unsigned long)part->address, h.numPlaces, h.numRouteNodes, h.numRouteEdges);
            return true;
        }
    }

    Serial.println("[NAV] Using built-in DE base map");
    return false;
}

void NavEngine::init() {
    memset(&sStat, 0, sizeof(sStat));
    sStat.mode = NAV_MODE_CAR;
    sStat.curLat = 52.5200;
    sStat.curLon = 13.4050;
    sStat.nextTurnIcon = ICO_NAVIGATION;
    strncpy(sStat.nextTurnCmd, "BEREIT", sizeof(sStat.nextTurnCmd));
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        sCellCache[i].key = -1;
        sCellCache[i].data = nullptr;
        sCellCache[i].len = 0;
    }
    reloadMap();
    updateCurrentPosition();
}

static void updateTurnByTurn() {
    if (!sStat.activeRoute || sRoute.empty()) return;

    // If tracking a live LoRa/Meshtastic peer, update destination coordinates from peer table
    if (sStat.mode == NAV_MODE_MESH) {
        LoraPeerNode peers[16];
        uint8_t np = RadioLoRa::getPeers(peers, 16);
        for (uint8_t i = 0; i < np; i++) {
            if (peers[i].hasPos && strcmp(peers[i].name, sStat.destName) == 0) {
                sStat.destLat = peers[i].lat;
                sStat.destLon = peers[i].lon;
                break;
            }
        }
    }

    // Find closest route vertex to current position
    int bestIdx = 0;
    double bestDist = 1e9;
    for (size_t i = 0; i < sRoute.size(); i++) {
        double d = haversineKm(sStat.curLat, sStat.curLon, sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = (int)i;
        }
    }

    // Remaining distance along route from bestIdx to end
    double remKm = bestDist;
    for (size_t i = bestIdx; i + 1 < sRoute.size(); i++) {
        remKm += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                             sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
    }
    sStat.remDistKm = (float)remKm;
    sStat.bearingDeg = calcBearingDeg(sStat.curLat, sStat.curLon, sStat.destLat, sStat.destLon);

    if (remKm < 0.05) {
        strncpy(sStat.nextTurnCmd, "ZIEL ERREICHT", sizeof(sStat.nextTurnCmd));
        sStat.nextTurnIcon = ICO_FLAG;
        sStat.nextTurnDistM = 0;
        sStat.etaMinutes = 0;
        return;
    }

    // Scan forward along route to find next significant turn (> 28 deg change in bearing)
    double distToTurnKm = haversineKm(sStat.curLat, sStat.curLon,
                                      sRoute[min((int)sRoute.size() - 1, bestIdx + 1)].lat_e5 * 1e-5,
                                      sRoute[min((int)sRoute.size() - 1, bestIdx + 1)].lon_e5 * 1e-5);
    float turnDelta = 0.0f;
    bool foundTurn = false;
    for (size_t i = bestIdx + 1; i + 1 < sRoute.size(); i++) {
        float b1 = calcBearingDeg(sRoute[i - 1].lat_e5 * 1e-5, sRoute[i - 1].lon_e5 * 1e-5,
                                  sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5);
        float b2 = calcBearingDeg(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                                  sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
        float diff = fmodf(b2 - b1 + 540.0f, 360.0f) - 180.0f;
        if (fabsf(diff) >= 28.0f) {
            turnDelta = diff;
            foundTurn = true;
            break;
        }
        distToTurnKm += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                                    sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
    }

    sStat.nextTurnDistM = (uint32_t)(distToTurnKm * 1000.0);
    if (!foundTurn) {
        strncpy(sStat.nextTurnCmd, "DEM VERLAUF FOLGEN", sizeof(sStat.nextTurnCmd));
        sStat.nextTurnIcon = ICO_STRAIGHT;
    } else if (turnDelta > 55.0f) {
        strncpy(sStat.nextTurnCmd, "RECHTS ABBIEGEN", sizeof(sStat.nextTurnCmd));
        sStat.nextTurnIcon = ICO_TURN_RIGHT;
    } else if (turnDelta > 25.0f) {
        strncpy(sStat.nextTurnCmd, "HALBRECHTS HALTEN", sizeof(sStat.nextTurnCmd));
        sStat.nextTurnIcon = ICO_TURN_SLIGHT_RIGHT;
    } else if (turnDelta < -55.0f) {
        strncpy(sStat.nextTurnCmd, "LINKS ABBIEGEN", sizeof(sStat.nextTurnCmd));
        sStat.nextTurnIcon = ICO_TURN_LEFT;
    } else {
        strncpy(sStat.nextTurnCmd, "HALBLINKS HALTEN", sizeof(sStat.nextTurnCmd));
        sStat.nextTurnIcon = ICO_TURN_SLIGHT_LEFT;
    }

    float speedKmh = (sStat.mode == NAV_MODE_CAR) ? 75.0f : 5.5f;
    if (GpsManager::hasFix() && GpsManager::speedKmh() > 2.5f) {
        speedKmh = GpsManager::speedKmh();
    }
    sStat.etaMinutes = (uint16_t)max(1.0f, (float)(remKm / speedKmh * 60.0));
}

void NavEngine::loop(unsigned long now) {
    static unsigned long lastTick = 0;
    if (now - lastTick < 1000) return;
    lastTick = now;

    updateCurrentPosition();
    if (sStat.activeRoute) {
        updateTurnByTurn();
    }

    // GPX Track recording (every 5s when enabled)
    if (sStat.gpxRecording && now - sLastGpxMs >= 5000) {
        sLastGpxMs = now;
        NavRoutePoint pt{(int32_t)(sStat.curLat * 1e5), (int32_t)(sStat.curLon * 1e5)};
        if (sGpxTrail.empty() ||
            abs(sGpxTrail.back().lat_e5 - pt.lat_e5) > 8 ||
            abs(sGpxTrail.back().lon_e5 - pt.lon_e5) > 8) {
            if (sGpxTrail.size() < 2000) sGpxTrail.push_back(pt);
            sStat.gpxPoints = (uint32_t)sGpxTrail.size();

            if (sStat.sdReady) {
                if (!SD.exists("/tracks")) SD.mkdir("/tracks");
                struct tm ti;
                RtcManager::getLocalTimeInfo(&ti);
                char gpxPath[40];
                snprintf(gpxPath, sizeof(gpxPath), "/tracks/%04d-%02d-%02d.gpx",
                         ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
                bool isNew = !SD.exists(gpxPath);
                File f = SD.open(gpxPath, FILE_APPEND);
                if (f) {
                    if (isNew) {
                        f.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
                        f.println("<gpx version=\"1.1\" creator=\"KRONOS WatchOS Ultra\"><trk><trkseg>");
                    }
                    f.printf("<trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele></trkpt>\n",
                             sStat.curLat, sStat.curLon, GpsManager::altitudeM());
                    f.close();
                }
            }
        }
    }
}

const NavStatus& NavEngine::status() { return sStat; }
void NavEngine::setMode(NavMode m) {
    if (sStat.mode == m) return;
    sStat.mode = m;
    if (sStat.activeRoute) {
        startNavigation(sStat.destName, sStat.destLat, sStat.destLon);
    }
}
NavMode NavEngine::getMode() { return sStat.mode; }
bool NavEngine::isNavigating() { return sStat.activeRoute; }

void NavEngine::stopNavigation() {
    sStat.activeRoute = false;
    sRoute.clear();
    strncpy(sStat.nextTurnCmd, "BEREIT", sizeof(sStat.nextTurnCmd));
    nav::markDirty(nav::PG_POSITION);
}

bool NavEngine::startNavigation(const char* name, double destLat, double destLon) {
    updateCurrentPosition();
    strncpy(sStat.destName, name ? name : "Ziel", sizeof(sStat.destName) - 1);
    sStat.destName[sizeof(sStat.destName) - 1] = 0;
    sStat.destLat = destLat;
    sStat.destLon = destLon;
    sRoute.clear();

    // Try offline A* route calculation on germany.knav if loaded (SD or QSPI Flash)
    bool routed = false;
    if (sStat.mapLoaded && sHdr.numRouteNodes > 0 && sHdr.numRouteEdges > 0 &&
        sStat.mode != NAV_MODE_MESH) {
        MapReader mr;
        if (mr.open()) {
            size_t nodesBytes = sHdr.numRouteNodes * sizeof(DiskRouteNode);
            size_t edgesBytes = sHdr.numRouteEdges * sizeof(DiskRouteEdge);
            DiskRouteNode* nodes = (DiskRouteNode*)heap_caps_malloc(nodesBytes, MALLOC_CAP_SPIRAM);
            DiskRouteEdge* edges = (DiskRouteEdge*)heap_caps_malloc(edgesBytes, MALLOC_CAP_SPIRAM);
            if (nodes && edges) {
                mr.readAt(sHdr.routeNodesOffset, nodes, nodesBytes);
                mr.readAt(sHdr.routeEdgesOffset, edges, edgesBytes);

                int32_t sLat5 = (int32_t)(sStat.curLat * 1e5);
                int32_t sLon5 = (int32_t)(sStat.curLon * 1e5);
                int32_t dLat5 = (int32_t)(destLat * 1e5);
                int32_t dLon5 = (int32_t)(destLon * 1e5);

                uint32_t srcNode = 0, dstNode = 0;
                int64_t bestSrcD = 0x7FFFFFFFFFFFFFFFLL, bestDstD = 0x7FFFFFFFFFFFFFFFLL;
                for (uint32_t i = 0; i < sHdr.numRouteNodes; i++) {
                    if (nodes[i].edgeCount == 0) continue;
                    int64_t dsy = nodes[i].lat_e5 - sLat5, dsx = nodes[i].lon_e5 - sLon5;
                    int64_t ds = dsy * dsy + dsx * dsx;
                    if (ds < bestSrcD) { bestSrcD = ds; srcNode = i; }

                    int64_t ddy = nodes[i].lat_e5 - dLat5, ddx = nodes[i].lon_e5 - dLon5;
                    int64_t dd = ddy * ddy + ddx * ddx;
                    if (dd < bestDstD) { bestDstD = dd; dstNode = i; }
                }

                uint32_t* gScore = (uint32_t*)heap_caps_malloc(sHdr.numRouteNodes * 4, MALLOC_CAP_SPIRAM);
                uint32_t* parent = (uint32_t*)heap_caps_malloc(sHdr.numRouteNodes * 4, MALLOC_CAP_SPIRAM);
                if (gScore && parent) {
                    memset(gScore, 0xFF, sHdr.numRouteNodes * 4);
                    memset(parent, 0xFF, sHdr.numRouteNodes * 4);
                    using P = std::pair<uint32_t, uint32_t>;
                    std::priority_queue<P, std::vector<P>, std::greater<P>> pq;
                    bool pedMode = (sStat.mode == NAV_MODE_PED);

                    auto heur = [&](uint32_t u) -> uint32_t {
                        double dlat = (nodes[u].lat_e5 - nodes[dstNode].lat_e5) * 1e-5 * 111.32;
                        double dlon = (nodes[u].lon_e5 - nodes[dstNode].lon_e5) * 1e-5 * 71.5;
                        return (uint32_t)(sqrt(dlat * dlat + dlon * dlon) * 6.0);
                    };

                    gScore[srcNode] = 0;
                    pq.push({heur(srcNode), srcNode});
                    uint32_t maxIter = 70000;
                    while (!pq.empty() && maxIter-- > 0) {
                        auto [fval, u] = pq.top();
                        pq.pop();
                        if (u == dstNode) break;
                        uint32_t gu = gScore[u];
                        for (uint16_t k = 0; k < nodes[u].edgeCount; k++) {
                            const DiskRouteEdge& e = edges[nodes[u].firstEdge + k];
                            uint8_t cls = (e.flags >> 2) & 0x07;
                            uint32_t w = e.dist_hm;
                            if (pedMode) {
                                if (cls == 8) w *= 4; // Avoid motorway for pedestrians
                            } else {
                                if (cls == 8) w = (w * 6) / 10;      // Prefer Autobahn for cars
                                else if (cls == 7) w = (w * 8) / 10; // Bundesstraße
                            }
                            uint32_t ng = gu + max<uint32_t>(1U, w);
                            if (ng < gScore[e.toNode]) {
                                gScore[e.toNode] = ng;
                                parent[e.toNode] = u;
                                pq.push({ng + heur(e.toNode), e.toNode});
                            }
                        }
                    }

                    if (parent[dstNode] != 0xFFFFFFFFU || srcNode == dstNode) {
                        std::vector<NavRoutePoint> rev;
                        rev.push_back({dLat5, dLon5});
                        uint32_t cur = dstNode;
                        uint32_t safety = 2000;
                        while (cur != srcNode && cur != 0xFFFFFFFFU && safety-- > 0) {
                            rev.push_back({nodes[cur].lat_e5, nodes[cur].lon_e5});
                            cur = parent[cur];
                        }
                        rev.push_back({nodes[srcNode].lat_e5, nodes[srcNode].lon_e5});
                        rev.push_back({sLat5, sLon5});
                        std::reverse(rev.begin(), rev.end());
                        sRoute = std::move(rev);
                        routed = true;
                    }
                }
                if (gScore) heap_caps_free(gScore);
                if (parent) heap_caps_free(parent);
            }
            if (nodes) heap_caps_free(nodes);
            if (edges) heap_caps_free(edges);
            mr.close();
        }
    }

    // Fallback multi-segment route if SD map is not loaded yet or target is direct Mesh peer
    if (!routed) {
        const int steps = 16;
        for (int i = 0; i <= steps; i++) {
            double t = (double)i / steps;
            // Slight realistic curve along Germany's corridor when simulating
            double bend = sin(t * M_PI) * 0.06 * ((sStat.mode == NAV_MODE_PED) ? -0.5 : 1.0);
            double la = sStat.curLat + (destLat - sStat.curLat) * t + bend * (destLon - sStat.curLon) * 0.25;
            double lo = sStat.curLon + (destLon - sStat.curLon) * t - bend * (destLat - sStat.curLat) * 0.25;
            sRoute.push_back({(int32_t)(la * 1e5), (int32_t)(lo * 1e5)});
        }
    }

    double totKm = 0;
    for (size_t i = 0; i + 1 < sRoute.size(); i++) {
        totKm += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                             sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
    }
    sStat.totalDistKm = (float)totKm;
    sStat.activeRoute = true;
    updateTurnByTurn();
    nav::markDirty(nav::PG_POSITION);
    return true;
}

static bool matchPrefixCaseInsensitive(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    // Substring match case-insensitive
    for (const char* h = hay; *h; h++) {
        const char* p = h;
        const char* n = needle;
        while (*p && *n) {
            char c1 = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p;
            char c2 = (*n >= 'A' && *n <= 'Z') ? (*n + 32) : *n;
            if (c1 != c2) break;
            p++; n++;
        }
        if (!*n) return true;
    }
    return false;
}

int NavEngine::searchPlaces(const char* query, NavPlace* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    int count = 0;

    // 1. Always include matching live LoRa / Meshtastic peers with coordinates first!
    LoraPeerNode peers[16];
    uint8_t np = RadioLoRa::getPeers(peers, 16);
    for (uint8_t i = 0; i < np && count < maxOut; i++) {
        if (!peers[i].hasPos) continue;
        if (matchPrefixCaseInsensitive(peers[i].name, query) ||
            (query && strcmp(query, "@mesh") == 0)) {
            NavPlace& dst = out[count++];
            snprintf(dst.name, sizeof(dst.name), "%s", peers[i].name);
            dst.lat = peers[i].lat;
            dst.lon = peers[i].lon;
            dst.pop = 0;
            dst.rank = 0; // 0 = LoRa/Meshtastic Peer
        }
    }

    // 2. Search `germany.knav` places table if loaded
    if (sStat.mapLoaded && sHdr.numPlaces > 0) {
        MapReader mr;
        if (mr.open()) {
            uint32_t curOff = sHdr.placesOffset;
            const int batchSize = 64;
            DiskPlace batch[batchSize];
            uint32_t rem = min<uint32_t>(sHdr.numPlaces, ( query && query[0] ) ? sHdr.numPlaces : 500);
            while (rem > 0 && count < maxOut) {
                uint32_t n = min<uint32_t>(rem, batchSize);
                if (!mr.readAt(curOff, batch, n * sizeof(DiskPlace))) break;
                curOff += n * sizeof(DiskPlace);
                for (uint32_t i = 0; i < n && count < maxOut; i++) {
                    if (matchPrefixCaseInsensitive(batch[i].name, query)) {
                        NavPlace& dst = out[count++];
                        strncpy(dst.name, batch[i].name, sizeof(dst.name) - 1);
                        dst.name[sizeof(dst.name) - 1] = 0;
                        dst.lat = batch[i].lat_e5 * 1e-5;
                        dst.lon = batch[i].lon_e5 * 1e-5;
                        dst.pop = batch[i].pop;
                        dst.rank = batch[i].rank;
                    }
                }
                rem -= n;
            }
            mr.close();
            if (count > 0) return count;
        }
    }

    // 3. Built-in German cities fallback
    for (int i = 0; i < N_BUILTIN_CITIES && count < maxOut; i++) {
        if (matchPrefixCaseInsensitive(BUILTIN_CITIES[i].name, query)) {
            out[count++] = BUILTIN_CITIES[i];
        }
    }
    return count;
}

void NavEngine::setTrackRecording(bool rec) {
    sStat.gpxRecording = rec;
    if (rec) sLastGpxMs = 0;
}
bool NavEngine::isTrackRecording() { return sStat.gpxRecording; }

bool NavEngine::markWaypoint(const char* label) {
    updateCurrentPosition();
    if (!sStat.sdReady) return false;
    File f = SD.open("/waypoints.csv", FILE_APPEND);
    if (!f) return false;
    f.printf("%lu,%s,%.6f,%.6f,%.1f\n",
             (unsigned long)time(NULL), label ? label : "POI",
             sStat.curLat, sStat.curLon, GpsManager::altitudeM());
    f.close();
    return true;
}

// =============================================================================
// Fast RGB565 Software Vector Map Renderer (AMOLED Dark Tactical Theme)
// =============================================================================
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

static inline void putPx(uint16_t* buf, int w, int h, int x, int y, uint16_t col) {
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h) {
        buf[y * w + x] = col;
    }
}

static void drawLine(uint16_t* buf, int w, int h, int x0, int y0, int x1, int y1, uint16_t col, int thickness = 1) {
    // Trivial reject if both points are far outside viewport
    if ((x0 < -50 && x1 < -50) || (x0 >= w + 50 && x1 >= w + 50) ||
        (y0 < -50 && y1 < -50) || (y0 >= h + 50 && y1 >= h + 50)) return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int maxSteps = w + h + 200;
    while (maxSteps-- > 0) {
        if (thickness <= 1) {
            putPx(buf, w, h, x0, y0, col);
        } else if (thickness == 2) {
            putPx(buf, w, h, x0, y0, col);
            putPx(buf, w, h, x0 + 1, y0, col);
            putPx(buf, w, h, x0, y0 + 1, col);
        } else {
            for (int ty = -1; ty <= 1; ty++)
                for (int tx = -1; tx <= 1; tx++)
                    putPx(buf, w, h, x0 + tx, y0 + ty, col);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void drawFilledCircle(uint16_t* buf, int w, int h, int cx, int cy, int r, uint16_t col) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) putPx(buf, w, h, cx + dx, cy + dy, col);
        }
    }
}

static void drawRing(uint16_t* buf, int w, int h, int cx, int cy, int r, uint16_t col) {
    int r2Min = (r - 1) * (r - 1);
    int r2Max = (r + 1) * (r + 1);
    for (int dy = -r - 1; dy <= r + 1; dy++) {
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2Min && d2 <= r2Max) putPx(buf, w, h, cx + dx, cy + dy, col);
        }
    }
}

// Compact 5x7 uppercase/digit bitmap font for fast inline map labels
static const uint8_t MINI_FONT[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space (32)
    {0x00,0x00,0x5F,0x00,0x00}, // ! (33)
    {0x00,0x00,0x00,0x00,0x00}, // " (34)
    {0x14,0x7F,0x14,0x7F,0x14}, // # (35)
    {0x00,0x00,0x00,0x00,0x00}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x00,0x00,0x00,0x00,0x00}, // &
    {0x00,0x00,0x07,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x00,0x00,0x00,0x00,0x00}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0 (48)
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A (65)
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
};

static void drawMapText(uint16_t* buf, int w, int h, int x, int y, const char* txt, uint16_t col, uint16_t haloCol) {
    int cx = x;
    for (int i = 0; txt[i] && i < 18; i++) {
        uint8_t c = (uint8_t)txt[i];
        // Map UTF-8 German umlauts (0xC3 0xA4/0xB6/0xBC/etc.) to A/O/U/S
        if (c == 0xC3 && txt[i + 1]) {
            uint8_t c2 = (uint8_t)txt[++i];
            if (c2 == 0xA4 || c2 == 0x84) c = 'A';
            else if (c2 == 0xB6 || c2 == 0x96) c = 'O';
            else if (c2 == 0xBC || c2 == 0x9C) c = 'U';
            else if (c2 == 0x9F) c = 'S';
            else c = ' ';
        }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 'Z') c = ' ';
        const uint8_t* glyph = MINI_FONT[c - 32];
        for (int colIdx = 0; colIdx < 5; colIdx++) {
            uint8_t bits = glyph[colIdx];
            for (int rowIdx = 0; rowIdx < 7; rowIdx++) {
                if (bits & (1 << rowIdx)) {
                    putPx(buf, w, h, cx + colIdx + 1, y + rowIdx + 1, haloCol);
                    putPx(buf, w, h, cx + colIdx, y + rowIdx, col);
                }
            }
        }
        cx += 6;
    }
}

static const uint8_t* getCachedCellData(MapReader& mr, bool isFine, int cellIdx, uint32_t& outLen) {
    int32_t key = ((isFine ? 1 : 0) << 20) | cellIdx;
    sCacheClock++;
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].key == key) {
            sCellCache[i].lastUsed = sCacheClock;
            outLen = sCellCache[i].len;
            return sCellCache[i].data;
        }
    }

    // Read directory entry
    uint32_t dirOff = (isFine ? sHdr.fineDirOffset : sHdr.coarseDirOffset) + cellIdx * sizeof(CellDirEntry);
    CellDirEntry de{};
    if (!mr.readAt(dirOff, &de, sizeof(de)) || de.length == 0 || de.length > 65536) {
        outLen = 0;
        return nullptr;
    }

    // Pick LRU slot
    int victim = 0;
    for (int i = 1; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].key == -1) { victim = i; break; }
        if (sCellCache[i].lastUsed < sCellCache[victim].lastUsed) victim = i;
    }
    if (sCellCache[victim].data) {
        heap_caps_free(sCellCache[victim].data);
        sCellCache[victim].data = nullptr;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(de.length, MALLOC_CAP_SPIRAM);
    if (!buf) { outLen = 0; return nullptr; }
    if (!mr.readAt(de.offset, buf, de.length)) {
        heap_caps_free(buf);
        outLen = 0;
        return nullptr;
    }

    sCellCache[victim].key = key;
    sCellCache[victim].lastUsed = sCacheClock;
    sCellCache[victim].data = buf;
    sCellCache[victim].len = de.length;
    outLen = de.length;
    return buf;
}

void NavEngine::renderMap(uint16_t* buf, int w, int h, double viewLat, double viewLon, int zoom) {
    if (!buf || w <= 0 || h <= 0) return;

    // 1. Dark tactical background fill
    uint16_t bgCol = rgb565(10, 14, 22);
    uint16_t gridCol = rgb565(18, 25, 38);
    for (int i = 0; i < w * h; i++) buf[i] = bgCol;

    // Degrees per pixel at zoom (zoom 6 = whole Germany, zoom 15 = street level)
    double lonPerPx = 360.0 / (256.0 * pow(2.0, (double)zoom));
    double cosLat = cos(viewLat * M_PI / 180.0);
    if (cosLat < 0.2) cosLat = 0.2;
    double latPerPx = lonPerPx * cosLat;

    auto project = [&](double lat, double lon, int& sx, int& sy) {
        sx = (int)(w * 0.5 + (lon - viewLon) / lonPerPx);
        sy = (int)(h * 0.5 - (lat - viewLat) / latPerPx);
    };

    // Subtle tactical coordinate grid
    for (int gx = (w / 2) % 48; gx < w; gx += 48) {
        for (int y = 0; y < h; y += 2) buf[y * w + gx] = gridCol;
    }
    for (int gy = (h / 2) % 48; gy < h; gy += 48) {
        for (int x = 0; x < w; x += 2) buf[gy * w + x] = gridCol;
    }

    // 2. Always draw Germany border outline at overview zoom levels
    uint16_t borderCol = rgb565(56, 74, 102);
    for (int i = 0; i + 1 < N_BORDER_PTS; i++) {
        int x0, y0, x1, y1;
        project(DE_BORDER_PTS[i][0] * 1e-5, DE_BORDER_PTS[i][1] * 1e-5, x0, y0);
        project(DE_BORDER_PTS[i + 1][0] * 1e-5, DE_BORDER_PTS[i + 1][1] * 1e-5, x1, y1);
        drawLine(buf, w, h, x0, y0, x1, y1, borderCol, 2);
    }

    // 3. Render vector tiles from SD card `germany.knav` if loaded
    double minLat = viewLat - (h * 0.55) * latPerPx;
    double maxLat = viewLat + (h * 0.55) * latPerPx;
    double minLon = viewLon - (w * 0.55) * lonPerPx;
    double maxLon = viewLon + (w * 0.55) * lonPerPx;

    if (sStat.mapLoaded) {
        MapReader mr;
        if (mr.open()) {
            bool useFine = (zoom >= 10);
            int nx = useFine ? FINE_NX : COARSE_NX;
            int ny = useFine ? FINE_NY : COARSE_NY;
            double latSpan = DE_MAX_LAT - DE_MIN_LAT;
            double lonSpan = DE_MAX_LON - DE_MIN_LON;

            int cy0 = std::clamp((int)((minLat - DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
            int cy1 = std::clamp((int)((maxLat - DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
            int cx0 = std::clamp((int)((minLon - DE_MIN_LON) / lonSpan * nx), 0, nx - 1);
            int cx1 = std::clamp((int)((maxLon - DE_MIN_LON) / lonSpan * nx), 0, nx - 1);

            // Cap cell span per frame so wide zoom stays 60fps
            if (useFine && (cy1 - cy0 > 5 || cx1 - cx0 > 5)) {
                useFine = false;
                nx = COARSE_NX; ny = COARSE_NY;
                cy0 = std::clamp((int)((minLat - DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
                cy1 = std::clamp((int)((maxLat - DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
                cx0 = std::clamp((int)((minLon - DE_MIN_LON) / lonSpan * nx), 0, nx - 1);
                cx1 = std::clamp((int)((maxLon - DE_MIN_LON) / lonSpan * nx), 0, nx - 1);
            }

            double cellLatH = latSpan / ny;
            double cellLonW = lonSpan / nx;

            for (int cy = cy0; cy <= cy1; cy++) {
                for (int cx = cx0; cx <= cx1; cx++) {
                    uint32_t clen = 0;
                    const uint8_t* cdata = getCachedCellData(mr, useFine, cy * nx + cx, clen);
                    if (!cdata || clen < 2) continue;

                    double baseLat = DE_MIN_LAT + cy * cellLatH;
                    double baseLon = DE_MIN_LON + cx * cellLonW;

                    uint16_t nseg = *(const uint16_t*)cdata;
                    size_t pos = 2;
                    for (uint16_t s = 0; s < nseg && pos + 2 <= clen; s++) {
                        uint8_t cls = cdata[pos++];
                        uint8_t npts = cdata[pos++];
                        if (pos + npts * 2 > clen) break;

                        // Filter minor streets at low zoom
                        if (zoom < 12 && (cls == 3 || cls == 4)) {
                            pos += npts * 2;
                            continue;
                        }

                        uint16_t col = rgb565(65, 75, 92);
                        int thick = 1;
                        switch (cls) {
                            case 0: col = rgb565(24, 130, 210); thick = 2; break; // Water
                            case 1: col = rgb565(75, 95, 125);  thick = 2; break; // Boundary
                            case 2: col = rgb565(90, 90, 115);  thick = 1; break; // Rail
                            case 3: col = rgb565(45, 110, 68);  thick = 1; break; // Path/Trail
                            case 4: col = rgb565(58, 66, 82);   thick = 1; break; // Residential
                            case 5: col = rgb565(95, 105, 125); thick = 1; break; // Tertiary
                            case 6: col = rgb565(140, 150, 175);thick = (zoom >= 12) ? 2 : 1; break; // Secondary
                            case 7: col = rgb565(235, 175, 20); thick = 2; break; // Primary / Bundesstraße
                            case 8: col = rgb565(255, 95, 20);  thick = (zoom >= 9) ? 2 : 1; break; // Autobahn
                        }

                        int px0 = 0, py0 = 0;
                        for (uint8_t k = 0; k < npts; k++) {
                            uint8_t qx = cdata[pos++];
                            uint8_t qy = cdata[pos++];
                            double ptLon = baseLon + (qx / 255.0) * cellLonW;
                            double ptLat = baseLat + (qy / 255.0) * cellLatH;
                            int sx, sy;
                            project(ptLat, ptLon, sx, sy);
                            if (k > 0) drawLine(buf, w, h, px0, py0, sx, sy, col, thick);
                            px0 = sx; py0 = sy;
                        }
                    }
                }
            }
            mr.close();
        }
    } else {
        // Built-in Autobahn backbone connecting major German cities when SD map is not loaded yet
        static const uint8_t AUTOBAHN_LINKS[][2] = {
            {0, 1}, {0, 7}, {7, 2}, {0, 11}, {11, 2}, {1, 10}, {10, 12},
            {12, 8}, {8, 9}, {9, 14}, {14, 6}, {6, 3}, {3, 18}, {18, 4},
            {4, 20}, {20, 21}, {21, 26}, {4, 13}, {13, 2}, {2, 22}, {22, 5}, {5, 21}
        };
        uint16_t abCol = rgb565(220, 90, 25);
        for (auto& lk : AUTOBAHN_LINKS) {
            int x0, y0, x1, y1;
            project(BUILTIN_CITIES[lk[0]].lat, BUILTIN_CITIES[lk[0]].lon, x0, y0);
            project(BUILTIN_CITIES[lk[1]].lat, BUILTIN_CITIES[lk[1]].lon, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, abCol, 2);
        }
    }

    // 4. Draw GPX recorded breadcrumb trail (amber dotted line)
    if (sGpxTrail.size() >= 2) {
        uint16_t gpxCol = rgb565(255, 200, 30);
        for (size_t i = 0; i + 1 < sGpxTrail.size(); i++) {
            int x0, y0, x1, y1;
            project(sGpxTrail[i].lat_e5 * 1e-5, sGpxTrail[i].lon_e5 * 1e-5, x0, y0);
            project(sGpxTrail[i + 1].lat_e5 * 1e-5, sGpxTrail[i + 1].lon_e5 * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, gpxCol, 2);
        }
    }

    // 5. Draw Active Navigation Route Polyline (glowing cyan/lime)
    if (sStat.activeRoute && sRoute.size() >= 2) {
        uint16_t routeHalo = rgb565(0, 90, 110);
        uint16_t routeCore = (sStat.mode == NAV_MODE_PED) ? rgb565(50, 235, 110) : rgb565(0, 245, 212);
        for (size_t i = 0; i + 1 < sRoute.size(); i++) {
            int x0, y0, x1, y1;
            project(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5, x0, y0);
            project(sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, routeHalo, 3);
            drawLine(buf, w, h, x0, y0, x1, y1, routeCore, 2);
        }
        // Destination Pin
        int dx, dy;
        project(sStat.destLat, sStat.destLon, dx, dy);
        drawFilledCircle(buf, w, h, dx, dy, 6, rgb565(255, 60, 60));
        drawRing(buf, w, h, dx, dy, 8, rgb565(255, 255, 255));
        drawMapText(buf, w, h, dx + 10, dy - 4, sStat.destName, rgb565(255, 120, 120), rgb565(0, 0, 0));
    }

    // 6. Draw City Names & Dots
    uint16_t cityDotCol = rgb565(220, 230, 245);
    uint16_t cityTxtCol = rgb565(200, 215, 235);
    int maxCities = (zoom <= 7) ? 14 : (zoom <= 9) ? 24 : N_BUILTIN_CITIES;
    for (int i = 0; i < maxCities; i++) {
        int sx, sy;
        project(BUILTIN_CITIES[i].lat, BUILTIN_CITIES[i].lon, sx, sy);
        if (sx >= 10 && sx < w - 45 && sy >= 10 && sy < h - 10) {
            drawFilledCircle(buf, w, h, sx, sy, 2, cityDotCol);
            drawMapText(buf, w, h, sx + 5, sy - 3, BUILTIN_CITIES[i].name, cityTxtCol, rgb565(5, 8, 14));
        }
    }

    // 7. Draw Live LoRa / Meshtastic Peer Nodes on the Map!
    LoraPeerNode peers[16];
    uint8_t nPeers = RadioLoRa::getPeers(peers, 16);
    for (uint8_t i = 0; i < nPeers; i++) {
        if (!peers[i].hasPos) continue;
        int px, py;
        project(peers[i].lat, peers[i].lon, px, py);
        if (px >= -10 && px < w + 10 && py >= -10 && py < h + 10) {
            uint16_t pCol = peers[i].isSos ? rgb565(255, 50, 60) : rgb565(195, 90, 255);
            drawFilledCircle(buf, w, h, px, py, 5, pCol);
            drawRing(buf, w, h, px, py, 7, rgb565(255, 255, 255));
            drawMapText(buf, w, h, px + 9, py - 3, peers[i].name, pCol, rgb565(0, 0, 0));
        }
    }

    // 8. Draw Current Position Marker ("You are here" GPS / LoRa Mesh Assisted)
    int ux, uy;
    project(sStat.curLat, sStat.curLon, ux, uy);
    if (ux >= -12 && ux < w + 12 && uy >= -12 && uy < h + 12) {
        uint16_t myCol = (sStat.posSource == POS_SRC_GPS_LIVE)  ? rgb565(48, 209, 88) :
                         (sStat.posSource == POS_SRC_LORA_MESH) ? rgb565(191, 90, 242) :
                                                                  rgb565(10, 132, 255);
        drawRing(buf, w, h, ux, uy, 11, myCol);
        drawFilledCircle(buf, w, h, ux, uy, 5, myCol);
        drawRing(buf, w, h, ux, uy, 6, rgb565(255, 255, 255));
    }

    // 9. Scale bar in bottom-left corner of map canvas
    double barKm = lonPerPx * cosLat * 111.32 * 60.0;
    char scaleTxt[20];
    if (barKm >= 10.0) snprintf(scaleTxt, sizeof(scaleTxt), "%.0f KM", barKm);
    else if (barKm >= 1.0) snprintf(scaleTxt, sizeof(scaleTxt), "%.1f KM", barKm);
    else snprintf(scaleTxt, sizeof(scaleTxt), "%.0f M", barKm * 1000.0);
    drawLine(buf, w, h, 12, h - 12, 72, h - 12, rgb565(220, 230, 240), 2);
    drawLine(buf, w, h, 12, h - 16, 12, h - 8, rgb565(220, 230, 240), 1);
    drawLine(buf, w, h, 72, h - 16, 72, h - 8, rgb565(220, 230, 240), 1);
    drawMapText(buf, w, h, 16, h - 23, scaleTxt, rgb565(220, 230, 240), rgb565(0, 0, 0));
}

// =============================================================================
// High-speed SD File Upload Support (Serial / HTTP)
// =============================================================================
bool NavEngine::beginSdUpload(const char* path, uint32_t totalSize) {
    if (!sStat.sdReady && !instance.installSD()) return false;
    sStat.sdReady = true;
    clearCellCache();
    // Ensure LoRa and NFC CS pins are HIGH on the shared SPI bus before SD write
    digitalWrite(LORA_CS, HIGH);
    digitalWrite(NFC_CS, HIGH);
    SD.mkdir("/kronos");
    sUploadFile = SD.open(path, FILE_WRITE, true);
    if (!sUploadFile) {
        // Fallback to root path /germany.knav
        sUploadFile = SD.open("/germany.knav", FILE_WRITE, true);
    }
    if (!sUploadFile) return false;
    sUploadRemaining = totalSize;
    return true;
}

bool NavEngine::writeSdUploadChunk(const uint8_t* data, size_t len) {
    if (!sUploadFile) return false;
    size_t w = sUploadFile.write(data, len);
    if (w != len) return false;
    if (sUploadRemaining >= len) sUploadRemaining -= (uint32_t)len;
    else sUploadRemaining = 0;
    return true;
}

bool NavEngine::endSdUpload() {
    if (sUploadFile) {
        sUploadFile.flush();
        sUploadFile.close();
    }
    return reloadMap();
}

