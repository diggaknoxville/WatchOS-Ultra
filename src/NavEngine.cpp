// =============================================================================
//  NavEngine - offline vector map, place search, A* routing, GPX logging
//
//  See include/NavEngine.h for the threading model and include/MapFormat.h
//  for the on-disk container. Log prefixes: [NAV] engine, [MAP] map file /
//  loader, [SD] card handling.
// =============================================================================
#include "NavEngine.h"
#include "MapFormat.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "RtcManager.h"
#include "ui/Icons.h"
#include "ui/Navigator.h"
#include "DisplayEngine.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Germany centre, used when nothing better is known
static constexpr double DE_CENTER_LAT = 51.1657;
static constexpr double DE_CENTER_LON = 10.4515;

static constexpr const char* MAP_SD_PATH_PRIMARY = "/kronos/germany.knav";
static constexpr const char* MAP_SD_TMP_PATH     = "/kronos/germany.tmp";
static constexpr const char* MAP_SD_CANDIDATES[] = {"/kronos/germany.knav", "/germany.knav", "/map/germany.knav"};
static constexpr const char* GPX_DIR = "/tracks";

// =============================================================================
// Locks
// =============================================================================
// Engine mutex: guards every piece of shared state in this file.
static SemaphoreHandle_t navMutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}
// I/O mutex (leaf): serialises SD card access between the background loader
// (which runs without the engine mutex) and everything else. Never take the
// engine mutex while holding it.
static SemaphoreHandle_t ioMutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

struct NavLock {
    NavLock() { xSemaphoreTakeRecursive(navMutex(), portMAX_DELAY); }
    ~NavLock() { xSemaphoreGiveRecursive(navMutex()); }
    NavLock(const NavLock&) = delete;
    NavLock& operator=(const NavLock&) = delete;
};
struct IoLock {
    IoLock() { xSemaphoreTakeRecursive(ioMutex(), portMAX_DELAY); }
    ~IoLock() { xSemaphoreGiveRecursive(ioMutex()); }
    IoLock(const IoLock&) = delete;
    IoLock& operator=(const IoLock&) = delete;
};

// =============================================================================
// Static state (all guarded by the engine mutex unless noted)
// =============================================================================
static NavStatus sStat{};
static bool sInited = false;
static unsigned long sInitMs = 0;

// --- map source --------------------------------------------------------------
static KnavHeader sHdr{};
static bool sHdrIsV3 = false;
static uint64_t sFileBound = 0;             // readable bytes (SD file size or flash partition size)
static uint32_t sMapGen = 0;                // bumped on every unload; async jobs compare against it
static char sMapPath[48] = "";
static const esp_partition_t* sFlashPart = nullptr;
static bool sUseFlashPart = false;
static bool sMirrorPending = false;         // flash map should be mirrored to SD (deferred, chunked)
static bool sDeferredBootDone = false;

// --- PSRAM tables (published by the loader, freed by unloadMapLocked) --------
static DiskPlace* sPlaces = nullptr;
static uint32_t sNumPlaces = 0;
static DiskPoi* sPois = nullptr;
static uint32_t sNumPois = 0;
static uint32_t* sPoiCellStart = nullptr;   // KNAV_ULTRA_CELLS + 1 entries (CSR index by ultra cell)
static uint32_t* sPoiOrder = nullptr;       // numPois entries, indexes into sPois
static DiskRouteNode* sNodes = nullptr;
static DiskRouteEdge* sEdges = nullptr;
static uint32_t sNumNodes = 0, sNumEdges = 0;
static bool sGraphReady = false;
static bool sLoaderFinished = false;        // all stages for the current generation done (or skipped)
// Non-empty-cell bitmaps per tier (built from the directories, entries validated)
static uint8_t* sCellBitmap[3] = {nullptr, nullptr, nullptr};
static bool sCellBitmapReady[3] = {false, false, false};

// --- waypoints -----------------------------------------------------------------
struct UserWaypoint {
    char name[24];
    double lat;
    double lon;
};
static constexpr int MAX_USER_WAYPOINTS = 16;
static UserWaypoint sWaypoints[MAX_USER_WAYPOINTS];
static int sNumWaypoints = 0;

// --- route -----------------------------------------------------------------
static std::vector<NavRoutePoint> sRoute;
struct RouteRequest {
    bool pending = false;
    uint32_t serial = 0;
    double sLat = 0, sLon = 0, dLat = 0, dLon = 0;
    NavMode mode = NAV_MODE_CAR;
};
static RouteRequest sReq;
static uint32_t sReqSerial = 0;
static bool sRoutingActive = false;         // background task is inside A* (graph must not be freed)
static bool sGraphDropPending = false;      // unloadMapLocked() ran while routing; free afterwards
static DiskRouteNode* sDropNodes = nullptr;
static DiskRouteEdge* sDropEdges = nullptr;
static bool sRouteWaitingForPos = false;    // request accepted without a position; route when one appears

// --- GPX ---------------------------------------------------------------------
static std::vector<NavRoutePoint> sGpxTrail;
static unsigned long sLastGpxMs = 0;
static char sGpxOpenPath[40] = "";          // file with an open <trkseg>, "" = none
static char sGpxOpenDay[12] = "";           // local date (YYYY-MM-DD) that file belongs to

// --- scratch (used only under the engine mutex) ------------------------------
static LoraPeerNode sPeerScratch[16];

// --- shared SD write test ------------------------------------------------------------
static File sUploadFile;
static uint32_t sUploadRemaining = 0;
static bool sUploadActive = false;

// =============================================================================
// Built-in city list. Used ONLY as a fallback label / search source while no
// map place table is resident in PSRAM (no map, or tables still loading).
// With a loaded map, labels come from the map's place table (rank <= 2 first).
// =============================================================================
static const NavPlace BUILTIN_CITIES[] = {
    {"Stuttgart",              48.7758,  9.1829,  632865, 1},
    {"Esslingen am Neckar",    48.7406,  9.3108,   94941, 2},
    {"Ludwigsburg",            48.8973,  9.1916,   94157, 2},
    {"Böblingen",              48.6855,  9.0153,   51460, 2},
    {"Sindelfingen",           48.7093,  9.0032,   64995, 2},
    {"Waiblingen",             48.8302,  9.3169,   55820, 2},
    {"Göppingen",              48.7035,  9.6528,   59053, 2},
    {"Tübingen",               48.5216,  9.0576,   92811, 2},
    {"Reutlingen",             48.4914,  9.2043,  117547, 1},
    {"Heilbronn",              49.1427,  9.2109,  128334, 1},
    {"Leonberg",               48.8014,  9.0131,   49512, 2},
    {"Fellbach",               48.8087,  9.2764,   45951, 2},
    {"Filderstadt",            48.6601,  9.2185,   46191, 2},
    {"Schorndorf",             48.8055,  9.5262,   40204, 2},
    {"Backnang",               48.9465,  9.4309,   37882, 2},
    {"Kirchheim unter Teck",   48.6484,  9.4510,   41822, 2},
    {"Nürtingen",              48.6267,  9.3353,   41418, 2},
    {"Bietigheim-Bissingen",   48.9598,  9.1268,   43755, 2},
    {"Kornwestheim",           48.8597,  9.1852,   33980, 2},
    {"Herrenberg",             48.5955,  8.8679,   32649, 2},
    {"Vaihingen an der Enz",   48.9329,  8.9564,   29467, 2},
    {"Calw",                   48.7145,  8.7405,   24219, 2},
    {"Berlin",                 52.5200, 13.4050, 3769962, 1},
    {"Hamburg",                53.5511,  9.9937, 1862565, 1},
    {"München",                48.1351, 11.5820, 1505036, 1},
    {"Köln",                   50.9375,  6.9603, 1069200, 1},
    {"Frankfurt am Main",      50.1109,  8.6821,  773068, 1},
    {"Düsseldorf",             51.2277,  6.7735,  653253, 1},
    {"Leipzig",                51.3397, 12.3731,  628718, 1},
    {"Dortmund",               51.5136,  7.4653,  600933, 1},
    {"Bremen",                 53.0793,  8.8017,  569396, 1},
    {"Dresden",                51.0504, 13.7373,  572240, 1},
    {"Hannover",               52.3759,  9.7320,  555372, 1},
    {"Nürnberg",               49.4521, 11.0767,  531159, 1},
    {"Karlsruhe",              49.0069,  8.4037,  309964, 1},
    {"Mannheim",               49.4875,  8.4660,  315554, 1},
    {"Ulm",                    48.4011,  9.9876,  128928, 1},
    {"Freiburg",               47.9990,  7.8421,  236140, 1},
};
static constexpr int N_BUILTIN_CITIES = sizeof(BUILTIN_CITIES) / sizeof(BUILTIN_CITIES[0]);

// Coarse outline of the German border (lat_e5, lon_e5), drawn at overview zoom.
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

// =============================================================================
// Small helpers
// =============================================================================
static inline double deg2rad(double d) { return d * M_PI / 180.0; }

static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    double dLat = deg2rad(lat2 - lat1);
    double dLon = deg2rad(lon2 - lon1);
    double a = sin(dLat * 0.5) * sin(dLat * 0.5) +
               cos(deg2rad(lat1)) * cos(deg2rad(lat2)) * sin(dLon * 0.5) * sin(dLon * 0.5);
    return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static float calcBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    double phi1 = deg2rad(lat1), phi2 = deg2rad(lat2);
    double dLon = deg2rad(lon2 - lon1);
    double y = sin(dLon) * cos(phi2);
    double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
    double brng = atan2(y, x) * 180.0 / M_PI;
    return (float)fmod(brng + 360.0, 360.0);
}

static inline double kmPerDegLon(double lat) {
    double c = cos(deg2rad(lat));
    if (c < 0.2) c = 0.2;
    return 111.32 * c;
}

static bool psramCanAlloc(size_t bytes, size_t reserve = 768 * 1024) {
    if (bytes == 0) return false;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t freeB = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return largest >= bytes && freeB >= bytes + reserve;
}

static void* psramAlloc(size_t bytes) {
    if (!psramCanAlloc(bytes)) return nullptr;
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

static void fmtAge(char* out, size_t n, uint32_t sec) {
    if (sec < 3600) snprintf(out, n, "%lum", (unsigned long)(sec / 60));
    else if (sec < 172800) snprintf(out, n, "%luh", (unsigned long)(sec / 3600));
    else snprintf(out, n, "%lud", (unsigned long)(sec / 86400));
}

static bool timeIsPlausible() {
    time_t t = time(NULL);
    return t > 1700000000 && RtcManager::isTimeValid();
}

static void setTurn(const char* cmd, const char* icon) {
    strncpy(sStat.nextTurnCmd, cmd, sizeof(sStat.nextTurnCmd) - 1);
    sStat.nextTurnCmd[sizeof(sStat.nextTurnCmd) - 1] = 0;
    sStat.nextTurnIcon = icon ? icon : ICO_NAVIGATION;
}

// =============================================================================
// Map reader (SD file or flash partition). readAt() is bounds-checked against
// the readable size handed to open(). SD access goes through the I/O mutex.
// =============================================================================
struct MapReader {
    File sdFile;
    const esp_partition_t* part = nullptr;
    uint64_t bound = 0;
    bool valid = false;

    bool open(bool useFlash, const esp_partition_t* flashPart, const char* path, uint64_t readable) {
        close();
        bound = readable;
        if (useFlash) {
            if (!flashPart) return false;
            part = flashPart;
            valid = true;
            return true;
        }
        IoLock io;
        sdFile = SD.open(path, FILE_READ);
        valid = (bool)sdFile;
        if (valid && bound == 0) bound = sdFile.size();
        return valid;
    }
    bool readAt(uint64_t offset, void* dst, size_t len) {
        if (!valid || !dst) return false;
        if (len == 0) return true;
        if (offset + (uint64_t)len > bound) return false;
        if (part) {
            if (offset + len > part->size) return false;
            return esp_partition_read(part, (size_t)offset, dst, len) == ESP_OK;
        }
        IoLock io;
        if (!sdFile.seek((uint32_t)offset)) return false;
        return sdFile.read((uint8_t*)dst, len) == len;
    }
    void close() {
        if (sdFile) {
            IoLock io;
            sdFile.close();
        }
        part = nullptr;
        valid = false;
    }
};

// Persistent reader for rendering (opened by reloadMap, closed by unload).
static MapReader sMapRd;

// =============================================================================
// Header validation
// =============================================================================
static bool rangeOk(uint64_t off, uint64_t count, uint64_t elemSize, uint64_t bound) {
    if (count == 0) return true;
    uint64_t end = off + count * elemSize;
    return off > 0 && end <= bound;
}

// Reads and validates a header from a reader whose bound is already known.
static bool readHeader(MapReader& rd, KnavHeader& h, bool& isV3) {
    memset(&h, 0, sizeof(h));
    char magic[KNAV_MAGIC_LEN];
    if (!rd.readAt(0, magic, sizeof(magic))) return false;
    if (knavMagicIsV3(magic)) {
        if (!rd.readAt(0, &h, KNAV_HEADER_V3_SIZE)) return false;
        isV3 = true;
    } else if (knavMagicIsV2(magic)) {
        if (!rd.readAt(0, &h, KNAV_HEADER_V2_SIZE)) return false;
        isV3 = false;
    } else {
        return false;
    }
    uint64_t bound = rd.bound;
    if (isV3) {
        if (h.fileSize < KNAV_HEADER_V3_SIZE || h.fileSize > bound) {
            Serial.printf("[MAP] header fileSize %lu exceeds readable size %llu\n",
                          (unsigned long)h.fileSize, (unsigned long long)bound);
            return false;
        }
        bound = h.fileSize;
    }
    if (h.numPlaces > KNAV_MAX_PLACES || h.numRouteNodes > KNAV_MAX_ROUTE_NODES ||
        h.numRouteEdges > KNAV_MAX_ROUTE_EDGES || h.numPois > KNAV_MAX_POIS) {
        Serial.println("[MAP] header counts out of range");
        return false;
    }
    if (!rangeOk(h.placesOffset, h.numPlaces, sizeof(DiskPlace), bound) ||
        !rangeOk(h.routeNodesOffset, h.numRouteNodes, sizeof(DiskRouteNode), bound) ||
        !rangeOk(h.routeEdgesOffset, h.numRouteEdges, sizeof(DiskRouteEdge), bound) ||
        !rangeOk(h.poisOffset, h.numPois, sizeof(DiskPoi), bound)) {
        Serial.println("[MAP] header table offsets outside file");
        return false;
    }
    if (h.coarseDirOffset && !rangeOk(h.coarseDirOffset, KNAV_COARSE_CELLS, sizeof(CellDirEntry), bound)) return false;
    if (h.fineDirOffset   && !rangeOk(h.fineDirOffset,   KNAV_FINE_CELLS,   sizeof(CellDirEntry), bound)) return false;
    if (h.ultraDirOffset  && !rangeOk(h.ultraDirOffset,  KNAV_ULTRA_CELLS,  sizeof(CellDirEntry), bound)) return false;
    return true;
}

static uint32_t dirOffsetForTier(int tier) {
    return tier == 2 ? sHdr.ultraDirOffset : tier == 1 ? sHdr.fineDirOffset : sHdr.coarseDirOffset;
}

// =============================================================================
// Cell cache (PSRAM, LRU, byte budgeted). Keys: (tier << 20) | cellIdx.
// =============================================================================
struct CachedCell {
    int32_t key;        // -1 = unused
    uint32_t lastUsed;
    uint8_t* data;      // nullptr with len 0 = known empty cell
    uint32_t len;
};
static constexpr int CELL_CACHE_SLOTS = 192;
static constexpr uint32_t CELL_CACHE_BUDGET = 1536u * 1024u;
static CachedCell sCellCache[CELL_CACHE_SLOTS];
static uint32_t sCacheClock = 0;
static uint32_t sCacheBytes = 0;
static bool sWarnedBigCell = false;

static void clearCellCache() {
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].data) heap_caps_free(sCellCache[i].data);
        sCellCache[i].data = nullptr;
        sCellCache[i].key = -1;
        sCellCache[i].len = 0;
        sCellCache[i].lastUsed = 0;
    }
    sCacheBytes = 0;
}

static int cacheVictim() {
    int victim = 0;
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].key == -1) return i;
        if (sCellCache[i].lastUsed < sCellCache[victim].lastUsed) victim = i;
    }
    return victim;
}

static void cacheEvict(int slot) {
    if (sCellCache[slot].data) {
        heap_caps_free(sCellCache[slot].data);
        sCellCache[slot].data = nullptr;
        if (sCacheBytes >= sCellCache[slot].len) sCacheBytes -= sCellCache[slot].len;
        else sCacheBytes = 0;
    }
    sCellCache[slot].key = -1;
    sCellCache[slot].len = 0;
}

static inline bool cellBitmapGet(int tier, uint32_t idx) {
    return (sCellBitmap[tier][idx >> 3] >> (idx & 7)) & 1;
}
static inline void cellBitmapSet(uint8_t* bm, uint32_t idx) {
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

// Returns the payload of a cell (nullptr with outLen 0 if empty / invalid).
static const uint8_t* getCachedCellData(int tier, uint32_t cellIdx, uint32_t& outLen) {
    outLen = 0;
    if (tier < 0 || tier > 2 || cellIdx >= knavDirEntries(tier)) return nullptr;
    if (sCellBitmapReady[tier] && !cellBitmapGet(tier, cellIdx)) return nullptr;

    int32_t key = (int32_t)(((uint32_t)tier << 20) | cellIdx);
    sCacheClock++;
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].key == key) {
            sCellCache[i].lastUsed = sCacheClock;
            outLen = sCellCache[i].len;
            return sCellCache[i].data;
        }
    }

    uint32_t baseDir = dirOffsetForTier(tier);
    if (baseDir == 0 || !sMapRd.valid) return nullptr;

    CellDirEntry de{};
    if (!sMapRd.readAt((uint64_t)baseDir + (uint64_t)cellIdx * sizeof(CellDirEntry), &de, sizeof(de))) return nullptr;

    int slot = cacheVictim();
    cacheEvict(slot);

    if (de.length == 0 || de.offset == 0) {
        // remember emptiness so the directory is not re-read every frame
        sCellCache[slot].key = key;
        sCellCache[slot].lastUsed = sCacheClock;
        return nullptr;
    }
    if (de.length > KNAV_MAX_CELL_BYTES || (uint64_t)de.offset + de.length > sFileBound) {
        if (!sWarnedBigCell) {
            sWarnedBigCell = true;
            Serial.printf("[MAP] cell tier%d #%lu invalid or > %lu bytes (len %lu) - skipped (builder must cap cells)\n",
                          tier, (unsigned long)cellIdx, (unsigned long)KNAV_MAX_CELL_BYTES, (unsigned long)de.length);
        }
        sCellCache[slot].key = key;
        sCellCache[slot].lastUsed = sCacheClock;
        return nullptr;
    }

    // make room within the byte budget
    while (sCacheBytes + de.length > CELL_CACHE_BUDGET) {
        int v = -1;
        for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
            if (i == slot || !sCellCache[i].data) continue;
            if (v < 0 || sCellCache[i].lastUsed < sCellCache[v].lastUsed) v = i;
        }
        if (v < 0) break;
        cacheEvict(v);
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(de.length, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    if (!sMapRd.readAt(de.offset, buf, de.length)) {
        heap_caps_free(buf);
        return nullptr;
    }
    sCellCache[slot].key = key;
    sCellCache[slot].lastUsed = sCacheClock;
    sCellCache[slot].data = buf;
    sCellCache[slot].len = de.length;
    sCacheBytes += de.length;
    outLen = de.length;
    return buf;
}

// =============================================================================
// Waypoints (NVS)
// =============================================================================
static void loadWaypointsFromStorage() {
    sNumWaypoints = 0;
    Preferences p;
    if (p.begin("kronos_wp", true)) {
        int n = p.getUChar("cnt", 0);
        if (n > MAX_USER_WAYPOINTS) n = MAX_USER_WAYPOINTS;
        for (int i = 0; i < n; i++) {
            char kName[12], kLat[12], kLon[12];
            snprintf(kName, sizeof(kName), "nm%d", i);
            snprintf(kLat, sizeof(kLat), "la%d", i);
            snprintf(kLon, sizeof(kLon), "lo%d", i);
            String nm = p.getString(kName, "POI");
            strncpy(sWaypoints[i].name, nm.c_str(), sizeof(sWaypoints[i].name) - 1);
            sWaypoints[i].name[sizeof(sWaypoints[i].name) - 1] = 0;
            sWaypoints[i].lat = p.getDouble(kLat, 0.0);
            sWaypoints[i].lon = p.getDouble(kLon, 0.0);
            sNumWaypoints++;
        }
        p.end();
    }
    sStat.savedWaypoints = (uint16_t)sNumWaypoints;
}

static void saveWaypointsToNvs() {
    Preferences p;
    if (p.begin("kronos_wp", false)) {
        p.putUChar("cnt", (uint8_t)sNumWaypoints);
        for (int i = 0; i < sNumWaypoints; i++) {
            char kName[12], kLat[12], kLon[12];
            snprintf(kName, sizeof(kName), "nm%d", i);
            snprintf(kLat, sizeof(kLat), "la%d", i);
            snprintf(kLon, sizeof(kLon), "lo%d", i);
            p.putString(kName, sWaypoints[i].name);
            p.putDouble(kLat, sWaypoints[i].lat);
            p.putDouble(kLon, sWaypoints[i].lon);
        }
        p.end();
    }
    sStat.savedWaypoints = (uint16_t)sNumWaypoints;
}

// =============================================================================
// SD card
// =============================================================================
// Plain write/read-back test. No raw SPI, no CS manipulation, no CSD writes:
// a failing test only means "not writable" for us.
static bool testSdWriteCapability() {
    IoLock io;
    const char* tmp = "/kronos_rw.tmp";
    File f = SD.open(tmp, FILE_WRITE, true);
    if (!f) return false;
    size_t w = f.write((const uint8_t*)"WATCHOS", 7);
    f.flush();
    f.close();
    if (w != 7) { SD.remove(tmp); return false; }
    File r = SD.open(tmp, FILE_READ);
    if (!r) { SD.remove(tmp); return false; }
    char buf[8] = {0};
    size_t nr = r.read((uint8_t*)buf, 7);
    r.close();
    SD.remove(tmp);
    return (nr == 7 && memcmp(buf, "WATCHOS", 7) == 0);
}

static void detectSdLocked() {
    bool ready = false;
    {
        IoLock io;
        ready = (instance.getDeviceProbe() & HW_SD_ONLINE) != 0 && SD.cardType() != CARD_NONE;
        if (!ready && instance.installSD()) ready = true;
    }
    sStat.sdReady = ready;
    if (ready) {
        sStat.sdWritable = testSdWriteCapability();
        if (!sStat.sdWritable) Serial.println("[SD] card is read-only for us (write test failed)");
        IoLock io;
        sStat.sdTotalMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
        // usedBytes() may scan the FAT; it is refreshed in the deferred boot step
    } else {
        sStat.sdWritable = false;
        sStat.sdTotalMB = sStat.sdUsedMB = 0;
    }
}

// =============================================================================
// Map (un)load
// =============================================================================
static void freeGraphBuffers(DiskRouteNode* n, DiskRouteEdge* e) {
    if (n) heap_caps_free(n);
    if (e) heap_caps_free(e);
}

// Releases everything that belongs to the current map generation.
static void unloadMapLocked() {
    sMapGen++;
    sMapRd.close();
    clearCellCache();
    for (int t = 0; t < 3; t++) {
        if (sCellBitmap[t]) heap_caps_free(sCellBitmap[t]);
        sCellBitmap[t] = nullptr;
        sCellBitmapReady[t] = false;
    }
    if (sPlaces) heap_caps_free(sPlaces);
    sPlaces = nullptr; sNumPlaces = 0;
    if (sPois) heap_caps_free(sPois);
    sPois = nullptr; sNumPois = 0;
    if (sPoiCellStart) heap_caps_free(sPoiCellStart);
    sPoiCellStart = nullptr;
    if (sPoiOrder) heap_caps_free(sPoiOrder);
    sPoiOrder = nullptr;
    if (sRoutingActive) {
        // A* is using the graph right now on the background task: hand the
        // buffers over, they are freed when the job finishes. (A second
        // unload during the same job finds sNodes already null.)
        if (!sDropNodes && !sDropEdges) {
            sDropNodes = sNodes; sDropEdges = sEdges;
            sGraphDropPending = true;
        } else {
            freeGraphBuffers(sNodes, sEdges);
        }
    } else {
        freeGraphBuffers(sNodes, sEdges);
    }
    sNodes = nullptr; sEdges = nullptr; sNumNodes = sNumEdges = 0;
    sGraphReady = false;
    sLoaderFinished = false;
    sUseFlashPart = false;
    sFlashPart = nullptr;
    sMapPath[0] = 0;
    sFileBound = 0;
    sHdrIsV3 = false;
    memset(&sHdr, 0, sizeof(sHdr));
    sStat.mapLoaded = false;
    sStat.ultraLoaded = false;
    sStat.graphReady = false;
    sStat.placesReady = false;
    sStat.mapLoadPct = 0;
    sStat.mapPlaces = 0;
    sStat.mapRouteNodes = 0;
    sStat.mapPois = 0;
    sWarnedBigCell = false;
}

static void publishHeaderLocked(const char* src) {
    sStat.mapLoaded = true;
    sStat.ultraLoaded = (sHdr.ultraDirOffset > 0);
    sStat.mapPlaces = sHdr.numPlaces;
    sStat.mapRouteNodes = sHdr.numRouteNodes;
    sStat.mapPois = sHdr.numPois;
    sStat.mapLoadPct = 0;
    Serial.printf("[MAP] %s %s: %lu places, %lu nodes, %lu edges, %lu POIs, ultra=%d, bound=%llu\n",
                  src, sHdrIsV3 ? KNAV_MAGIC_V3 : KNAV_MAGIC_V2,
                  (unsigned long)sHdr.numPlaces, (unsigned long)sHdr.numRouteNodes,
                  (unsigned long)sHdr.numRouteEdges, (unsigned long)sHdr.numPois,
                  (int)sStat.ultraLoaded, (unsigned long long)sFileBound);
}

bool NavEngine::reloadMap() {
    NavLock lock;
    unloadMapLocked();
    loadWaypointsFromStorage();
    detectSdLocked();

    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);

    // 1. SD card candidates
    if (sStat.sdReady) {
        for (const char* path : MAP_SD_CANDIDATES) {
            bool exists;
            {
                IoLock io;
                exists = SD.exists(path);
            }
            if (!exists) continue;
            MapReader rd;
            if (!rd.open(false, nullptr, path, 0)) continue;
            KnavHeader h{};
            bool v3 = false;
            bool ok = readHeader(rd, h, v3);
            uint64_t bound = rd.bound;
            rd.close();
            if (!ok) {
                Serial.printf("[MAP] %s rejected (bad header)\n", path);
                continue;
            }
            sHdr = h;
            sHdrIsV3 = v3;
            sFileBound = v3 ? h.fileSize : bound;
            strncpy(sMapPath, path, sizeof(sMapPath) - 1);
            sMapPath[sizeof(sMapPath) - 1] = 0;
            if (!sMapRd.open(false, nullptr, sMapPath, sFileBound)) {
                Serial.printf("[MAP] cannot open %s for rendering\n", path);
                unloadMapLocked();
                continue;
            }
            publishHeaderLocked(path);
            sMirrorPending = false;
            return true;
        }
    }

    // 2. internal flash partition
    if (part && part->size >= KNAV_HEADER_V2_SIZE) {
        MapReader rd;
        if (rd.open(true, part, nullptr, part->size)) {
            KnavHeader h{};
            bool v3 = false;
            bool ok = readHeader(rd, h, v3);
            rd.close();
            if (ok) {
                sHdr = h;
                sHdrIsV3 = v3;
                sFlashPart = part;
                sUseFlashPart = true;
                sFileBound = v3 ? h.fileSize : part->size;
                if (!sMapRd.open(true, part, nullptr, sFileBound)) {
                    unloadMapLocked();
                } else {
                    publishHeaderLocked("flash");
                    bool sdHasMap = false;
                    if (sStat.sdReady && sStat.sdWritable) {
                        IoLock io;
                        sdHasMap = SD.exists(MAP_SD_PATH_PRIMARY);
                    }
                    sMirrorPending = sStat.sdReady && sStat.sdWritable && !sdHasMap;
                    return true;
                }
            }
        }
    }

    Serial.println("[MAP] no map file - built-in city list only");
    return false;
}

bool NavEngine::getMapCenter(double& lat, double& lon) {
    NavLock lock;
    if (sStat.mapLoaded && sHdrIsV3 && sHdr.bboxMaxLat_e5 > sHdr.bboxMinLat_e5 && sHdr.bboxMaxLon_e5 > sHdr.bboxMinLon_e5) {
        lat = (sHdr.bboxMinLat_e5 + sHdr.bboxMaxLat_e5) * 0.5e-5;
        lon = (sHdr.bboxMinLon_e5 + sHdr.bboxMaxLon_e5) * 0.5e-5;
        return true;
    }
    lat = DE_CENTER_LAT;
    lon = DE_CENTER_LON;
    return sStat.mapLoaded;
}

// =============================================================================
// Background loader: directories -> places -> POIs -> graph, 16 KB per tick,
// SD reads without the engine mutex. Private state is touched only by the
// background task; the command (generation) is read under the engine mutex.
// =============================================================================
enum LoadStage : uint8_t {
    LS_IDLE = 0, LS_DIR0, LS_DIR1, LS_DIR2, LS_PLACES, LS_POIS, LS_NODES, LS_EDGES, LS_DONE
};
static constexpr uint32_t LOAD_CHUNK = 16 * 1024;

struct LoaderState {
    uint32_t activeGen = 0;      // generation the private state belongs to
    LoadStage stage = LS_IDLE;
    MapReader rd;
    KnavHeader hdr{};
    uint64_t bound = 0;
    bool useFlash = false;
    const esp_partition_t* part = nullptr;
    char path[48] = "";
    // per stage
    uint8_t* buf = nullptr;      // destination table (owned until published)
    uint64_t stageOff = 0;       // absolute file offset of the stage data
    uint32_t stageBytes = 0;     // bytes to read in this stage
    uint32_t stageDone = 0;
    uint64_t totalBytes = 1;     // for progress
    uint64_t totalDone = 0;
    uint8_t* chunk = nullptr;    // 16 KB scratch for directory scanning
    uint32_t dirValid = 0, dirInvalid = 0;
};
static LoaderState sLd;

static void loaderReset() {
    if (sLd.buf) heap_caps_free(sLd.buf);
    sLd.buf = nullptr;
    if (sLd.chunk) heap_caps_free(sLd.chunk);
    sLd.chunk = nullptr;
    sLd.rd.close();
    sLd.stage = LS_IDLE;
    sLd.stageBytes = sLd.stageDone = 0;
    sLd.totalDone = 0;
}

// Marks the current generation as fully loaded (engine mutex held).
static void loaderFinishLocked() {
    sLd.stage = LS_DONE;
    sLd.rd.close();
    if (sLd.chunk) { heap_caps_free(sLd.chunk); sLd.chunk = nullptr; }
    if (sLd.activeGen != sMapGen) return;
    if (!sGraphReady && sNodes) {
        // nodes arrived but edges did not (memory / read error): drop the half graph
        freeGraphBuffers(sNodes, sEdges);
        sNodes = nullptr; sEdges = nullptr; sNumNodes = sNumEdges = 0;
    }
    sLoaderFinished = true;
    sStat.mapLoadPct = 100;
}

static uint64_t stageDataBytes(const KnavHeader& h, LoadStage s) {
    switch (s) {
        case LS_DIR0:   return h.coarseDirOffset ? (uint64_t)KNAV_COARSE_CELLS * sizeof(CellDirEntry) : 0;
        case LS_DIR1:   return h.fineDirOffset   ? (uint64_t)KNAV_FINE_CELLS   * sizeof(CellDirEntry) : 0;
        case LS_DIR2:   return h.ultraDirOffset  ? (uint64_t)KNAV_ULTRA_CELLS  * sizeof(CellDirEntry) : 0;
        case LS_PLACES: return (uint64_t)h.numPlaces * sizeof(DiskPlace);
        case LS_POIS:   return (uint64_t)h.numPois * sizeof(DiskPoi);
        case LS_NODES:  return (uint64_t)h.numRouteNodes * sizeof(DiskRouteNode);
        case LS_EDGES:  return (uint64_t)h.numRouteEdges * sizeof(DiskRouteEdge);
        default:        return 0;
    }
}
static uint64_t stageOffset(const KnavHeader& h, LoadStage s) {
    switch (s) {
        case LS_DIR0:   return h.coarseDirOffset;
        case LS_DIR1:   return h.fineDirOffset;
        case LS_DIR2:   return h.ultraDirOffset;
        case LS_PLACES: return h.placesOffset;
        case LS_POIS:   return h.poisOffset;
        case LS_NODES:  return h.routeNodesOffset;
        case LS_EDGES:  return h.routeEdgesOffset;
        default:        return 0;
    }
}

// Begins a stage: allocates the destination. Returns false if the stage is skipped.
static bool loaderBeginStage(LoadStage s) {
    sLd.stage = s;
    sLd.stageOff = stageOffset(sLd.hdr, s);
    uint64_t bytes = stageDataBytes(sLd.hdr, s);
    sLd.stageBytes = (uint32_t)bytes;
    sLd.stageDone = 0;
    if (bytes == 0) return false;
    if (s <= LS_DIR2) {
        if (!sLd.chunk) sLd.chunk = (uint8_t*)heap_caps_malloc(LOAD_CHUNK, MALLOC_CAP_SPIRAM);
        int tier = (int)s - (int)LS_DIR0;
        size_t bmBytes = (knavDirEntries(tier) + 7) / 8;
        sLd.buf = (uint8_t*)heap_caps_malloc(bmBytes, MALLOC_CAP_SPIRAM);
        if (!sLd.buf || !sLd.chunk) { Serial.println("[MAP] loader: no memory for directory bitmap"); return false; }
        memset(sLd.buf, 0, bmBytes);
        sLd.dirValid = sLd.dirInvalid = 0;
        return true;
    }
    if (bytes > 0xFFFFFFFFull) return false;
    sLd.buf = (uint8_t*)psramAlloc((size_t)bytes);
    if (!sLd.buf) {
        Serial.printf("[MAP] loader: not enough PSRAM for stage %d (%lu KB, free %lu KB) - skipped\n",
                      (int)s, (unsigned long)(bytes / 1024),
                      (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return false;
    }
    return true;
}

// Called under the engine mutex when a table is complete.
static void loaderPublishLocked(LoadStage s) {
    if (sLd.activeGen != sMapGen || !sStat.mapLoaded) {
        if (sLd.buf) heap_caps_free(sLd.buf);
        sLd.buf = nullptr;
        return;
    }
    switch (s) {
        case LS_DIR0: case LS_DIR1: case LS_DIR2: {
            int tier = (int)s - (int)LS_DIR0;
            if (sCellBitmap[tier]) heap_caps_free(sCellBitmap[tier]);
            sCellBitmap[tier] = sLd.buf;
            sCellBitmapReady[tier] = true;
            if (sLd.dirInvalid) {
                Serial.printf("[MAP] tier%d directory: %lu cells, %lu invalid entries ignored\n",
                              tier, (unsigned long)sLd.dirValid, (unsigned long)sLd.dirInvalid);
            }
            break;
        }
        case LS_PLACES:
            sPlaces = (DiskPlace*)sLd.buf;
            sNumPlaces = sLd.hdr.numPlaces;
            for (uint32_t i = 0; i < sNumPlaces; i++) sPlaces[i].name[sizeof(sPlaces[i].name) - 1] = 0;
            sStat.placesReady = true;
            Serial.printf("[MAP] place table resident: %lu entries\n", (unsigned long)sNumPlaces);
            break;
        case LS_POIS: {
            DiskPoi* pois = (DiskPoi*)sLd.buf;
            uint32_t n = sLd.hdr.numPois;
            uint32_t* cellStart = (uint32_t*)psramAlloc((KNAV_ULTRA_CELLS + 1) * sizeof(uint32_t));
            uint32_t* order = (uint32_t*)psramAlloc((size_t)n * sizeof(uint32_t));
            if (!cellStart || !order) {
                if (cellStart) heap_caps_free(cellStart);
                if (order) heap_caps_free(order);
                heap_caps_free(pois);
                Serial.println("[MAP] POI index: no memory - POIs disabled");
                break;
            }
            // CSR by ultra cell (POIs outside the ultra box are dropped)
            const double latSpan = KNAV_STG_MAX_LAT - KNAV_STG_MIN_LAT;
            const double lonSpan = KNAV_STG_MAX_LON - KNAV_STG_MIN_LON;
            auto cellOf = [&](const DiskPoi& p) -> int32_t {
                double la = p.lat_e5 * 1e-5, lo = p.lon_e5 * 1e-5;
                if (la < KNAV_STG_MIN_LAT || la >= KNAV_STG_MAX_LAT || lo < KNAV_STG_MIN_LON || lo >= KNAV_STG_MAX_LON) return -1;
                int cy = (int)((la - KNAV_STG_MIN_LAT) / latSpan * KNAV_ULTRA_NY);
                int cx = (int)((lo - KNAV_STG_MIN_LON) / lonSpan * KNAV_ULTRA_NX);
                cy = std::clamp(cy, 0, KNAV_ULTRA_NY - 1);
                cx = std::clamp(cx, 0, KNAV_ULTRA_NX - 1);
                return cy * KNAV_ULTRA_NX + cx;
            };
            memset(cellStart, 0, (KNAV_ULTRA_CELLS + 1) * sizeof(uint32_t));
            for (uint32_t i = 0; i < n; i++) {
                pois[i].name[sizeof(pois[i].name) - 1] = 0;
                int32_t c = cellOf(pois[i]);
                if (c >= 0) cellStart[c + 1]++;
            }
            for (uint32_t c = 0; c < KNAV_ULTRA_CELLS; c++) cellStart[c + 1] += cellStart[c];
            // fill using a running cursor per cell (reuse a temporary copy of starts)
            uint32_t* cursor = (uint32_t*)psramAlloc(KNAV_ULTRA_CELLS * sizeof(uint32_t));
            if (!cursor) {
                heap_caps_free(cellStart); heap_caps_free(order); heap_caps_free(pois);
                Serial.println("[MAP] POI index: no memory - POIs disabled");
                break;
            }
            memcpy(cursor, cellStart, KNAV_ULTRA_CELLS * sizeof(uint32_t));
            for (uint32_t i = 0; i < n; i++) {
                int32_t c = cellOf(pois[i]);
                if (c >= 0) order[cursor[c]++] = i;
            }
            heap_caps_free(cursor);
            sPois = pois; sNumPois = n; sPoiCellStart = cellStart; sPoiOrder = order;
            Serial.printf("[MAP] POI index resident: %lu POIs in %lu cells\n",
                          (unsigned long)cellStart[KNAV_ULTRA_CELLS], (unsigned long)KNAV_ULTRA_CELLS);
            break;
        }
        case LS_NODES:
            sNodes = (DiskRouteNode*)sLd.buf;
            sNumNodes = sLd.hdr.numRouteNodes;
            break;
        case LS_EDGES: {
            sEdges = (DiskRouteEdge*)sLd.buf;
            sNumEdges = sLd.hdr.numRouteEdges;
            if (!sNodes || sNumNodes == 0 || sNumEdges == 0) {
                heap_caps_free(sEdges); sEdges = nullptr; sNumEdges = 0;
                break;
            }
            // validate once: node edge ranges and edge targets
            uint32_t badNodes = 0, badEdges = 0;
            for (uint32_t i = 0; i < sNumNodes; i++) {
                uint64_t end = (uint64_t)sNodes[i].firstEdge + sNodes[i].edgeCount;
                if (end > sNumEdges) { sNodes[i].edgeCount = 0; badNodes++; }
            }
            for (uint32_t i = 0; i < sNumEdges; i++) {
                if (sEdges[i].toNode >= sNumNodes) { sEdges[i].flags = 0; sEdges[i].toNode = 0; badEdges++; }
                if (sEdges[i].dist_hm == 0) sEdges[i].dist_hm = 1;
            }
            if (badNodes || badEdges) {
                Serial.printf("[MAP] graph: %lu nodes / %lu edges with invalid references disabled\n",
                              (unsigned long)badNodes, (unsigned long)badEdges);
            }
            sGraphReady = true;
            sStat.graphReady = true;
            Serial.printf("[MAP] routing graph resident: %lu nodes, %lu edges (%lu KB)\n",
                          (unsigned long)sNumNodes, (unsigned long)sNumEdges,
                          (unsigned long)((sNumNodes * sizeof(DiskRouteNode) + sNumEdges * sizeof(DiskRouteEdge)) / 1024));
            break;
        }
        default: break;
    }
    sLd.buf = nullptr;
}

static LoadStage nextStage(LoadStage s) {
    return (s >= LS_EDGES) ? LS_DONE : (LoadStage)((int)s + 1);
}

// One loader step per background tick. Returns true if work was done.
static bool loaderTick() {
    // --- sync with the command state ---
    uint32_t gen;
    bool mapLoaded;
    {
        NavLock lock;
        gen = sMapGen;
        mapLoaded = sStat.mapLoaded;
        if (sLd.activeGen != gen) {
            loaderReset();
            sLd.activeGen = gen;
            if (!mapLoaded) { sLd.stage = LS_DONE; return false; }
            sLd.hdr = sHdr;
            sLd.bound = sFileBound;
            sLd.useFlash = sUseFlashPart;
            sLd.part = sFlashPart;
            strncpy(sLd.path, sMapPath, sizeof(sLd.path) - 1);
            sLd.path[sizeof(sLd.path) - 1] = 0;
            sLd.totalBytes = 1;
            for (int s = LS_DIR0; s <= LS_EDGES; s++) sLd.totalBytes += stageDataBytes(sLd.hdr, (LoadStage)s);
            sLd.totalDone = 0;
            sLd.stage = LS_IDLE;
        }
        if (sLd.stage == LS_DONE || !mapLoaded) {
            if (!sLoaderFinished && mapLoaded) loaderFinishLocked();
            return false;
        }
    }

    if (sLd.stage == LS_IDLE) {
        if (!sLd.rd.open(sLd.useFlash, sLd.part, sLd.path, sLd.bound)) {
            Serial.println("[MAP] loader: cannot open map source");
            NavLock lock;
            loaderFinishLocked();
            return false;
        }
        LoadStage s = LS_DIR0;
        while (s != LS_DONE && !loaderBeginStage(s)) {
            if (sLd.buf) { heap_caps_free(sLd.buf); sLd.buf = nullptr; }
            s = nextStage(s);
        }
        if (s == LS_DONE) { NavLock lock; loaderFinishLocked(); }
        return true;
    }

    // --- read one chunk (no engine mutex held) ---
    uint32_t want = std::min<uint32_t>(LOAD_CHUNK, sLd.stageBytes - sLd.stageDone);
    bool ok;
    if (sLd.stage <= LS_DIR2) {
        ok = sLd.rd.readAt(sLd.stageOff + sLd.stageDone, sLd.chunk, want);
        if (ok) {
            int tier = (int)sLd.stage - (int)LS_DIR0;
            uint32_t firstIdx = sLd.stageDone / sizeof(CellDirEntry);
            uint32_t n = want / sizeof(CellDirEntry);
            const CellDirEntry* de = (const CellDirEntry*)sLd.chunk;
            for (uint32_t i = 0; i < n; i++) {
                if (de[i].length == 0 || de[i].offset == 0) continue;
                bool valid = de[i].length <= KNAV_MAX_CELL_BYTES && (uint64_t)de[i].offset + de[i].length <= sLd.bound;
                if (valid) { cellBitmapSet(sLd.buf, firstIdx + i); sLd.dirValid++; }
                else sLd.dirInvalid++;
            }
            (void)tier;
        }
    } else {
        ok = sLd.rd.readAt(sLd.stageOff + sLd.stageDone, sLd.buf + sLd.stageDone, want);
    }
    if (!ok) {
        Serial.printf("[MAP] loader: read failed in stage %d at %lu - map tables incomplete\n",
                      (int)sLd.stage, (unsigned long)sLd.stageDone);
        if (sLd.buf) { heap_caps_free(sLd.buf); sLd.buf = nullptr; }
        NavLock lock;
        loaderFinishLocked();
        return true;
    }
    sLd.stageDone += want;
    sLd.totalDone += want;

    {
        NavLock lock;
        if (sLd.activeGen == sMapGen) {
            sStat.mapLoadPct = (uint8_t)std::min<uint64_t>(99, sLd.totalDone * 100 / sLd.totalBytes);
        }
        if (sLd.stageDone >= sLd.stageBytes) {
            LoadStage done = sLd.stage;
            loaderPublishLocked(done);
            LoadStage s = nextStage(done);
            while (s != LS_DONE && !loaderBeginStage(s)) {
                if (sLd.buf) { heap_caps_free(sLd.buf); sLd.buf = nullptr; }
                s = nextStage(s);
            }
            if (s == LS_DONE) loaderFinishLocked();
        }
    }
    return true;
}

// =============================================================================
// Deferred boot work (background task): GPX repair, SD usage, flash->SD mirror
// =============================================================================
static bool fileEndsWithGpxClose(File& f) {
    size_t sz = f.size();
    if (sz < 7) return false;
    size_t tailLen = std::min<size_t>(sz, 24);
    if (!f.seek((uint32_t)(sz - tailLen))) return false;
    char tail[25] = {0};
    size_t n = f.read((uint8_t*)tail, tailLen);
    tail[n] = 0;
    return strstr(tail, "</gpx>") != nullptr;
}

static void gpxRepairUnterminated() {
    IoLock io;
    if (!SD.exists(GPX_DIR)) return;
    File dir = SD.open(GPX_DIR);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    int repaired = 0;
    for (int n = 0; n < 128; n++) {
        File f = dir.openNextFile();
        if (!f) break;
        char path[48];
        strncpy(path, f.path(), sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        bool isGpx = !f.isDirectory() && strlen(path) > 4 && strcmp(path + strlen(path) - 4, ".gpx") == 0;
        bool needsClose = isGpx && f.size() > 0 && !fileEndsWithGpxClose(f);
        f.close();
        if (needsClose) {
            File a = SD.open(path, FILE_APPEND);
            if (a) {
                a.print("</trkseg></trk></gpx>\n");
                a.close();
                repaired++;
            }
        }
    }
    dir.close();
    if (repaired) Serial.printf("[NAV] closed %d unterminated GPX file(s)\n", repaired);
}

struct MirrorJob {
    bool active = false;
    File out;
    uint64_t total = 0;
    uint64_t done = 0;
    uint32_t gen = 0;
    uint8_t* buf = nullptr;
};
static MirrorJob sMirror;

// Real data end of a map (V3: header fileSize; V2: computed from the tables
// and directories). Runs on the background task without the engine mutex.
static uint64_t computeDataEnd(MapReader& rd, const KnavHeader& h, bool v3) {
    if (v3 && h.fileSize) return h.fileSize;
    uint64_t end = KNAV_HEADER_V2_SIZE;
    auto upd = [&](uint64_t off, uint64_t count, uint64_t sz) {
        if (count && off) end = std::max(end, off + count * sz);
    };
    upd(h.placesOffset, h.numPlaces, sizeof(DiskPlace));
    upd(h.routeNodesOffset, h.numRouteNodes, sizeof(DiskRouteNode));
    upd(h.routeEdgesOffset, h.numRouteEdges, sizeof(DiskRouteEdge));
    upd(h.poisOffset, h.numPois, sizeof(DiskPoi));
    const uint32_t dirs[3] = {h.coarseDirOffset, h.fineDirOffset, h.ultraDirOffset};
    CellDirEntry chunk[256];
    for (int t = 0; t < 3; t++) {
        if (!dirs[t]) continue;
        uint32_t n = (uint32_t)knavDirEntries(t);
        end = std::max<uint64_t>(end, (uint64_t)dirs[t] + (uint64_t)n * sizeof(CellDirEntry));
        for (uint32_t i = 0; i < n; i += 256) {
            uint32_t cnt = std::min<uint32_t>(256, n - i);
            if (!rd.readAt((uint64_t)dirs[t] + (uint64_t)i * sizeof(CellDirEntry), chunk, cnt * sizeof(CellDirEntry))) return 0;
            for (uint32_t k = 0; k < cnt; k++) {
                if (chunk[k].length && chunk[k].offset) {
                    uint64_t e = (uint64_t)chunk[k].offset + chunk[k].length;
                    if (e <= rd.bound) end = std::max(end, e);
                }
            }
        }
    }
    return end;
}

static void mirrorStart() {
    KnavHeader h;
    bool v3;
    const esp_partition_t* part;
    uint64_t bound;
    uint32_t gen;
    {
        NavLock lock;
        if (!sMirrorPending || !sUseFlashPart || !sFlashPart || !sStat.sdReady || !sStat.sdWritable) { sMirrorPending = false; return; }
        h = sHdr; v3 = sHdrIsV3; part = sFlashPart; bound = sFileBound; gen = sMapGen;
        sMirrorPending = false;
    }
    MapReader rd;
    if (!rd.open(true, part, nullptr, bound)) return;
    uint64_t total = computeDataEnd(rd, h, v3);
    rd.close();
    if (total < KNAV_HEADER_V2_SIZE || total > part->size) {
        Serial.println("[SD] mirror: cannot determine map size, skipped");
        return;
    }
    sMirror.buf = (uint8_t*)heap_caps_malloc(LOAD_CHUNK, MALLOC_CAP_SPIRAM);
    if (!sMirror.buf) return;
    {
        IoLock io;
        SD.mkdir("/kronos");
        if (SD.exists(MAP_SD_TMP_PATH)) SD.remove(MAP_SD_TMP_PATH);
        sMirror.out = SD.open(MAP_SD_TMP_PATH, FILE_WRITE, true);
    }
    if (!sMirror.out) {
        heap_caps_free(sMirror.buf); sMirror.buf = nullptr;
        Serial.println("[SD] mirror: cannot create temp file");
        return;
    }
    sMirror.total = total;
    sMirror.done = 0;
    sMirror.gen = gen;
    sMirror.active = true;
    Serial.printf("[SD] mirroring flash map to SD (%lu KB, chunked in background)...\n", (unsigned long)(total / 1024));
}

static void mirrorAbort(const char* why) {
    {
        IoLock io;
        if (sMirror.out) sMirror.out.close();
        if (SD.exists(MAP_SD_TMP_PATH)) SD.remove(MAP_SD_TMP_PATH);
    }
    if (sMirror.buf) heap_caps_free(sMirror.buf);
    sMirror.buf = nullptr;
    sMirror.active = false;
    Serial.printf("[SD] mirror aborted (%s)\n", why);
}

static void mirrorTick() {
    if (!sMirror.active) return;
    const esp_partition_t* part;
    {
        NavLock lock;
        part = (sMirror.gen == sMapGen) ? sFlashPart : nullptr;
    }
    if (!part) { mirrorAbort("map changed"); return; }
    uint32_t chunk = (uint32_t)std::min<uint64_t>(LOAD_CHUNK, sMirror.total - sMirror.done);
    if (esp_partition_read(part, (size_t)sMirror.done, sMirror.buf, chunk) != ESP_OK) {
        mirrorAbort("flash read"); return;
    }
    size_t w;
    {
        IoLock io;
        w = sMirror.out.write(sMirror.buf, chunk);
    }
    if (w != chunk) { mirrorAbort("SD write failed"); return; }
    sMirror.done += chunk;
    if (sMirror.done >= sMirror.total) {
        {
            IoLock io;
            sMirror.out.flush();
            sMirror.out.close();
        }
        heap_caps_free(sMirror.buf); sMirror.buf = nullptr;
        sMirror.active = false;
        // publish atomically: nobody may have started an upload meanwhile
        NavLock lock;
        if (sMirror.gen != sMapGen || sUploadActive) {
            IoLock io;
            SD.remove(MAP_SD_TMP_PATH);
            Serial.println("[SD] mirror discarded (map changed while copying)");
            return;
        }
        IoLock io;
        if (SD.exists(MAP_SD_PATH_PRIMARY)) SD.remove(MAP_SD_PATH_PRIMARY);
        if (SD.rename(MAP_SD_TMP_PATH, MAP_SD_PATH_PRIMARY)) {
            Serial.printf("[SD] mirror complete: %lu KB written to %s\n", (unsigned long)(sMirror.total / 1024), MAP_SD_PATH_PRIMARY);
        } else {
            SD.remove(MAP_SD_TMP_PATH);
            Serial.println("[SD] mirror: rename failed");
        }
    }
}

static void deferredBootStep() {
    bool sdReady;
    {
        NavLock lock;
        sdReady = sStat.sdReady;
    }
    if (sdReady) {
        gpxRepairUnterminated();
        uint32_t usedMB;
        {
            IoLock io;
            usedMB = (uint32_t)(SD.usedBytes() / (1024ULL * 1024ULL));
        }
        NavLock lock;
        sStat.sdUsedMB = usedMB;
    }
}

// =============================================================================
// Position
// =============================================================================
static void updateCurrentPositionLocked() {
    sStat.posAgeSec = 0;
    if (GpsManager::hasFix()) {
        sStat.posSource = POS_SRC_GPS_LIVE;
        snprintf(sStat.posSourceTag, sizeof(sStat.posSourceTag), "3D GPS (%u SAT)", (unsigned)GpsManager::satellites());
        sStat.curLat = GpsManager::latitude();
        sStat.curLon = GpsManager::longitude();
        sStat.headingDeg = GpsManager::courseDeg();
        return;
    }
    if (GpsManager::hasLastFix()) {
        sStat.posSource = POS_SRC_SAVED_FIX;
        sStat.curLat = GpsManager::latitude();
        sStat.curLon = GpsManager::longitude();
        time_t fixT = GpsManager::lastFixEpoch();
        time_t nowT = time(NULL);
        if (fixT > 0 && timeIsPlausible() && nowT > fixT) {
            sStat.posAgeSec = (uint32_t)(nowT - fixT);
            char age[8];
            fmtAge(age, sizeof(age), sStat.posAgeSec);
            snprintf(sStat.posSourceTag, sizeof(sStat.posSourceTag), "GPS GESP. (%s)", age);
        } else {
            strncpy(sStat.posSourceTag, "GPS GESPEICHERT", sizeof(sStat.posSourceTag));
        }
        return;
    }
    double mLat = 0, mLon = 0;
    char meshName[20] = {0};
    uint32_t ageSec = 0;
    if (RadioLoRa::getMeshFallbackPosition(mLat, mLon, meshName, sizeof(meshName), &ageSec)) {
        sStat.posSource = POS_SRC_LORA_MESH;
        sStat.posAgeSec = ageSec;
        char age[8];
        fmtAge(age, sizeof(age), ageSec);
        snprintf(sStat.posSourceTag, sizeof(sStat.posSourceTag), "MESH %.10s (%s)", meshName, age);
        sStat.curLat = mLat;
        sStat.curLon = mLon;
        return;
    }
    sStat.posSource = POS_SRC_DEFAULT;
    strncpy(sStat.posSourceTag, "KEINE POSITION", sizeof(sStat.posSourceTag));
    sStat.curLat = 0.0;
    sStat.curLon = 0.0;
    sStat.headingDeg = 0.0f;
}

static inline bool havePositionLocked() { return sStat.posSource != POS_SRC_DEFAULT; }

// =============================================================================
// Routing
// =============================================================================
static float modeSpeedKmh(NavMode m) { return (m == NAV_MODE_PED) ? 5.0f : 70.0f; }

static void setStraightLineRouteLocked(const char* cmd) {
    sRoute.clear();
    sRoute.push_back({(int32_t)llround(sStat.curLat * 1e5), (int32_t)llround(sStat.curLon * 1e5)});
    sRoute.push_back({(int32_t)llround(sStat.destLat * 1e5), (int32_t)llround(sStat.destLon * 1e5)});
    sStat.routeStraightLine = true;
    sStat.totalDistKm = (float)haversineKm(sStat.curLat, sStat.curLon, sStat.destLat, sStat.destLon);
    setTurn(cmd, ICO_ROUTE);
}

static void queueRouteRequestLocked() {
    sReq.pending = true;
    sReq.serial = ++sReqSerial;
    sReq.sLat = sStat.curLat; sReq.sLon = sStat.curLon;
    sReq.dLat = sStat.destLat; sReq.dLon = sStat.destLon;
    sReq.mode = sStat.mode;
    sStat.routingBusy = true;
    sStat.routeFailed = false;
    setStraightLineRouteLocked("BERECHNE ROUTE...");
}

static void updateTurnByTurnLocked() {
    if (!sStat.activeRoute) return;

    if (sStat.mode == NAV_MODE_MESH) {
        uint8_t np = RadioLoRa::getPeers(sPeerScratch, 16);
        for (uint8_t i = 0; i < np; i++) {
            if (sPeerScratch[i].hasPos && strcmp(sPeerScratch[i].name, sStat.destName) == 0) {
                sStat.destLat = sPeerScratch[i].lat;
                sStat.destLon = sPeerScratch[i].lon;
                break;
            }
        }
    }

    if (!havePositionLocked()) {
        sStat.remDistKm = 0;
        sStat.nextTurnDistM = 0;
        sStat.etaMinutes = 0;
        setTurn("KEINE POSITION", ICO_LOCATION_SEARCHING);
        return;
    }

    // straight-line modes keep the first point glued to the current position
    if (sStat.routeStraightLine || sStat.mode == NAV_MODE_MESH) {
        if (sRoute.size() != 2) {
            sRoute.clear();
            sRoute.push_back({0, 0});
            sRoute.push_back({0, 0});
        }
        sRoute[0] = {(int32_t)llround(sStat.curLat * 1e5), (int32_t)llround(sStat.curLon * 1e5)};
        sRoute[1] = {(int32_t)llround(sStat.destLat * 1e5), (int32_t)llround(sStat.destLon * 1e5)};
        double d = haversineKm(sStat.curLat, sStat.curLon, sStat.destLat, sStat.destLon);
        sStat.remDistKm = (float)d;
        sStat.bearingDeg = calcBearingDeg(sStat.curLat, sStat.curLon, sStat.destLat, sStat.destLon);
        sStat.nextTurnDistM = (uint32_t)(d * 1000.0);
        if (d < 0.05) {
            setTurn("ZIEL ERREICHT", ICO_FLAG);
            sStat.etaMinutes = 0;
            return;
        }
        if (!sStat.routingBusy) {
            setTurn(sStat.mode == NAV_MODE_MESH ? "LUFTLINIE (MESH)" : "LUFTLINIE", ICO_ROUTE);
        }
        float speedKmh = modeSpeedKmh(sStat.mode);
        if (GpsManager::hasFix() && GpsManager::speedKmh() > 2.5f) speedKmh = GpsManager::speedKmh();
        sStat.etaMinutes = (uint16_t)std::min(65535.0f, std::max(1.0f, (float)(d / speedKmh * 60.0)));
        return;
    }

    if (sRoute.size() < 2) return;

    int bestIdx = 0;
    double bestDist = 1e9;
    for (size_t i = 0; i < sRoute.size(); i++) {
        double d = haversineKm(sStat.curLat, sStat.curLon, sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5);
        if (d < bestDist) { bestDist = d; bestIdx = (int)i; }
    }

    double remKm = bestDist;
    for (size_t i = bestIdx; i + 1 < sRoute.size(); i++) {
        remKm += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                             sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
    }
    sStat.remDistKm = (float)remKm;
    sStat.bearingDeg = calcBearingDeg(sStat.curLat, sStat.curLon, sStat.destLat, sStat.destLon);

    if (remKm < 0.05) {
        setTurn("ZIEL ERREICHT", ICO_FLAG);
        sStat.nextTurnDistM = 0;
        sStat.etaMinutes = 0;
        return;
    }

    size_t nextI = std::min(sRoute.size() - 1, (size_t)bestIdx + 1);
    double distToTurnKm = haversineKm(sStat.curLat, sStat.curLon, sRoute[nextI].lat_e5 * 1e-5, sRoute[nextI].lon_e5 * 1e-5);
    float turnDelta = 0.0f;
    bool foundTurn = false;
    for (size_t i = (size_t)bestIdx + 1; i + 1 < sRoute.size(); i++) {
        float b1 = calcBearingDeg(sRoute[i - 1].lat_e5 * 1e-5, sRoute[i - 1].lon_e5 * 1e-5,
                                  sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5);
        float b2 = calcBearingDeg(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                                  sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
        float diff = fmodf(b2 - b1 + 540.0f, 360.0f) - 180.0f;
        if (fabsf(diff) >= 28.0f) { turnDelta = diff; foundTurn = true; break; }
        distToTurnKm += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                                    sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
    }

    sStat.nextTurnDistM = (uint32_t)(distToTurnKm * 1000.0);
    if (!foundTurn)             setTurn("DEM VERLAUF FOLGEN", ICO_STRAIGHT);
    else if (turnDelta > 55.0f) setTurn("RECHTS ABBIEGEN", ICO_TURN_RIGHT);
    else if (turnDelta > 25.0f) setTurn("HALBRECHTS HALTEN", ICO_TURN_SLIGHT_RIGHT);
    else if (turnDelta < -55.0f) setTurn("LINKS ABBIEGEN", ICO_TURN_LEFT);
    else                        setTurn("HALBLINKS HALTEN", ICO_TURN_SLIGHT_LEFT);

    float speedKmh = (sStat.mode == NAV_MODE_CAR) ? 75.0f : 5.0f;
    if (GpsManager::hasFix() && GpsManager::speedKmh() > 2.5f) speedKmh = GpsManager::speedKmh();
    sStat.etaMinutes = (uint16_t)std::min(65535.0f, std::max(1.0f, (float)(remKm / speedKmh * 60.0)));
}

// --- A* job (background task, graph pointers captured under the mutex) ------
struct RouteJob {
    uint32_t serial = 0, gen = 0;
    double sLat = 0, sLon = 0, dLat = 0, dLon = 0;
    NavMode mode = NAV_MODE_CAR;
    const DiskRouteNode* nodes = nullptr;
    const DiskRouteEdge* edges = nullptr;
    uint32_t numNodes = 0, numEdges = 0;
};

static inline bool edgeUsable(uint8_t flags, bool ped) {
    return ped ? (flags & KNAV_EDGE_PED) != 0 : (flags & KNAV_EDGE_CAR) != 0;
}
static inline uint32_t edgeWeight(const DiskRouteEdge& e, bool ped) {
    uint32_t w = e.dist_hm;
    uint8_t rank = knavEdgeRoadRank(e.flags);
    if (ped) {
        if (rank >= 4) w *= 4;
        else if (rank == 3) w *= 2;
    } else {
        if (rank >= 4) w = (w * 6) / 10;
        else if (rank == 3) w = (w * 8) / 10;
    }
    return w < 1 ? 1 : w;
}

// Nearest graph node (with at least one usable edge) to a coordinate.
static bool nearestNode(const RouteJob& j, double lat, double lon, bool ped, uint32_t& out, double& distKm) {
    int32_t lat5 = (int32_t)llround(lat * 1e5), lon5 = (int32_t)llround(lon * 1e5);
    double lonScale = kmPerDegLon(lat) / 111.32;  // squash longitude so degrees compare fairly
    int64_t best = INT64_MAX;
    uint32_t bestIdx = 0;
    bool found = false;
    for (uint32_t i = 0; i < j.numNodes; i++) {
        const DiskRouteNode& n = j.nodes[i];
        if (n.edgeCount == 0) continue;
        int64_t dy = (int64_t)n.lat_e5 - lat5;
        int64_t dx = (int64_t)((n.lon_e5 - lon5) * lonScale);
        int64_t d = dy * dy + dx * dx;
        if (d >= best) continue;
        bool usable = false;
        uint32_t e0 = n.firstEdge, e1 = n.firstEdge + n.edgeCount;
        if (e1 > j.numEdges) continue;
        for (uint32_t k = e0; k < e1 && !usable; k++) usable = edgeUsable(j.edges[k].flags, ped);
        if (!usable) continue;
        best = d; bestIdx = i; found = true;
    }
    if (!found) return false;
    out = bestIdx;
    distKm = haversineKm(lat, lon, j.nodes[bestIdx].lat_e5 * 1e-5, j.nodes[bestIdx].lon_e5 * 1e-5);
    return true;
}

static bool runAStar(const RouteJob& j, std::vector<NavRoutePoint>& outRoute) {
    if (!j.nodes || !j.edges || j.numNodes == 0 || j.numEdges == 0) return false;
    bool ped = (j.mode == NAV_MODE_PED);

    uint32_t src, dst;
    double dS, dD;
    if (!nearestNode(j, j.sLat, j.sLon, ped, src, dS) || !nearestNode(j, j.dLat, j.dLon, ped, dst, dD)) {
        Serial.println("[NAV] A*: no usable graph node near start/destination");
        return false;
    }
    if (dS > 25.0 || dD > 25.0) {
        Serial.printf("[NAV] A*: start/dest too far from graph (%.1f / %.1f km)\n", dS, dD);
        return false;
    }

    uint32_t* gScore = (uint32_t*)psramAlloc((size_t)j.numNodes * 4);
    uint32_t* parent = (uint32_t*)psramAlloc((size_t)j.numNodes * 4);
    uint8_t* closed = (uint8_t*)psramAlloc(((size_t)j.numNodes + 7) / 8);
    if (!gScore || !parent || !closed) {
        if (gScore) heap_caps_free(gScore);
        if (parent) heap_caps_free(parent);
        if (closed) heap_caps_free(closed);
        Serial.println("[NAV] A*: out of memory");
        return false;
    }
    memset(gScore, 0xFF, (size_t)j.numNodes * 4);
    memset(parent, 0xFF, (size_t)j.numNodes * 4);
    memset(closed, 0, ((size_t)j.numNodes + 7) / 8);

    // Admissible heuristic: true distance in hm scaled by the smallest edge
    // weight factor of the mode (car: motorways cost 0.6x), using the smaller
    // of cos(lat) scaling and the legacy 71.5 km/deg so old files stay safe.
    const double midLat = (j.sLat + j.dLat) * 0.5;
    const double kmLat = 111.32;
    const double kmLon = std::min(kmPerDegLon(midLat), 71.5);
    const double hScale = ped ? 1.0 : 0.6;
    const int32_t dstLat5 = j.nodes[dst].lat_e5, dstLon5 = j.nodes[dst].lon_e5;
    auto heur = [&](uint32_t u) -> uint32_t {
        double dlat = (j.nodes[u].lat_e5 - dstLat5) * 1e-5 * kmLat;
        double dlon = (j.nodes[u].lon_e5 - dstLon5) * 1e-5 * kmLon;
        return (uint32_t)(sqrt(dlat * dlat + dlon * dlon) * 10.0 * hScale);
    };

    using P = std::pair<uint32_t, uint32_t>;   // (f, node)
    std::priority_queue<P, std::vector<P>, std::greater<P>> pq;
    gScore[src] = 0;
    pq.push({heur(src), src});

    uint32_t maxIter = std::min<uint32_t>(j.numNodes * 4u + 10000u, 1500000u);
    bool reached = (src == dst);
    while (!pq.empty() && maxIter-- > 0 && !reached) {
        P top = pq.top();
        pq.pop();
        uint32_t u = top.second;
        if (u >= j.numNodes) continue;
        if (closed[u >> 3] & (1u << (u & 7))) continue;            // already expanded
        if (top.first > gScore[u] + heur(u)) continue;             // stale entry
        closed[u >> 3] |= (uint8_t)(1u << (u & 7));
        if (u == dst) { reached = true; break; }
        const DiskRouteNode& n = j.nodes[u];
        uint32_t e0 = n.firstEdge, e1 = n.firstEdge + n.edgeCount;
        if (e1 > j.numEdges) continue;
        uint32_t gu = gScore[u];
        for (uint32_t k = e0; k < e1; k++) {
            const DiskRouteEdge& e = j.edges[k];
            if (!edgeUsable(e.flags, ped)) continue;
            uint32_t v = e.toNode;
            if (v >= j.numNodes) continue;
            if (closed[v >> 3] & (1u << (v & 7))) continue;
            uint32_t ng = gu + edgeWeight(e, ped);
            if (ng < gScore[v]) {
                gScore[v] = ng;
                parent[v] = u;
                pq.push({ng + heur(v), v});
            }
        }
    }

    bool ok = false;
    if (reached || parent[dst] != 0xFFFFFFFFu) {
        std::vector<NavRoutePoint> rev;
        rev.push_back({(int32_t)llround(j.dLat * 1e5), (int32_t)llround(j.dLon * 1e5)});
        uint32_t cur = dst;
        uint32_t safety = j.numNodes + 2;
        while (cur != 0xFFFFFFFFu && cur < j.numNodes && safety-- > 0) {
            rev.push_back({j.nodes[cur].lat_e5, j.nodes[cur].lon_e5});
            if (cur == src) break;
            cur = parent[cur];
        }
        if (cur == src) {
            rev.push_back({(int32_t)llround(j.sLat * 1e5), (int32_t)llround(j.sLon * 1e5)});
            std::reverse(rev.begin(), rev.end());
            outRoute = std::move(rev);
            ok = true;
        }
    }
    heap_caps_free(gScore);
    heap_caps_free(parent);
    heap_caps_free(closed);
    return ok;
}

static void routingTick() {
    RouteJob job;
    {
        NavLock lock;
        if (!sReq.pending || sRoutingActive) return;
        if (!sStat.activeRoute) { sReq.pending = false; sStat.routingBusy = false; return; }
        if (sReq.mode == NAV_MODE_MESH) { sReq.pending = false; sStat.routingBusy = false; return; }
        if (!sStat.mapLoaded || sHdr.numRouteNodes == 0 || sHdr.numRouteEdges == 0) {
            sReq.pending = false;
            sStat.routingBusy = false;
            sStat.routeFailed = true;
            setStraightLineRouteLocked("LUFTLINIE");
            updateTurnByTurnLocked();
            nav::markDirty(nav::PG_POSITION);
            Serial.println("[NAV] no routing graph in map - straight line");
            return;
        }
        if (!sGraphReady) {
            if (sLoaderFinished) {
                sReq.pending = false;
                sStat.routingBusy = false;
                sStat.routeFailed = true;
                setStraightLineRouteLocked("LUFTLINIE");
                updateTurnByTurnLocked();
                nav::markDirty(nav::PG_POSITION);
                Serial.println("[NAV] routing graph not resident (memory) - straight line");
            }
            return;   // graph still loading: keep the request pending
        }
        job.serial = sReq.serial;
        job.gen = sMapGen;
        job.sLat = sReq.sLat; job.sLon = sReq.sLon; job.dLat = sReq.dLat; job.dLon = sReq.dLon;
        job.mode = sReq.mode;
        job.nodes = sNodes; job.edges = sEdges; job.numNodes = sNumNodes; job.numEdges = sNumEdges;
        sReq.pending = false;
        sRoutingActive = true;
    }

    std::vector<NavRoutePoint> route;
    unsigned long t0 = millis();
    bool ok = runAStar(job, route);
    unsigned long dt = millis() - t0;

    NavLock lock;
    sRoutingActive = false;
    if (sGraphDropPending) {
        // the map was unloaded while we were routing: free the old graph now
        freeGraphBuffers(sDropNodes, sDropEdges);
        sDropNodes = nullptr; sDropEdges = nullptr;
        sGraphDropPending = false;
        ok = false;
    }
    if (job.serial != sReqSerial || job.gen != sMapGen || !sStat.activeRoute) {
        return;   // superseded; a newer request (if any) is still pending
    }
    sStat.routingBusy = false;
    if (ok && route.size() >= 2) {
        sRoute = std::move(route);
        sStat.routeStraightLine = false;
        sStat.routeFailed = false;
        double totKm = 0;
        for (size_t i = 0; i + 1 < sRoute.size(); i++) {
            totKm += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                                 sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5);
        }
        sStat.totalDistKm = (float)totKm;
        Serial.printf("[NAV] route %s: %u points, %.1f km, %lu ms\n",
                      job.mode == NAV_MODE_PED ? "FUSS" : "AUTO", (unsigned)sRoute.size(), totKm, dt);
    } else {
        sStat.routeFailed = true;
        setStraightLineRouteLocked("LUFTLINIE");
        Serial.printf("[NAV] no road route found (%lu ms) - straight line\n", dt);
    }
    updateTurnByTurnLocked();
    nav::markDirty(nav::PG_POSITION);
}

// =============================================================================
// GPX
// =============================================================================
static void gpxWriteCloseLocked() {
    if (!sGpxOpenPath[0]) return;
    if (sStat.sdReady && sStat.sdWritable) {
        IoLock io;
        File f = SD.open(sGpxOpenPath, FILE_APPEND);
        if (f) {
            f.print("</trkseg></trk></gpx>\n");
            f.close();
        }
    }
    sGpxOpenPath[0] = 0;
    sGpxOpenDay[0] = 0;
}

static void gpxLogPointLocked(unsigned long now) {
    if (!sStat.gpxRecording || sStat.posSource != POS_SRC_GPS_LIVE) return;
    if (now - sLastGpxMs < 5000) return;
    sLastGpxMs = now;

    NavRoutePoint pt{(int32_t)llround(sStat.curLat * 1e5), (int32_t)llround(sStat.curLon * 1e5)};
    if (!sGpxTrail.empty() &&
        abs(sGpxTrail.back().lat_e5 - pt.lat_e5) <= 8 &&
        abs(sGpxTrail.back().lon_e5 - pt.lon_e5) <= 8) {
        return;
    }
    if (sGpxTrail.size() < 2000) sGpxTrail.push_back(pt);
    sStat.gpxPoints = (uint32_t)sGpxTrail.size();

    if (!(sStat.sdReady && sStat.sdWritable)) return;

    struct tm ti{};
    RtcManager::getLocalTimeInfo(&ti);
    char day[12];
    snprintf(day, sizeof(day), "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);

    // day rollover: terminate the previous file
    if (sGpxOpenPath[0] && strcmp(sGpxOpenDay, day) != 0) gpxWriteCloseLocked();

    IoLock io;
    if (!SD.exists(GPX_DIR)) SD.mkdir(GPX_DIR);
    bool needHeader = false;
    char gpxPath[40];
    if (sGpxOpenPath[0]) {
        strncpy(gpxPath, sGpxOpenPath, sizeof(gpxPath));
        gpxPath[sizeof(gpxPath) - 1] = 0;
    } else {
        // (re)open: a file that already exists and is terminated must not be
        // appended to; use a time-suffixed sibling instead of rewriting it.
        snprintf(gpxPath, sizeof(gpxPath), "%s/%s.gpx", GPX_DIR, day);
        File probe = SD.open(gpxPath, FILE_READ);
        if (!probe) {
            needHeader = true;
        } else {
            bool terminated = fileEndsWithGpxClose(probe);
            probe.close();
            if (terminated) {
                snprintf(gpxPath, sizeof(gpxPath), "%s/%s_%02d%02d.gpx", GPX_DIR, day, ti.tm_hour, ti.tm_min);
                needHeader = !SD.exists(gpxPath);
            }
        }
    }
    File f = SD.open(gpxPath, FILE_APPEND);
    if (!f) return;
    if (needHeader) {
        f.print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        f.print("<gpx version=\"1.1\" creator=\"WatchOS Ultra\" xmlns=\"http://www.topografix.com/GPX/1/1\"><trk><trkseg>\n");
    }
    char timeTag[40] = "";
    if (timeIsPlausible()) {
        time_t t = time(NULL);
        struct tm g{};
        gmtime_r(&t, &g);
        snprintf(timeTag, sizeof(timeTag), "<time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>",
                 g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    }
    f.printf("<trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele>%s</trkpt>\n",
             sStat.curLat, sStat.curLon, GpsManager::altitudeM(), timeTag);
    f.close();
    strncpy(sGpxOpenPath, gpxPath, sizeof(sGpxOpenPath) - 1);
    sGpxOpenPath[sizeof(sGpxOpenPath) - 1] = 0;
    strncpy(sGpxOpenDay, day, sizeof(sGpxOpenDay) - 1);
    sGpxOpenDay[sizeof(sGpxOpenDay) - 1] = 0;
}

// =============================================================================
// Public: lifecycle
// =============================================================================
void NavEngine::init() {
    NavLock lock;
    if (sInited) return;
    memset(&sStat, 0, sizeof(sStat));
    sStat.mode = NAV_MODE_CAR;
    sStat.posSource = POS_SRC_DEFAULT;
    strncpy(sStat.posSourceTag, "KEINE POSITION", sizeof(sStat.posSourceTag));
    sStat.curLat = 0.0;
    sStat.curLon = 0.0;
    setTurn("BEREIT", ICO_NAVIGATION);
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        sCellCache[i].key = -1;
        sCellCache[i].data = nullptr;
        sCellCache[i].len = 0;
        sCellCache[i].lastUsed = 0;
    }
    sInitMs = millis();
    reloadMap();                       // header + SD detection only; bulk loading happens in loop()
    updateCurrentPositionLocked();
    sInited = true;
}

void NavEngine::loop(unsigned long now) {
    {
        NavLock lock;
        if (!sInited) return;
    }

    // deferred boot work, ~2 s after init so the UI is up first
    if (!sDeferredBootDone && now - sInitMs >= 2000) {
        sDeferredBootDone = true;
        deferredBootStep();
    }

    // one bulk I/O step per tick: table loading first, then the mirror copy
    bool did = loaderTick();
    if (!did && sDeferredBootDone) {
        if (!sMirror.active) {
            bool pending;
            { NavLock lock; pending = sMirrorPending; }
            if (pending) mirrorStart();
        }
        if (sMirror.active) mirrorTick();
    }

    routingTick();

    static unsigned long lastTick = 0;
    if (now - lastTick < 1000) return;
    lastTick = now;

    NavLock lock;
    PosSource before = sStat.posSource;
    updateCurrentPositionLocked();
    if (sStat.activeRoute) {
        if (sRouteWaitingForPos && havePositionLocked() && sStat.mode != NAV_MODE_MESH) {
            sRouteWaitingForPos = false;
            queueRouteRequestLocked();
        }
        updateTurnByTurnLocked();
    }
    if (before != sStat.posSource) nav::markDirty(nav::PG_POSITION);
    gpxLogPointLocked(now);
}

NavStatus NavEngine::status() {
    NavLock lock;
    return sStat;
}

// =============================================================================
// Public: mode & route
// =============================================================================
void NavEngine::setMode(NavMode m) {
    NavLock lock;
    if (sStat.mode == m) return;
    sStat.mode = m;
    if (sStat.activeRoute) {
        sStat.routeFailed = false;
        if (m == NAV_MODE_MESH) {
            sReq.pending = false;
            sStat.routingBusy = false;
            sRouteWaitingForPos = false;
            setStraightLineRouteLocked("LUFTLINIE (MESH)");
        } else if (havePositionLocked()) {
            queueRouteRequestLocked();
        } else {
            sRouteWaitingForPos = true;
            sRoute.clear();
            setTurn("KEINE POSITION", ICO_LOCATION_SEARCHING);
        }
        updateTurnByTurnLocked();
    }
    nav::markDirty(nav::PG_POSITION);
}

NavMode NavEngine::getMode() {
    NavLock lock;
    return sStat.mode;
}

bool NavEngine::isNavigating() {
    NavLock lock;
    return sStat.activeRoute;
}

bool NavEngine::isRoutingBusy() {
    NavLock lock;
    return sStat.routingBusy;
}

void NavEngine::stopNavigation() {
    NavLock lock;
    sStat.activeRoute = false;
    sStat.routingBusy = false;
    sStat.routeStraightLine = false;
    sStat.routeFailed = false;
    sReq.pending = false;
    sRouteWaitingForPos = false;
    sRoute.clear();
    sStat.remDistKm = 0;
    sStat.totalDistKm = 0;
    sStat.nextTurnDistM = 0;
    sStat.etaMinutes = 0;
    setTurn("BEREIT", ICO_NAVIGATION);
    nav::markDirty(nav::PG_POSITION);
}

bool NavEngine::startNavigation(const char* name, double destLat, double destLon) {
    NavLock lock;
    if (!(destLat >= -90.0 && destLat <= 90.0 && destLon >= -180.0 && destLon <= 180.0)) return false;
    updateCurrentPositionLocked();
    strncpy(sStat.destName, (name && name[0]) ? name : "Ziel", sizeof(sStat.destName) - 1);
    sStat.destName[sizeof(sStat.destName) - 1] = 0;
    sStat.destLat = destLat;
    sStat.destLon = destLon;
    sStat.activeRoute = true;
    sStat.routeFailed = false;
    sStat.routeStraightLine = false;
    sStat.routingBusy = false;
    sReq.pending = false;
    sRouteWaitingForPos = false;
    sRoute.clear();

    if (!havePositionLocked()) {
        // keep the destination; route as soon as a position shows up
        sRouteWaitingForPos = (sStat.mode != NAV_MODE_MESH);
        sStat.routeFailed = true;
        sStat.totalDistKm = 0;
        setTurn("KEINE POSITION", ICO_LOCATION_SEARCHING);
        Serial.printf("[NAV] destination %s set, waiting for a position\n", sStat.destName);
        nav::markDirty(nav::PG_POSITION);
        return true;
    }
    if (sStat.mode == NAV_MODE_MESH) {
        setStraightLineRouteLocked("LUFTLINIE (MESH)");
    } else {
        queueRouteRequestLocked();
    }
    updateTurnByTurnLocked();
    nav::markDirty(nav::PG_POSITION);
    return true;
}

bool NavEngine::startBacktrack() {
    NavLock lock;
    if (sGpxTrail.size() < 2) {
        DisplayEngine::showToast("Backtrack", "Keine Brotkrümel vorhanden", 2500, ICO_HISTORY);
        return false;
    }
    updateCurrentPositionLocked();

    std::vector<NavRoutePoint> route;
    route.reserve(sGpxTrail.size());
    for (int i = (int)sGpxTrail.size() - 1; i >= 0; i--) {
        route.push_back(sGpxTrail[i]);
    }

    sRoute = route;
    sStat.activeRoute = true;
    sStat.routeFailed = false;
    sStat.routeStraightLine = false;
    sStat.routingBusy = false;
    sReq.pending = false;
    sRouteWaitingForPos = false;

    sStat.destLat = route.back().lat_e5 * 1e-5;
    sStat.destLon = route.back().lon_e5 * 1e-5;
    strncpy(sStat.destName, "START (BACKTRACK)", sizeof(sStat.destName) - 1);
    sStat.destName[sizeof(sStat.destName) - 1] = 0;

    float totalDist = 0.0f;
    for (size_t i = 0; i + 1 < sRoute.size(); i++) {
        totalDist += haversineKm(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5,
                                 sRoute[i+1].lat_e5 * 1e-5, sRoute[i+1].lon_e5 * 1e-5);
    }
    sStat.totalDistKm = totalDist;
    sStat.remDistKm = totalDist;

    setTurn("ZURÜCK ZUM START", ICO_HISTORY);
    updateTurnByTurnLocked();
    nav::markDirty(nav::PG_POSITION);
    DisplayEngine::showNotification("Backtrack aktiv", "Folge der Brotkrümel-Spur", 3500, ICO_HISTORY);
    Serial.printf("[GPX] Backtrack started: %u points, %.2f km\n", (unsigned)route.size(), totalDist);
    return true;
}

bool NavEngine::loadGpxRoute(const char* filename) {
    if (!filename || !filename[0]) return false;
    {
        NavLock lock;
        if (!sStat.sdReady) return false;
    }
    if (!NavEngine::lockSd(500)) return false;

    char fullPath[64];
    if (filename[0] == '/') {
        strncpy(fullPath, filename, sizeof(fullPath) - 1);
    } else {
        snprintf(fullPath, sizeof(fullPath), "/routes/%s", filename);
        if (!SD.exists(fullPath)) {
            snprintf(fullPath, sizeof(fullPath), "/tracks/%s", filename);
            if (!SD.exists(fullPath)) {
                snprintf(fullPath, sizeof(fullPath), "/%s", filename);
            }
        }
    }
    fullPath[sizeof(fullPath) - 1] = 0;

    File f = SD.open(fullPath, FILE_READ);
    if (!f) {
        NavEngine::unlockSd();
        Serial.printf("[GPX] Failed to open %s\n", fullPath);
        DisplayEngine::showToast("GPX-Fehler", "Datei nicht gefunden", 2500, ICO_ROUTE);
        return false;
    }

    std::vector<NavRoutePoint> route;
    route.reserve(256);

    char line[160];
    while (f.available()) {
        size_t l = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[l] = 0;
        // Skip bounds or metadata headers so tags like <bounds minlat="..." minlon="..."> are not mistaken for waypoints
        if (strstr(line, "<bounds") != nullptr || strstr(line, "<metadata") != nullptr) continue;

        // Look for lat="..." and lon="..." preceded by a space or tab (filters out minlat/maxlat)
        const char* pLat = strstr(line, " lat=\"");
        if (!pLat) pLat = strstr(line, "\tlat=\"");
        const char* pLon = strstr(line, " lon=\"");
        if (!pLon) pLon = strstr(line, "\tlon=\"");

        if (pLat && pLon) {
            double lat = atof(pLat + 6);
            double lon = atof(pLon + 6);
            if (lat != 0.0 && lon != 0.0) {
                NavRoutePoint pt{(int32_t)llround(lat * 1e5), (int32_t)llround(lon * 1e5)};
                if (route.empty() ||
                    abs(route.back().lat_e5 - pt.lat_e5) > 4 ||
                    abs(route.back().lon_e5 - pt.lon_e5) > 4) {
                    route.push_back(pt);
                }
            }
        }
    }
    f.close();
    NavEngine::unlockSd();

    if (route.size() < 2) {
        Serial.printf("[GPX] No valid points found in %s\n", fullPath);
        DisplayEngine::showToast("GPX-Fehler", "Keine Wegpunkte gefunden", 2500, ICO_ROUTE);
        return false;
    }

    char destName[32];
    const char* base = strrchr(fullPath, '/');
    base = base ? (base + 1) : fullPath;
    strncpy(destName, base, sizeof(destName) - 1);
    destName[sizeof(destName) - 1] = 0;

    float totalDist = 0.0f;
    for (size_t i = 0; i + 1 < route.size(); i++) {
        totalDist += haversineKm(route[i].lat_e5 * 1e-5, route[i].lon_e5 * 1e-5,
                                 route[i+1].lat_e5 * 1e-5, route[i+1].lon_e5 * 1e-5);
    }
    size_t routePointsCount = route.size();

    // Acquire NavLock only for the atomic assignment to internal state
    {
        NavLock lock;
        sRoute = std::move(route);
        sStat.activeRoute = true;
        sStat.routeFailed = false;
        sStat.routeStraightLine = false;
        sStat.routingBusy = false;
        sReq.pending = false;
        sRouteWaitingForPos = false;

        sStat.destLat = sRoute.back().lat_e5 * 1e-5;
        sStat.destLon = sRoute.back().lon_e5 * 1e-5;
        strncpy(sStat.destName, destName, sizeof(sStat.destName) - 1);
        sStat.destName[sizeof(sStat.destName) - 1] = 0;

        sStat.totalDistKm = totalDist;
        sStat.remDistKm = totalDist;

        setTurn("FOLGE GPX-ROUTE", ICO_ROUTE);
        updateTurnByTurnLocked();
    }

    nav::markDirty(nav::PG_POSITION);
    DisplayEngine::showNotification("GPX-Route geladen", destName, 3500, ICO_ROUTE);
    Serial.printf("[GPX] Route loaded: %u points, %.2f km from %s\n", (unsigned)routePointsCount, totalDist, fullPath);
    return true;
}

int NavEngine::listGpxFiles(char outFiles[][32], int maxFiles) {
    if (!outFiles || maxFiles <= 0) return 0;
    {
        NavLock lock;
        if (!sStat.sdReady) return 0;
    }
    if (!NavEngine::lockSd(300)) return 0;

    int count = 0;
    const char* dirs[] = {"/routes", "/tracks", "/"};
    for (const char* d : dirs) {
        if (!SD.exists(d)) continue;
        File dir = SD.open(d);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); continue; }
        File entry = dir.openNextFile();
        while (entry && count < maxFiles) {
            if (!entry.isDirectory()) {
                const char* fn = entry.name();
                size_t l = strlen(fn);
                if (l > 4 && strcasecmp(fn + l - 4, ".gpx") == 0) {
                    bool dup = false;
                    for (int k = 0; k < count; k++) {
                        if (strcmp(outFiles[k], fn) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        strncpy(outFiles[count++], fn, 31);
                        outFiles[count - 1][31] = 0;
                    }
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
    }
    NavEngine::unlockSd();
    return count;
}

void NavEngine::clearTrail() {
    NavLock lock;
    sGpxTrail.clear();
    sStat.gpxPoints = 0;
}

// =============================================================================
// Public: search
// =============================================================================
// Case folding for ASCII and the German 2-byte umlauts (C3 84/A4, 96/B6, 9C/BC).
static inline void foldStep(const char*& p, uint16_t& out) {
    uint8_t c = (uint8_t)*p;
    if (c == 0xC3 && p[1]) {
        uint8_t c2 = (uint8_t)p[1];
        p += 2;
        if (c2 == 0x84 || c2 == 0xA4) out = 0xE4;      // ä
        else if (c2 == 0x96 || c2 == 0xB6) out = 0xF6; // ö
        else if (c2 == 0x9C || c2 == 0xBC) out = 0xFC; // ü
        else if (c2 == 0x9F) out = 0xDF;               // ß
        else out = 0xC300 | c2;
        return;
    }
    p++;
    if (c >= 'A' && c <= 'Z') c += 32;
    out = c;
}

// 2 = prefix match, 1 = substring match, 0 = no match
static int matchQuality(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return 2;
    int best = 0;
    for (const char* h = hay; *h; ) {
        const char* p = h;
        const char* n = needle;
        bool ok = true;
        while (*n) {
            if (!*p) { ok = false; break; }
            uint16_t a, b;
            foldStep(p, a);
            foldStep(n, b);
            if (a != b) { ok = false; break; }
        }
        if (ok) {
            if (h == hay) return 2;
            best = 1;
        }
        uint16_t skip;
        foldStep(h, skip);
    }
    return best;
}

static void placeToNav(const DiskPlace& p, NavPlace& dst) {
    strncpy(dst.name, p.name, sizeof(dst.name) - 1);
    dst.name[sizeof(dst.name) - 1] = 0;
    dst.lat = p.lat_e5 * 1e-5;
    dst.lon = p.lon_e5 * 1e-5;
    dst.pop = p.pop;
    dst.rank = p.rank;
}

static bool isDupName(const NavPlace* out, int count, const char* name) {
    for (int k = 0; k < count; k++) if (strcmp(out[k].name, name) == 0) return true;
    return false;
}

int NavEngine::searchPlaces(const char* query, NavPlace* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    NavLock lock;
    int count = 0;
    bool wantMesh = query && strcmp(query, "@mesh") == 0;
    bool wantPoi  = query && strcmp(query, "@poi") == 0;
    bool wantGpx  = query && strcmp(query, "@gpx") == 0;
    bool emptyQ   = !query || !query[0];

    // 0. Backtrack option if breadcrumb trail exists
    if ((emptyQ || wantGpx) && sGpxTrail.size() >= 2 && count < maxOut) {
        NavPlace& dst = out[count++];
        strncpy(dst.name, "Zurück zum Start (Backtrack)", sizeof(dst.name) - 1);
        dst.name[sizeof(dst.name) - 1] = 0;
        dst.lat = sGpxTrail.front().lat_e5 * 1e-5;
        dst.lon = sGpxTrail.front().lon_e5 * 1e-5;
        dst.pop = 0;
        dst.rank = 6;
    }

    // 0b. GPX routes from MicroSD
    if ((emptyQ || wantGpx) && sStat.sdReady && count < maxOut) {
        char gpxFiles[6][32];
        int ng = listGpxFiles(gpxFiles, 6);
        for (int i = 0; i < ng && count < maxOut; i++) {
            NavPlace& dst = out[count++];
            strncpy(dst.name, gpxFiles[i], sizeof(dst.name) - 1);
            dst.name[sizeof(dst.name) - 1] = 0;
            dst.lat = 0;
            dst.lon = 0;
            dst.pop = 0;
            dst.rank = 7;
        }
    }
    if (wantGpx) return count;

    // 1. mesh peers with a position (rank 0)
    uint8_t np = RadioLoRa::getPeers(sPeerScratch, 16);
    for (uint8_t i = 0; i < np && count < maxOut; i++) {
        if (!sPeerScratch[i].hasPos) continue;
        if (wantMesh || matchQuality(sPeerScratch[i].name, query) > 0) {
            NavPlace& dst = out[count++];
            snprintf(dst.name, sizeof(dst.name), "%s", sPeerScratch[i].name);
            dst.lat = sPeerScratch[i].lat;
            dst.lon = sPeerScratch[i].lon;
            dst.pop = 0;
            dst.rank = 0;
        }
    }
    if (wantMesh) return count;

    // 2. saved waypoints (rank 5), newest first
    for (int i = sNumWaypoints - 1; i >= 0 && count < maxOut; i--) {
        if (wantPoi || matchQuality(sWaypoints[i].name, query) > 0) {
            NavPlace& dst = out[count++];
            snprintf(dst.name, sizeof(dst.name), "%s", sWaypoints[i].name);
            dst.lat = sWaypoints[i].lat;
            dst.lon = sWaypoints[i].lon;
            dst.pop = 0;
            dst.rank = 5;
        }
    }
    if (wantPoi) return count;

    // 3. map place table (PSRAM)
    if (sPlaces && sNumPlaces > 0 && count < maxOut) {
        if (emptyQ) {
            if (havePositionLocked()) {
                // nearest rank<=2 places to the current position (top-K by squared scaled degrees)
                struct Cand { uint32_t idx; double d2; };
                static Cand cand[64];
                int nC = 0;
                int K = std::min(maxOut - count, 64);
                double lonScale = kmPerDegLon(sStat.curLat) / 111.32;
                for (uint32_t i = 0; i < sNumPlaces; i++) {
                    const DiskPlace& p = sPlaces[i];
                    if (p.rank > 2) continue;
                    double dy = p.lat_e5 * 1e-5 - sStat.curLat;
                    double dx = (p.lon_e5 * 1e-5 - sStat.curLon) * lonScale;
                    double d2 = dy * dy + dx * dx;
                    if (nC < K) {
                        int j = nC++;
                        while (j > 0 && cand[j - 1].d2 > d2) { cand[j] = cand[j - 1]; j--; }
                        cand[j] = {i, d2};
                    } else if (d2 < cand[nC - 1].d2) {
                        int j = nC - 1;
                        while (j > 0 && cand[j - 1].d2 > d2) { cand[j] = cand[j - 1]; j--; }
                        cand[j] = {i, d2};
                    }
                }
                for (int i = 0; i < nC && count < maxOut; i++) {
                    const DiskPlace& p = sPlaces[cand[i].idx];
                    if (isDupName(out, count, p.name)) continue;
                    placeToNav(p, out[count++]);
                }
            } else {
                // no position: the table is sorted by importance
                for (uint32_t i = 0; i < sNumPlaces && count < maxOut; i++) {
                    if (isDupName(out, count, sPlaces[i].name)) continue;
                    placeToNav(sPlaces[i], out[count++]);
                }
            }
        } else {
            // prefix matches first, then substring matches (table order = importance)
            for (int pass = 2; pass >= 1 && count < maxOut; pass--) {
                for (uint32_t i = 0; i < sNumPlaces && count < maxOut; i++) {
                    if (matchQuality(sPlaces[i].name, query) != pass) continue;
                    if (isDupName(out, count, sPlaces[i].name)) continue;
                    placeToNav(sPlaces[i], out[count++]);
                }
            }
        }
        return count;
    }

    // 4. fallback while no place table is resident: built-in city list
    for (int i = 0; i < N_BUILTIN_CITIES && count < maxOut; i++) {
        if (matchQuality(BUILTIN_CITIES[i].name, query) > 0 && !isDupName(out, count, BUILTIN_CITIES[i].name)) {
            out[count++] = BUILTIN_CITIES[i];
        }
    }
    return count;
}

// =============================================================================
// Public: GPX & waypoints
// =============================================================================
void NavEngine::setTrackRecording(bool rec) {
    NavLock lock;
    if (sStat.gpxRecording == rec) return;
    sStat.gpxRecording = rec;
    if (rec) {
        sLastGpxMs = 0;
    } else {
        gpxWriteCloseLocked();
    }
}

bool NavEngine::isTrackRecording() {
    NavLock lock;
    return sStat.gpxRecording;
}

bool NavEngine::markWaypoint(const char* label) {
    NavLock lock;
    updateCurrentPositionLocked();
    if (!havePositionLocked()) {
        Serial.println("[NAV] waypoint not saved: no position");
        return false;
    }
    int idx = sNumWaypoints;
    if (idx >= MAX_USER_WAYPOINTS) {
        for (int i = 0; i + 1 < MAX_USER_WAYPOINTS; i++) sWaypoints[i] = sWaypoints[i + 1];
        idx = MAX_USER_WAYPOINTS - 1;
    } else {
        sNumWaypoints++;
    }
    if (label && label[0]) snprintf(sWaypoints[idx].name, sizeof(sWaypoints[idx].name), "%s #%d", label, idx + 1);
    else                   snprintf(sWaypoints[idx].name, sizeof(sWaypoints[idx].name), "POI #%d", idx + 1);
    sWaypoints[idx].lat = sStat.curLat;
    sWaypoints[idx].lon = sStat.curLon;
    saveWaypointsToNvs();

    if (sStat.sdReady && sStat.sdWritable) {
        IoLock io;
        File f = SD.open("/waypoints.csv", FILE_APPEND);
        if (f) {
            f.printf("%lu,%s,%.6f,%.6f,%.1f,%s\n",
                     (unsigned long)time(NULL), sWaypoints[idx].name,
                     sStat.curLat, sStat.curLon, GpsManager::altitudeM(), sStat.posSourceTag);
            f.close();
        }
    }
    nav::markDirty(nav::PG_POSITION);
    return true;
}

// =============================================================================
// Rendering primitives (RGB565 software rasteriser)
// =============================================================================
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

static inline void putPx(uint16_t* buf, int w, int h, int x, int y, uint16_t col) {
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h) buf[y * w + x] = col;
}

static void drawLine(uint16_t* buf, int w, int h, int x0, int y0, int x1, int y1, uint16_t col, int thickness = 1) {
    if ((x0 < -60 && x1 < -60) || (x0 >= w + 60 && x1 >= w + 60) ||
        (y0 < -60 && y1 < -60) || (y0 >= h + 60 && y1 >= h + 60)) return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int maxSteps = w + h + 480;
    while (maxSteps-- > 0) {
        if (thickness <= 1) {
            putPx(buf, w, h, x0, y0, col);
        } else if (thickness == 2) {
            putPx(buf, w, h, x0, y0, col);
            putPx(buf, w, h, x0 + 1, y0, col);
            putPx(buf, w, h, x0, y0 + 1, col);
        } else if (thickness == 3) {
            for (int ty = -1; ty <= 1; ty++)
                for (int tx = -1; tx <= 1; tx++)
                    putPx(buf, w, h, x0 + tx, y0 + ty, col);
        } else {
            int r = thickness / 2;
            for (int ty = -r; ty <= r; ty++)
                for (int tx = -r; tx <= r; tx++)
                    if (tx * tx + ty * ty <= r * r + 1) putPx(buf, w, h, x0 + tx, y0 + ty, col);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void drawFilledPolygon(uint16_t* buf, int w, int h, const int16_t* px, const int16_t* py, int npts, uint16_t fillCol) {
    if (npts < 3) return;
    int minY = py[0], maxY = py[0], minX = px[0], maxX = px[0];
    for (int i = 1; i < npts; i++) {
        if (py[i] < minY) minY = py[i];
        if (py[i] > maxY) maxY = py[i];
        if (px[i] < minX) minX = px[i];
        if (px[i] > maxX) maxX = px[i];
    }
    if (maxX < 0 || minX >= w || maxY < 0 || minY >= h) return;
    minY = std::max(0, minY);
    maxY = std::min(h - 1, maxY);

    int nodes[64];
    for (int y = minY; y <= maxY; y++) {
        int nNodes = 0;
        int j = npts - 1;
        for (int i = 0; i < npts; i++) {
            int yi = py[i], yj = py[j];
            if ((yi < y && yj >= y) || (yj < y && yi >= y)) {
                if (nNodes < 64) nodes[nNodes++] = px[i] + (y - yi) * (px[j] - px[i]) / (yj - yi);
            }
            j = i;
        }
        for (int i = 1; i < nNodes; i++) {
            int v = nodes[i];
            int k = i - 1;
            while (k >= 0 && nodes[k] > v) { nodes[k + 1] = nodes[k]; k--; }
            nodes[k + 1] = v;
        }
        uint16_t* row = buf + y * w;
        for (int i = 0; i + 1 < nNodes; i += 2) {
            int x0 = std::max(0, nodes[i]);
            int x1 = std::min(w - 1, nodes[i + 1]);
            for (int x = x0; x <= x1; x++) row[x] = fillCol;
        }
    }
}

static void drawFilledCircle(uint16_t* buf, int w, int h, int cx, int cy, int r, uint16_t col) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) putPx(buf, w, h, cx + dx, cy + dy, col);
}

static void drawRing(uint16_t* buf, int w, int h, int cx, int cy, int r, uint16_t col) {
    int r2Min = (r - 1) * (r - 1), r2Max = (r + 1) * (r + 1);
    for (int dy = -r - 1; dy <= r + 1; dy++)
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2Min && d2 <= r2Max) putPx(buf, w, h, cx + dx, cy + dy, col);
        }
}

static void drawRoundedBadge(uint16_t* buf, int w, int h, int cx, int cy, int r, uint16_t bgCol, uint16_t borderCol) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= (r - 1) * (r - 1)) putPx(buf, w, h, cx + dx, cy + dy, bgCol);
            else if (d2 <= r * r) putPx(buf, w, h, cx + dx, cy + dy, borderCol);
        }
}

static const uint8_t MINI_FONT[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space (32)
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x00,0x00,0x00,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x00,0x00,0x00,0x00,0x00}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x00,0x00,0x00,0x00,0x00}, // &
    {0x00,0x00,0x07,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x2A,0x1C,0x7F,0x1C,0x2A}, // *
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

static void drawMapText(uint16_t* buf, int w, int h, int x, int y, const char* txt, uint16_t col, uint16_t haloCol, int maxChars = 18) {
    int cx = x;
    for (int i = 0; txt[i] && i < maxChars; i++) {
        uint8_t c = (uint8_t)txt[i];
        if (c == 0xC3 && txt[i + 1]) {
            uint8_t c2 = (uint8_t)txt[++i];
            if (c2 == 0xA4 || c2 == 0x84) c = 'A';
            else if (c2 == 0xB6 || c2 == 0x96) c = 'O';
            else if (c2 == 0xBC || c2 == 0x9C) c = 'U';
            else if (c2 == 0x9F) c = 'S';
            else c = ' ';
        } else if (c >= 0x80) {
            c = ' ';
        }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 'Z') c = ' ';
        const uint8_t* glyph = MINI_FONT[c - 32];
        for (int colIdx = 0; colIdx < 5; colIdx++) {
            uint8_t bits = glyph[colIdx];
            for (int rowIdx = 0; rowIdx < 7; rowIdx++) {
                if (bits & (1 << rowIdx)) {
                    putPx(buf, w, h, cx + colIdx - 1, y + rowIdx, haloCol);
                    putPx(buf, w, h, cx + colIdx + 1, y + rowIdx, haloCol);
                    putPx(buf, w, h, cx + colIdx, y + rowIdx - 1, haloCol);
                    putPx(buf, w, h, cx + colIdx, y + rowIdx + 1, haloCol);
                    putPx(buf, w, h, cx + colIdx, y + rowIdx, col);
                }
            }
        }
        cx += 6;
    }
}

static int textPixelWidth(const char* txt, int maxChars) {
    int n = 0;
    for (int i = 0; txt[i] && n < maxChars; i++) {
        if ((uint8_t)txt[i] == 0xC3 && txt[i + 1]) i++;
        n++;
    }
    return n * 6;
}

// =============================================================================
// Map rendering
// =============================================================================
struct LabelBox { int x0, y0, x1, y1; };
static bool labelFits(const LabelBox* placed, int n, const LabelBox& b) {
    for (int i = 0; i < n; i++) {
        if (!(b.x1 < placed[i].x0 || b.x0 > placed[i].x1 || b.y1 < placed[i].y0 || b.y0 > placed[i].y1)) return false;
    }
    return true;
}

// Shrinks a cell range symmetrically around its centre so it has at most
// maxW x maxH cells.
static void limitCellRange(int& c0, int& c1, int maxN) {
    int n = c1 - c0 + 1;
    if (n <= maxN) return;
    int mid = (c0 + c1) / 2;
    c0 = mid - maxN / 2;
    c1 = c0 + maxN - 1;
}

void NavEngine::renderMap(uint16_t* buf, int w, int h, double viewLat, double viewLon, int zoom, float headingDeg) {
    if (!buf || w <= 0 || h <= 0) return;
    zoom = std::clamp(zoom, 3, 18);
    NavLock lock;

    bool noPosHint = false;
    if (fabs(viewLat) < 0.01 && fabs(viewLon) < 0.01) {
        // "no position": centre on the map
        if (sStat.mapLoaded && sHdrIsV3 && sHdr.bboxMaxLat_e5 > sHdr.bboxMinLat_e5 && sHdr.bboxMaxLon_e5 > sHdr.bboxMinLon_e5) {
            viewLat = (sHdr.bboxMinLat_e5 + sHdr.bboxMaxLat_e5) * 0.5e-5;
            viewLon = (sHdr.bboxMinLon_e5 + sHdr.bboxMaxLon_e5) * 0.5e-5;
        } else {
            viewLat = DE_CENTER_LAT;
            viewLon = DE_CENTER_LON;
        }
        noPosHint = true;
    }

    // 1. base canvas (Google Maps sleek dark-slate theme)
    uint16_t bgCol = (zoom >= 13) ? rgb565(28, 33, 44) : rgb565(20, 25, 34);
    uint16_t gridCol = rgb565(32, 38, 50);
    for (int i = 0; i < w * h; i++) buf[i] = bgCol;

    double lonPerPx = 360.0 / (256.0 * pow(2.0, (double)zoom));
    double cosLat = cos(deg2rad(viewLat));
    if (cosLat < 0.2) cosLat = 0.2;
    double latPerPx = lonPerPx * cosLat;

    bool rotate = fabsf(headingDeg) > 0.5f;
    double radR = rotate ? (-headingDeg * (double)M_PI / 180.0) : 0.0;
    double cosR = rotate ? cos(radR) : 1.0;
    double sinR = rotate ? sin(radR) : 0.0;
    double halfW = w * 0.5, halfH = h * 0.5;

    auto project = [&](double lat, double lon, int& sx, int& sy) {
        double dx = (lon - viewLon) / lonPerPx;
        double dy = -(lat - viewLat) / latPerPx;
        if (rotate) {
            sx = (int)(halfW + (dx * cosR - dy * sinR));
            sy = (int)(halfH + (dx * sinR + dy * cosR));
        } else {
            sx = (int)(halfW + dx);
            sy = (int)(halfH + dy);
        }
    };

    if (zoom <= 13) {
        for (int gx = (w / 2) % 48; gx < w; gx += 48)
            for (int y = 0; y < h; y += 2) buf[y * w + gx] = gridCol;
        for (int gy = (h / 2) % 48; gy < h; gy += 48)
            for (int x = 0; x < w; x += 2) buf[gy * w + x] = gridCol;
    }

    // 2. German border outline at overview zoom
    if (zoom <= 11) {
        uint16_t borderCol = rgb565(56, 74, 102);
        for (int i = 0; i + 1 < N_BORDER_PTS; i++) {
            int x0, y0, x1, y1;
            project(DE_BORDER_PTS[i][0] * 1e-5, DE_BORDER_PTS[i][1] * 1e-5, x0, y0);
            project(DE_BORDER_PTS[i + 1][0] * 1e-5, DE_BORDER_PTS[i + 1][1] * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, borderCol, 2);
        }
    }

    double diagFactor = rotate ? 0.85 : 0.58;
    double minLat = viewLat - (h * diagFactor) * latPerPx;
    double maxLat = viewLat + (h * diagFactor) * latPerPx;
    double minLon = viewLon - (w * diagFactor) * lonPerPx;
    double maxLon = viewLon + (w * diagFactor) * lonPerPx;

    int16_t polyX[64], polyY[64];

    auto renderCellBuffer = [&](const uint8_t* cdata, uint32_t clen, double baseLat, double baseLon,
                                double cellLatH, double cellLonW, int passMode) {
        if (!cdata || clen < 2) return;
        uint16_t nseg = (uint16_t)(cdata[0] | (cdata[1] << 8));
        size_t pos = 2;

        for (uint16_t s = 0; s < nseg && pos + 2 <= clen; s++) {
            uint8_t cls = cdata[pos++];
            uint8_t npts = cdata[pos++];
            if (pos + (size_t)npts * 2 > clen) break;
            if (cls >= FEAT_CLASS_COUNT) { pos += (size_t)npts * 2; continue; }
            bool isPoly = (cls <= FEAT_LAST_POLYGON);

            if ((passMode == 0 && !isPoly) ||
                (passMode != 0 && isPoly) ||
                (passMode != 0 && zoom < 12 && (cls == FEAT_PATH || cls == FEAT_RESIDENTIAL)) ||
                (passMode == 0 && cls == FEAT_BUILDING && zoom < 13)) {
                pos += (size_t)npts * 2;
                continue;
            }

            int cnt = std::min<int>(npts, 64);
            for (int k = 0; k < cnt; k++) {
                uint8_t qx = cdata[pos + k * 2];
                uint8_t qy = cdata[pos + k * 2 + 1];
                double ptLon = baseLon + (qx / 255.0) * cellLonW;
                double ptLat = baseLat + (qy / 255.0) * cellLatH;
                int sx, sy;
                project(ptLat, ptLon, sx, sy);
                polyX[k] = (int16_t)constrain(sx, -200, w + 200);
                polyY[k] = (int16_t)constrain(sy, -200, h + 200);
            }
            pos += (size_t)npts * 2;

            if (passMode == 0) {
                // Pass 0: Polygons (Google Maps dark palette)
                if (cls == FEAT_FARMLAND) {
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(32, 40, 30));
                    for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], rgb565(44, 58, 38), 1);
                } else if (cls == FEAT_FOREST) {
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(18, 50, 32));
                    for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], rgb565(28, 76, 48), 1);
                } else if (cls == FEAT_PARK) {
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(24, 66, 44));
                    for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], rgb565(38, 96, 62), 1);
                } else if (cls == FEAT_URBAN) {
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(32, 38, 50));
                } else if (cls == FEAT_WATER_POLY) {
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(20, 70, 125));
                    for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], rgb565(50, 135, 215), 1);
                } else if (cls == FEAT_BUILDING) {
                    uint16_t bFill = (zoom >= 15) ? rgb565(40, 48, 62) : rgb565(34, 40, 52);
                    uint16_t bEdge = (zoom >= 15) ? rgb565(65, 78, 100) : rgb565(52, 62, 80);
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, bFill);
                    if (zoom >= 14) {
                        for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], bEdge, 1);
                    }
                }
            } else if (passMode == 1) {
                // Pass 1: Road Casings (draw all casings first across all cells!)
                uint16_t caseCol = 0;
                int caseW = 0;
                switch (cls) {
                    case FEAT_RESIDENTIAL:
                        caseCol = rgb565(36, 44, 56);
                        caseW = (zoom >= 16) ? 5 : (zoom >= 15) ? 4 : (zoom >= 14) ? 3 : 0;
                        break;
                    case FEAT_TERTIARY:
                        caseCol = rgb565(42, 50, 66);
                        caseW = (zoom >= 15) ? 5 : (zoom >= 13) ? 3 : 0;
                        break;
                    case FEAT_SECONDARY:
                        caseCol = rgb565(44, 54, 70);
                        caseW = (zoom >= 15) ? 5 : (zoom >= 12) ? 3 : 0;
                        break;
                    case FEAT_PRIMARY:
                        caseCol = rgb565(110, 80, 15);
                        caseW = (zoom >= 14) ? 5 : (zoom >= 11) ? 3 : 0;
                        break;
                    case FEAT_MOTORWAY:
                        caseCol = rgb565(130, 48, 12);
                        caseW = (zoom >= 13) ? 6 : (zoom >= 9) ? 4 : 0;
                        break;
                    default:
                        break;
                }
                if (caseW > 0) {
                    for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], caseCol, caseW);
                }
            } else if (passMode == 2) {
                // Pass 2: Road Cores & Paths (Google Maps crisp fills on top of casings)
                uint16_t coreCol = rgb565(140, 155, 175);
                int coreW = 1;
                switch (cls) {
                    case FEAT_WATER:
                        coreCol = rgb565(50, 135, 215);
                        coreW = (zoom >= 13) ? 3 : 2;
                        break;
                    case FEAT_BOUNDARY:
                        coreCol = rgb565(85, 110, 145);
                        break;
                    case FEAT_RAIL:
                        coreCol = rgb565(160, 170, 195);
                        coreW = (zoom >= 14) ? 2 : 1;
                        break;
                    case FEAT_PATH:
                        coreCol = rgb565(76, 175, 80);
                        coreW = (zoom >= 15) ? 2 : 1;
                        break;
                    case FEAT_RESIDENTIAL:
                        coreCol = rgb565(190, 202, 218);
                        coreW = (zoom >= 16) ? 3 : (zoom >= 14) ? 2 : 1;
                        break;
                    case FEAT_TERTIARY:
                        coreCol = rgb565(215, 226, 238);
                        coreW = (zoom >= 15) ? 3 : (zoom >= 13) ? 2 : 1;
                        break;
                    case FEAT_SECONDARY:
                        coreCol = rgb565(232, 240, 250);
                        coreW = (zoom >= 15) ? 3 : (zoom >= 12) ? 2 : 1;
                        break;
                    case FEAT_PRIMARY:
                        coreCol = rgb565(255, 205, 55);
                        coreW = (zoom >= 14) ? 3 : (zoom >= 11) ? 2 : 1;
                        break;
                    case FEAT_MOTORWAY:
                        coreCol = rgb565(255, 154, 30);
                        coreW = (zoom >= 13) ? 4 : (zoom >= 9) ? 2 : 1;
                        break;
                    default:
                        break;
                }
                for (int k = 0; k + 1 < cnt; k++) drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], coreCol, coreW);
            }
        }
    };

    // ultra cell range of the viewport (also used for the POI index)
    const double stgLatSpan = KNAV_STG_MAX_LAT - KNAV_STG_MIN_LAT;
    const double stgLonSpan = KNAV_STG_MAX_LON - KNAV_STG_MIN_LON;
    bool viewTouchesStg = (maxLat >= KNAV_STG_MIN_LAT && minLat <= KNAV_STG_MAX_LAT &&
                           maxLon >= KNAV_STG_MIN_LON && minLon <= KNAV_STG_MAX_LON);
    int ucy0 = 0, ucy1 = -1, ucx0 = 0, ucx1 = -1;
    if (viewTouchesStg) {
        ucy0 = std::clamp((int)((minLat - KNAV_STG_MIN_LAT) / stgLatSpan * KNAV_ULTRA_NY), 0, KNAV_ULTRA_NY - 1);
        ucy1 = std::clamp((int)((maxLat - KNAV_STG_MIN_LAT) / stgLatSpan * KNAV_ULTRA_NY), 0, KNAV_ULTRA_NY - 1);
        ucx0 = std::clamp((int)((minLon - KNAV_STG_MIN_LON) / stgLonSpan * KNAV_ULTRA_NX), 0, KNAV_ULTRA_NX - 1);
        ucx1 = std::clamp((int)((maxLon - KNAV_STG_MIN_LON) / stgLonSpan * KNAV_ULTRA_NX), 0, KNAV_ULTRA_NX - 1);
    }

    // 3. vector tiles
    if (sStat.mapLoaded && sMapRd.valid) {
        bool ultraOk = sStat.ultraLoaded && zoom >= 13 && viewTouchesStg &&
                       (ucx1 - ucx0 + 1) <= 12 && (ucy1 - ucy0 + 1) <= 8;

        if (ultraOk) {
            double cellLatH = stgLatSpan / KNAV_ULTRA_NY;
            double cellLonW = stgLonSpan / KNAV_ULTRA_NX;
            for (int pass = 0; pass < 3; pass++) {
                for (int cy = ucy0; cy <= ucy1; cy++) {
                    for (int cx = ucx0; cx <= ucx1; cx++) {
                        uint32_t clen = 0;
                        const uint8_t* cdata = getCachedCellData(2, (uint32_t)(cy * KNAV_ULTRA_NX + cx), clen);
                        renderCellBuffer(cdata, clen, KNAV_STG_MIN_LAT + cy * cellLatH,
                                         KNAV_STG_MIN_LON + cx * cellLonW, cellLatH, cellLonW, pass);
                    }
                }
            }
        } else {
            // Fine (256x256) when the viewport fits 8x6 fine cells, else coarse (32x32),
            // capped to 16x10 coarse cells around the centre.
            bool useFine = (zoom >= 10) && sHdr.fineDirOffset != 0;
            const double latSpan = KNAV_DE_MAX_LAT - KNAV_DE_MIN_LAT;
            const double lonSpan = KNAV_DE_MAX_LON - KNAV_DE_MIN_LON;
            int nx = useFine ? KNAV_FINE_NX : KNAV_COARSE_NX;
            int ny = useFine ? KNAV_FINE_NY : KNAV_COARSE_NY;
            auto cellRange = [&](int& cy0, int& cy1, int& cx0, int& cx1) {
                cy0 = std::clamp((int)((minLat - KNAV_DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
                cy1 = std::clamp((int)((maxLat - KNAV_DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
                cx0 = std::clamp((int)((minLon - KNAV_DE_MIN_LON) / lonSpan * nx), 0, nx - 1);
                cx1 = std::clamp((int)((maxLon - KNAV_DE_MIN_LON) / lonSpan * nx), 0, nx - 1);
            };
            int cy0, cy1, cx0, cx1;
            cellRange(cy0, cy1, cx0, cx1);
            if (useFine && (cy1 - cy0 + 1 > 6 || cx1 - cx0 + 1 > 8)) {
                useFine = false;
                nx = KNAV_COARSE_NX; ny = KNAV_COARSE_NY;
                cellRange(cy0, cy1, cx0, cx1);
            }
            if (!useFine) {
                limitCellRange(cx0, cx1, 16);
                limitCellRange(cy0, cy1, 10);
            }
            double cellLatH = latSpan / ny;
            double cellLonW = lonSpan / nx;
            int tier = useFine ? 1 : 0;
            if (dirOffsetForTier(tier) != 0) {
                for (int pass = 0; pass < 3; pass++) {
                    for (int cy = cy0; cy <= cy1; cy++) {
                        for (int cx = cx0; cx <= cx1; cx++) {
                            uint32_t clen = 0;
                            const uint8_t* cdata = getCachedCellData(tier, (uint32_t)(cy * nx + cx), clen);
                            renderCellBuffer(cdata, clen, KNAV_DE_MIN_LAT + cy * cellLatH,
                                             KNAV_DE_MIN_LON + cx * cellLonW, cellLatH, cellLonW, pass);
                        }
                    }
                }
            }
        }
    }

    LabelBox placed[40];
    int nPlaced = 0;

    // 4. POIs & street names from the in-PSRAM index (zoom >= 13)
    if (sPois && sPoiCellStart && sPoiOrder && zoom >= 13 && viewTouchesStg) {
        int py0 = ucy0, py1 = ucy1, px0 = ucx0, px1 = ucx1;
        limitCellRange(px0, px1, 12);
        limitCellRange(py0, py1, 8);
        int32_t minLat5 = (int32_t)(minLat * 1e5), maxLat5 = (int32_t)(maxLat * 1e5);
        int32_t minLon5 = (int32_t)(minLon * 1e5), maxLon5 = (int32_t)(maxLon * 1e5);
        int poiPlaced = 0;
        for (int cy = py0; cy <= py1 && poiPlaced < 26; cy++) {
            for (int cx = px0; cx <= px1 && poiPlaced < 26; cx++) {
                if (cy < 0 || cy >= KNAV_ULTRA_NY || cx < 0 || cx >= KNAV_ULTRA_NX) continue;
                uint32_t c = (uint32_t)(cy * KNAV_ULTRA_NX + cx);
                for (uint32_t k = sPoiCellStart[c]; k < sPoiCellStart[c + 1] && poiPlaced < 26; k++) {
                    uint32_t pi = sPoiOrder[k];
                    if (pi >= sNumPois) continue;
                    const DiskPoi& p = sPois[pi];
                    if (p.lat_e5 < minLat5 || p.lat_e5 > maxLat5 || p.lon_e5 < minLon5 || p.lon_e5 > maxLon5) continue;
                    if (p.category == POI_STREET && zoom < 14) continue;
                    if (p.category == POI_NONE || p.category >= POI_CATEGORY_COUNT) continue;

                    int sx, sy;
                    project(p.lat_e5 * 1e-5, p.lon_e5 * 1e-5, sx, sy);
                    if (sx < 14 || sx >= w - 50 || sy < 14 || sy >= h - 16) continue;

                    int txtW = textPixelWidth(p.name, 15) + 14;
                    LabelBox b{sx - 6, sy - 7, sx + txtW, sy + 9};
                    if (!labelFits(placed, nPlaced, b)) continue;
                    if (nPlaced < 40) placed[nPlaced++] = b;
                    poiPlaced++;

                    if (p.category == POI_STREET) {
                        drawMapText(buf, w, h, sx, sy - 3, p.name, rgb565(185, 200, 222), rgb565(10, 14, 22), 16);
                    } else {
                        uint16_t badgeCol = rgb565(50, 140, 250);
                        char sym[2] = {'*', 0};
                        switch (p.category) {
                            case POI_TRANSIT: badgeCol = rgb565(30, 185, 95);  sym[0] = 'S'; break;
                            case POI_PARKING: badgeCol = rgb565(40, 130, 245); sym[0] = 'P'; break;
                            case POI_FUEL:    badgeCol = rgb565(245, 140, 30); sym[0] = 'T'; break;
                            case POI_FOOD:    badgeCol = rgb565(240, 155, 45); sym[0] = 'R'; break;
                            case POI_SHOP:    badgeCol = rgb565(40, 185, 220); sym[0] = 'M'; break;
                            case POI_HEALTH:  badgeCol = rgb565(245, 65, 75);  sym[0] = '+'; break;
                            case POI_SIGHT:   badgeCol = rgb565(215, 175, 45); sym[0] = '*'; break;
                            default: break;
                        }
                        drawRoundedBadge(buf, w, h, sx, sy, 6, badgeCol, rgb565(255, 255, 255));
                        drawMapText(buf, w, h, sx - 2, sy - 3, sym, rgb565(255, 255, 255), badgeCol, 1);
                        if (zoom >= 14 && p.name[0]) {
                            drawMapText(buf, w, h, sx + 9, sy - 3, p.name, rgb565(230, 238, 250), rgb565(8, 12, 18), 14);
                        }
                    }
                }
            }
        }
    }

    // 5. GPX breadcrumb trail
    if (sGpxTrail.size() >= 2) {
        uint16_t gpxCol = rgb565(255, 200, 30);
        for (size_t i = 0; i + 1 < sGpxTrail.size(); i++) {
            int x0, y0, x1, y1;
            project(sGpxTrail[i].lat_e5 * 1e-5, sGpxTrail[i].lon_e5 * 1e-5, x0, y0);
            project(sGpxTrail[i + 1].lat_e5 * 1e-5, sGpxTrail[i + 1].lon_e5 * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, gpxCol, 2);
        }
    }

    // 6. active route (straight line = thinner line without halo) + destination
    if (sStat.activeRoute) {
        if (sRoute.size() >= 2 && havePositionLocked()) {
            bool straight = sStat.routeStraightLine || sStat.mode == NAV_MODE_MESH;
            uint16_t routeHalo = rgb565(0, 90, 110);
            uint16_t routeCore = (sStat.mode == NAV_MODE_MESH) ? rgb565(195, 90, 255) :
                                 (sStat.mode == NAV_MODE_PED)  ? rgb565(50, 235, 110) : rgb565(0, 245, 212);
            for (size_t i = 0; i + 1 < sRoute.size(); i++) {
                int x0, y0, x1, y1;
                project(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5, x0, y0);
                project(sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5, x1, y1);
                if (straight) {
                    drawLine(buf, w, h, x0, y0, x1, y1, routeCore, 2);
                } else {
                    drawLine(buf, w, h, x0, y0, x1, y1, routeHalo, 4);
                    drawLine(buf, w, h, x0, y0, x1, y1, routeCore, 2);
                }
            }
        }
        int dx, dy;
        project(sStat.destLat, sStat.destLon, dx, dy);
        drawFilledCircle(buf, w, h, dx, dy, 6, rgb565(255, 60, 60));
        drawRing(buf, w, h, dx, dy, 8, rgb565(255, 255, 255));
        drawMapText(buf, w, h, dx + 10, dy - 4, sStat.destName, rgb565(255, 120, 120), rgb565(0, 0, 0));
    }

    // 7. saved waypoints
    for (int i = 0; i < sNumWaypoints; i++) {
        int wx, wy;
        project(sWaypoints[i].lat, sWaypoints[i].lon, wx, wy);
        if (wx >= -10 && wx < w + 10 && wy >= -10 && wy < h + 10) {
            drawRoundedBadge(buf, w, h, wx, wy, 5, rgb565(255, 195, 20), rgb565(255, 255, 255));
            drawMapText(buf, w, h, wx + 8, wy - 3, sWaypoints[i].name, rgb565(255, 215, 60), rgb565(0, 0, 0), 12);
        }
    }

    // 8. place labels: map place table (rank by zoom) or the built-in fallback list
    {
        uint16_t cityDotCol = rgb565(225, 235, 250);
        uint16_t cityTxtCol = rgb565(210, 225, 245);
        uint8_t maxRank = (zoom <= 7) ? 1 : (zoom <= 10) ? 2 : (zoom <= 13) ? 3 : 4;
        int labels = 0;
        auto tryLabel = [&](const char* name, double lat, double lon, uint8_t rank) {
            if (rank > maxRank || labels >= 22) return;
            if (lat < minLat || lat > maxLat || lon < minLon || lon > maxLon) return;
            int sx, sy;
            project(lat, lon, sx, sy);
            if (sx < 10 || sx >= w - 45 || sy < 10 || sy >= h - 10) return;
            int tw = textPixelWidth(name, 16);
            LabelBox b{sx - 4, sy - 6, sx + 6 + tw, sy + 6};
            if (!labelFits(placed, nPlaced, b)) return;
            if (nPlaced < 40) placed[nPlaced++] = b;
            labels++;
            drawFilledCircle(buf, w, h, sx, sy, (rank <= 1) ? 3 : 2, cityDotCol);
            drawMapText(buf, w, h, sx + 5, sy - 3, name, cityTxtCol, rgb565(5, 8, 14), 16);
        };
        if (sPlaces && sNumPlaces > 0) {
            for (uint32_t i = 0; i < sNumPlaces && labels < 22; i++) {
                const DiskPlace& p = sPlaces[i];
                tryLabel(p.name, p.lat_e5 * 1e-5, p.lon_e5 * 1e-5, p.rank);
            }
        } else {
            for (int i = 0; i < N_BUILTIN_CITIES; i++) {
                tryLabel(BUILTIN_CITIES[i].name, BUILTIN_CITIES[i].lat, BUILTIN_CITIES[i].lon, BUILTIN_CITIES[i].rank);
            }
        }
    }

    // 9. mesh peers
    uint8_t nPeers = RadioLoRa::getPeers(sPeerScratch, 16);
    for (uint8_t i = 0; i < nPeers; i++) {
        if (!sPeerScratch[i].hasPos) continue;
        int px, py;
        project(sPeerScratch[i].lat, sPeerScratch[i].lon, px, py);
        if (px >= -10 && px < w + 10 && py >= -10 && py < h + 10) {
            uint16_t pCol = sPeerScratch[i].isSos ? rgb565(255, 50, 60) : rgb565(195, 90, 255);
            drawFilledCircle(buf, w, h, px, py, 5, pCol);
            drawRing(buf, w, h, px, py, 7, rgb565(255, 255, 255));
            drawMapText(buf, w, h, px + 9, py - 3, sPeerScratch[i].name, pCol, rgb565(0, 0, 0));
        }
    }

    // 10. own position (only when known)
    if (havePositionLocked()) {
        int ux, uy;
        project(sStat.curLat, sStat.curLon, ux, uy);
        if (ux >= -12 && ux < w + 12 && uy >= -12 && uy < h + 12) {
            uint16_t myCol = (sStat.posSource == POS_SRC_GPS_LIVE)  ? rgb565(48, 209, 88) :
                             (sStat.posSource == POS_SRC_LORA_MESH) ? rgb565(191, 90, 242) :
                                                                      rgb565(10, 132, 255);
            // Dynamic heading direction cone when moving or heading-up active
            float spd = GpsManager::speedKmh();
            float myHeading = (sStat.headingDeg >= 0.0f) ? sStat.headingDeg : headingDeg;
            if (spd > 1.5f || rotate) {
                float arrowAngle = rotate ? (myHeading - headingDeg) : myHeading;
                float rad = arrowAngle * 0.0174533f;
                int tipX = ux + (int)(sinf(rad) * 18.0f);
                int tipY = uy - (int)(cosf(rad) * 18.0f);
                int wing1X = ux + (int)(sinf(rad + 2.5f) * 11.0f);
                int wing1Y = uy - (int)(cosf(rad + 2.5f) * 11.0f);
                int wing2X = ux + (int)(sinf(rad - 2.5f) * 11.0f);
                int wing2Y = uy - (int)(cosf(rad - 2.5f) * 11.0f);
                drawLine(buf, w, h, tipX, tipY, wing1X, wing1Y, myCol, 2);
                drawLine(buf, w, h, tipX, tipY, wing2X, wing2Y, myCol, 2);
                drawLine(buf, w, h, wing1X, wing1Y, wing2X, wing2Y, myCol, 1);
            }
            drawRing(buf, w, h, ux, uy, 12, myCol);
            drawFilledCircle(buf, w, h, ux, uy, 5, myCol);
            drawRing(buf, w, h, ux, uy, 6, rgb565(255, 255, 255));
        }
    }

    // Compass rose / North indicator (top-right)
    {
        int cx = w - 20, cy = 20;
        drawFilledCircle(buf, w, h, cx, cy, 9, rgb565(18, 24, 36));
        drawRing(buf, w, h, cx, cy, 10, rgb565(55, 70, 95));
        float nAngle = rotate ? (-headingDeg) : 0.0f;
        float nRad = nAngle * 0.0174533f;
        int nx = cx + (int)(sinf(nRad) * 7.0f);
        int ny = cy - (int)(cosf(nRad) * 7.0f);
        int sx = cx - (int)(sinf(nRad) * 7.0f);
        int sy = cy + (int)(cosf(nRad) * 7.0f);
        drawLine(buf, w, h, cx, cy, nx, ny, rgb565(255, 65, 65), 2);
        drawLine(buf, w, h, cx, cy, sx, sy, rgb565(210, 220, 235), 1);
        int lx = cx + (int)(sinf(nRad) * 12.0f) - 2;
        int ly = cy - (int)(cosf(nRad) * 12.0f) - 4;
        drawMapText(buf, w, h, lx, ly, "N", rgb565(255, 85, 85), rgb565(10, 14, 22), 1);
    }

    // 11. scale bar (60 px)
    double barKm = lonPerPx * cosLat * 111.32 * 60.0;
    char scaleTxt[20];
    if (barKm >= 10.0) snprintf(scaleTxt, sizeof(scaleTxt), "%.0f KM", barKm);
    else if (barKm >= 1.0) snprintf(scaleTxt, sizeof(scaleTxt), "%.1f KM", barKm);
    else snprintf(scaleTxt, sizeof(scaleTxt), "%.0f M", barKm * 1000.0);
    drawLine(buf, w, h, 12, h - 12, 72, h - 12, rgb565(220, 230, 240), 2);
    drawLine(buf, w, h, 12, h - 16, 12, h - 8, rgb565(220, 230, 240), 1);
    drawLine(buf, w, h, 72, h - 16, 72, h - 8, rgb565(220, 230, 240), 1);
    drawMapText(buf, w, h, 16, h - 23, scaleTxt, rgb565(220, 230, 240), rgb565(0, 0, 0));

    // 12. hints
    if (noPosHint) {
        const char* hint = "KEINE POSITION";
        int tw = textPixelWidth(hint, 18);
        drawMapText(buf, w, h, (w - tw) / 2, 10, hint, rgb565(255, 190, 60), rgb565(0, 0, 0));
    }
    if (sStat.mapLoaded && !sLoaderFinished) {
        char ld[24];
        snprintf(ld, sizeof(ld), "KARTE LAEDT %u%%", (unsigned)sStat.mapLoadPct);
        int tw = textPixelWidth(ld, 18);
        drawMapText(buf, w, h, w - tw - 10, h - 23, ld, rgb565(170, 185, 210), rgb565(0, 0, 0));
    }
}

// =============================================================================
// Public: SD upload
// =============================================================================
bool NavEngine::beginSdUpload(const char* path, uint32_t totalSize) {
    NavLock lock;
    if (sUploadActive) {
        IoLock io;
        sUploadFile.close();
        sUploadActive = false;
    }
    if (!sStat.sdReady) {
        IoLock io;
        if (!instance.installSD()) return false;
        sStat.sdReady = true;
    }
    // unload the current map: the file is about to be overwritten. A running
    // mirror copy writes to its own temp file and aborts itself on the
    // generation change, so it cannot collide with the upload.
    unloadMapLocked();
    sMirrorPending = false;

    IoLock io;
    SD.mkdir("/kronos");
    const char* p = (path && path[0]) ? path : MAP_SD_PATH_PRIMARY;
    sUploadFile = SD.open(p, FILE_WRITE, true);
    if (!sUploadFile) sUploadFile = SD.open("/germany.knav", FILE_WRITE, true);
    if (!sUploadFile) {
        Serial.println("[SD] upload: cannot create map file");
        return false;
    }
    sUploadRemaining = totalSize;
    sUploadActive = true;
    Serial.printf("[SD] upload started: %s (%lu bytes)\n", p, (unsigned long)totalSize);
    return true;
}

bool NavEngine::writeSdUploadChunk(const uint8_t* data, size_t len) {
    NavLock lock;
    if (!sUploadActive || !sUploadFile || !data) return false;
    IoLock io;
    size_t w = sUploadFile.write(data, len);
    if (w != len) return false;
    if (sUploadRemaining >= len) sUploadRemaining -= (uint32_t)len;
    else sUploadRemaining = 0;
    return true;
}

bool NavEngine::endSdUpload() {
    NavLock lock;
    if (sUploadActive) {
        {
            IoLock io;
            sUploadFile.flush();
            sUploadFile.close();
        }
        sUploadActive = false;
        if (sUploadRemaining > 0) {
            Serial.printf("[SD] upload finished short by %lu bytes\n", (unsigned long)sUploadRemaining);
        } else {
            Serial.println("[SD] upload finished, reloading map");
        }
    }
    bool ok = reloadMap();
    nav::markDirty(nav::PG_POSITION);
    return ok;
}

bool NavEngine::lockSd(uint32_t waitMs) {
    return xSemaphoreTakeRecursive(ioMutex(), pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void NavEngine::unlockSd() {
    xSemaphoreGiveRecursive(ioMutex());
}

