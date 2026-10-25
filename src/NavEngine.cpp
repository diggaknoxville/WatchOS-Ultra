#include "NavEngine.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "RtcManager.h"
#include "DisplayEngine.h"
#include "ui/Icons.h"
#include "ui/Navigator.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
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

// Stuttgart + all adjacent Landkreise bounding box for Tier 3 Ultra-Fine Street/Building/Landuse grid
static constexpr double STU_MIN_LAT = 48.42;
static constexpr double STU_MAX_LAT = 49.18;
static constexpr double STU_MIN_LON = 8.75;
static constexpr double STU_MAX_LON = 9.75;

static constexpr int COARSE_NX = 32;
static constexpr int COARSE_NY = 32;
static constexpr int FINE_NX   = 256;
static constexpr int FINE_NY   = 256;
static constexpr int ULTRA_NX  = 128;
static constexpr int ULTRA_NY  = 128;

// Feature classes in KNAVDE02
enum FeatureClass : uint8_t {
    FEAT_FARMLAND    = 0,  // Filled polygon: Ackerland / Weinberge / Wiesen
    FEAT_FOREST      = 1,  // Filled polygon: Wald / Forst
    FEAT_PARK        = 2,  // Filled polygon: Park / Grünanlage
    FEAT_URBAN       = 3,  // Filled polygon: Wohn-/Gewerbegebiet
    FEAT_WATER_POLY  = 4,  // Filled polygon: See / Neckar / Flussfläche
    FEAT_BUILDING    = 5,  // Filled polygon: Gebäude-Grundriss (3D footprint)
    FEAT_WATER       = 6,  // Polyline: Fluss / Bach
    FEAT_BOUNDARY    = 7,  // Polyline: Grenze
    FEAT_RAIL        = 8,  // Polyline: S-Bahn / U-Bahn / DB-Schiene
    FEAT_PATH        = 9,  // Polyline: Fußweg / Radweg / Waldweg
    FEAT_RESIDENTIAL = 10, // Polyline: Wohnstraße / Anliegerstraße
    FEAT_TERTIARY    = 11, // Polyline: Kreisstraße / Ortsverbindungsstraße
    FEAT_SECONDARY   = 12, // Polyline: Landesstraße
    FEAT_PRIMARY     = 13, // Polyline: Bundesstraße
    FEAT_MOTORWAY    = 14  // Polyline: Autobahn
};

struct KnavHeader {
    char magic[8];          // "KNAVDE02" or "KNAVDE01"
    uint32_t numPlaces;
    uint32_t placesOffset;
    uint32_t numRouteNodes;
    uint32_t numRouteEdges;
    uint32_t routeNodesOffset;
    uint32_t routeEdgesOffset;
    uint32_t coarseDirOffset;
    uint32_t fineDirOffset;
    uint32_t ultraDirOffset;
    uint32_t numPois;
    uint32_t poisOffset;
};

struct DiskPlace {
    int32_t lat_e5;
    int32_t lon_e5;
    uint32_t pop;
    uint8_t rank;
    char name[35];
};

struct DiskPoi {
    int32_t lat_e5;
    int32_t lon_e5;
    uint8_t type; // 0=Transit(S/U), 1=Parking, 2=Fuel, 3=Food/Cafe, 4=Shop/Market, 5=Health, 6=Sight, 7=StreetName
    char name[27];
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

struct UserWaypoint {
    char name[24];
    double lat;
    double lon;
};

static constexpr int MAX_USER_WAYPOINTS = 16;
static UserWaypoint sWaypoints[MAX_USER_WAYPOINTS];
static int sNumWaypoints = 0;

// Built-in cities (Stuttgart + all adjacent Landkreise + major German cities)
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
static bool sIsV2 = false;
static char sMapPath[48] = "/kronos/germany.knav";
static const esp_partition_t* sFlashPart = nullptr;
static bool sUseFlashPart = false;
static std::vector<NavRoutePoint> sRoute;
static std::vector<NavRoutePoint> sGpxTrail;
static unsigned long sLastGpxMs = 0;

// =============================================================================
// SD Card Hardware Write-Protect Unlock (CMD27 PROGRAM_CSD + CMD42 UNLOCK)
// =============================================================================
static uint8_t sdCrc7(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t d = data[i];
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80) crc ^= 0x09;
            d <<= 1;
        }
    }
    return (crc << 1) | 1;
}

static bool testSdWriteCapability() {
    digitalWrite(LORA_CS, HIGH);
    digitalWrite(NFC_CS, HIGH);
    File f = SD.open("/kronos_rw.tmp", FILE_WRITE, true);
    if (!f) return false;
    size_t w = f.write((const uint8_t*)"KRONOS", 6);
    f.flush();
    f.close();
    if (w != 6) return false;
    File r = SD.open("/kronos_rw.tmp", FILE_READ);
    if (!r) return false;
    char buf[8] = {0};
    size_t nr = r.read((uint8_t*)buf, 6);
    r.close();
    SD.remove("/kronos_rw.tmp");
    return (nr == 6 && memcmp(buf, "KRONOS", 6) == 0);
}

static bool tryUnlockSdCardHardware() {
    digitalWrite(LORA_CS, HIGH);
    digitalWrite(NFC_CS, HIGH);
    SD.end();
    delay(20);

    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(SD_CS, HIGH);
    for (int i = 0; i < 10; i++) SPI.transfer(0xFF);

    auto sendCmd = [&](uint8_t cmd, uint32_t arg) -> uint8_t {
        SPI.transfer(0xFF);
        uint8_t pkt[6] = {
            (uint8_t)(0x40 | cmd),
            (uint8_t)(arg >> 24),
            (uint8_t)(arg >> 16),
            (uint8_t)(arg >> 8),
            (uint8_t)(arg & 0xFF),
            0
        };
        pkt[5] = sdCrc7(pkt, 5);
        for (int i = 0; i < 6; i++) SPI.transfer(pkt[i]);
        for (int i = 0; i < 16; i++) {
            uint8_t r = SPI.transfer(0xFF);
            if ((r & 0x80) == 0) return r;
        }
        return 0xFF;
    };

    digitalWrite(SD_CS, LOW);
    uint8_t r0 = sendCmd(0, 0); // CMD0
    if (r0 == 0x01) {
        sendCmd(8, 0x1AA);
        for (int i = 0; i < 4; i++) SPI.transfer(0xFF);
        for (int tries = 0; tries < 200; tries++) {
            sendCmd(55, 0);
            if (sendCmd(41, 0x40000000) == 0x00) break;
            delay(2);
        }
        // Read CSD via CMD9
        if (sendCmd(9, 0) == 0x00) {
            uint8_t tok = 0xFF;
            for (int i = 0; i < 200; i++) {
                tok = SPI.transfer(0xFF);
                if (tok == 0xFE) break;
            }
            if (tok == 0xFE) {
                uint8_t csd[16];
                for (int i = 0; i < 16; i++) csd[i] = SPI.transfer(0xFF);
                SPI.transfer(0xFF); SPI.transfer(0xFF); // CRC16

                bool wpSet = (csd[14] & 0x30) != 0;
                Serial.printf("[SD] CSD[14]=0x%02X (PERM/TMP_WP=%d)\n", csd[14], (int)wpSet);
                if (wpSet) {
                    // Attempt CMD27 (PROGRAM_CSD) clearing bits 4 & 5 (TMP_WRITE_PROTECT & PERM_WRITE_PROTECT)
                    csd[14] &= ~0x30;
                    csd[15] = sdCrc7(csd, 15);
                    uint8_t r27 = sendCmd(27, 0);
                    if (r27 == 0x00) {
                        SPI.transfer(0xFF);
                        SPI.transfer(0xFE); // Data token
                        for (int i = 0; i < 16; i++) SPI.transfer(csd[i]);
                        SPI.transfer(0xFF); SPI.transfer(0xFF); // Dummy CRC16
                        uint8_t resp = SPI.transfer(0xFF);
                        for (int w = 0; w < 500; w++) {
                            if (SPI.transfer(0xFF) == 0xFF) break;
                            delay(1);
                        }
                        Serial.printf("[SD] CMD27 PROGRAM_CSD response=0x%02X\n", resp);
                    }
                }
            }
        }
    }
    digitalWrite(SD_CS, HIGH);
    SPI.transfer(0xFF);
    SPI.endTransaction();
    delay(20);

    bool mounted = SD.begin(SD_CS, SPI, 16000000U);
    if (!mounted) mounted = instance.installSD();
    if (!mounted) return false;
    return testSdWriteCapability();
}

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
            sWaypoints[i].lat = p.getDouble(kLat, 48.7758);
            sWaypoints[i].lon = p.getDouble(kLon, 9.1829);
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
            digitalWrite(LORA_CS, HIGH);
            digitalWrite(NFC_CS, HIGH);
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

// LRU Cell Cache in PSRAM (tier: 0=Coarse, 1=Fine, 2=Ultra Stuttgart)
struct CachedCell {
    int32_t key; // (tier << 20) | cellIdx, -1 = unused
    uint32_t lastUsed;
    uint8_t* data;
    uint32_t len;
};
static constexpr int CELL_CACHE_SLOTS = 32;
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
    strncpy(sStat.posSourceTag, "STUTTGART (LETZTE)", sizeof(sStat.posSourceTag));
    if (fabs(sStat.curLat) < 1.0 || (fabs(sStat.curLat - 52.5200) < 0.01 && fabs(sStat.curLon - 13.4050) < 0.01)) {
        sStat.curLat = 48.7758; // Stuttgart Schlossplatz default
        sStat.curLon = 9.1829;
    }
}

bool NavEngine::reloadMap() {
    clearCellCache();
    loadWaypointsFromStorage();

    sStat.sdReady = (instance.getDeviceProbe() & HW_SD_ONLINE) != 0 && SD.cardType() != CARD_NONE;
    if (!sStat.sdReady) {
        if (instance.installSD()) {
            sStat.sdReady = true;
        }
    }
    if (sStat.sdReady) {
        sStat.sdWritable = testSdWriteCapability();
        if (!sStat.sdWritable) {
            Serial.println("[SD] Card is write-protected in CSD, attempting CMD27 hardware unlock...");
            sStat.sdWritable = tryUnlockSdCardHardware();
            Serial.printf("[SD] After CMD27 unlock attempt: sdWritable=%d\n", (int)sStat.sdWritable);
        }
        sStat.sdTotalMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
        sStat.sdUsedMB  = (uint32_t)(SD.usedBytes() / (1024ULL * 1024ULL));
    } else {
        sStat.sdWritable = false;
        sStat.sdTotalMB = sStat.sdUsedMB = 0;
    }

    sStat.mapLoaded = false;
    sStat.ultraLoaded = false;
    sStat.mapPlaces = N_BUILTIN_CITIES;
    sStat.mapRouteNodes = 0;
    sStat.mapPois = 0;
    sUseFlashPart = false;
    sIsV2 = false;

    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);

    // If SD is writable and doesn't have /kronos/germany.knav yet, mirror Flash KNAVDE02 onto SD!
    if (sStat.sdReady && sStat.sdWritable && !SD.exists("/kronos/germany.knav") && part) {
        KnavHeader fh{};
        if (esp_partition_read(part, 0, &fh, sizeof(fh)) == ESP_OK &&
            (memcmp(fh.magic, "KNAVDE02", 8) == 0 || memcmp(fh.magic, "KNAVDE01", 8) == 0)) {
            SD.mkdir("/kronos");
            File out = SD.open("/kronos/germany.knav", FILE_WRITE, true);
            if (out) {
                Serial.println("[SD] Copying KNAVDE02 map database to SD card /kronos/germany.knav...");
                uint8_t* copyBuf = (uint8_t*)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
                if (copyBuf) {
                    uint32_t off = 0;
                    while (off < part->size) {
                        uint32_t chunk = min<uint32_t>(32768, part->size - off);
                        if (esp_partition_read(part, off, copyBuf, chunk) != ESP_OK) break;
                        if (out.write(copyBuf, chunk) != chunk) break;
                        off += chunk;
                    }
                    heap_caps_free(copyBuf);
                }
                out.flush();
                out.close();
            }
        }
    }

    // 1. Check SD card first for germany.knav (prefers KNAVDE02)
    if (sStat.sdReady) {
        const char* candidates[] = {"/kronos/germany.knav", "/germany.knav", "/map/germany.knav"};
        for (const char* path : candidates) {
            if (SD.exists(path)) {
                File f = SD.open(path, FILE_READ);
                if (f && f.size() >= 40) {
                    KnavHeader h{};
                    size_t nr = f.read((uint8_t*)&h, sizeof(h));
                    if (nr >= 40 && (memcmp(h.magic, "KNAVDE02", 8) == 0 || memcmp(h.magic, "KNAVDE01", 8) == 0)) {
                        sHdr = h;
                        sIsV2 = (memcmp(h.magic, "KNAVDE02", 8) == 0);
                        strncpy(sMapPath, path, sizeof(sMapPath) - 1);
                        sStat.mapLoaded = true;
                        sStat.ultraLoaded = sIsV2 && (h.ultraDirOffset > 0);
                        sStat.mapPlaces = h.numPlaces;
                        sStat.mapRouteNodes = h.numRouteNodes;
                        sStat.mapPois = sIsV2 ? h.numPois : 0;
                        Serial.printf("[NAV] Loaded SD %s (%s): %u places, %u route nodes, %u POIs\n",
                                      path, sIsV2 ? "V2-ULTRA" : "V1", h.numPlaces, h.numRouteNodes, sStat.mapPois);
                        f.close();
                        return true;
                    }
                }
                if (f) f.close();
            }
        }
    }

    // 2. Check internal 9.875 MB QSPI Flash storage partition (0x610000) for KNAVDE02 / KNAVDE01
    if (part && part->size >= sizeof(KnavHeader)) {
        KnavHeader h{};
        if (esp_partition_read(part, 0, &h, sizeof(h)) == ESP_OK &&
            (memcmp(h.magic, "KNAVDE02", 8) == 0 || memcmp(h.magic, "KNAVDE01", 8) == 0)) {
            sHdr = h;
            sIsV2 = (memcmp(h.magic, "KNAVDE02", 8) == 0);
            sFlashPart = part;
            sUseFlashPart = true;
            sStat.mapLoaded = true;
            sStat.ultraLoaded = sIsV2 && (h.ultraDirOffset > 0);
            sStat.mapPlaces = h.numPlaces;
            sStat.mapRouteNodes = h.numRouteNodes;
            sStat.mapPois = sIsV2 ? h.numPois : 0;
            Serial.printf("[NAV] Loaded Flash %s (@0x%lx): %u places, %u route nodes, %u POIs, ultra=%d\n",
                          sIsV2 ? "KNAVDE02" : "KNAVDE01",
                          (unsigned long)part->address, h.numPlaces, h.numRouteNodes, sStat.mapPois,
                          (int)sStat.ultraLoaded);
            return true;
        }
    }

    Serial.println("[NAV] Using built-in Stuttgart & DE base map");
    return false;
}

void NavEngine::init() {
    memset(&sStat, 0, sizeof(sStat));
    sStat.mode = NAV_MODE_CAR;
    sStat.curLat = 48.7758; // Stuttgart Schlossplatz
    sStat.curLon = 9.1829;
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

    int bestIdx = 0;
    double bestDist = 1e9;
    for (size_t i = 0; i < sRoute.size(); i++) {
        double d = haversineKm(sStat.curLat, sStat.curLon, sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = (int)i;
        }
    }

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

            if (sStat.sdReady && sStat.sdWritable) {
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
                            uint8_t cls = (e.flags >> 2) & 0x0F;
                            uint32_t w = e.dist_hm;
                            if (pedMode) {
                                if (cls == FEAT_MOTORWAY || cls == 8) w *= 4;
                            } else {
                                if (cls == FEAT_MOTORWAY || cls == 8) w = (w * 6) / 10;
                                else if (cls == FEAT_PRIMARY || cls == 7) w = (w * 8) / 10;
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

    if (!routed) {
        const int steps = 16;
        for (int i = 0; i <= steps; i++) {
            double t = (double)i / steps;
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

    // 1. Live LoRa / Meshtastic peers with coordinates
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
            dst.rank = 0;
        }
    }
    if (query && strcmp(query, "@mesh") == 0) return count;

    // 2. User-saved 1-click POIs
    for (int i = sNumWaypoints - 1; i >= 0 && count < maxOut; i--) {
        if (matchPrefixCaseInsensitive(sWaypoints[i].name, query) ||
            (query && strcmp(query, "@poi") == 0)) {
            NavPlace& dst = out[count++];
            snprintf(dst.name, sizeof(dst.name), "%s", sWaypoints[i].name);
            dst.lat = sWaypoints[i].lat;
            dst.lon = sWaypoints[i].lon;
            dst.pop = 0;
            dst.rank = 5;
        }
    }

    // 3. Built-in Stuttgart + Adjacent Landkreise priority list when query is empty (TOP view)
    if (!query || !query[0]) {
        for (int i = 0; i < min(18, N_BUILTIN_CITIES) && count < maxOut; i++) {
            out[count++] = BUILTIN_CITIES[i];
        }
    }

    // 4. Search `germany.knav` places table if loaded
    if (sStat.mapLoaded && sHdr.numPlaces > 0 && count < maxOut) {
        MapReader mr;
        if (mr.open()) {
            uint32_t curOff = sHdr.placesOffset;
            const int batchSize = 64;
            DiskPlace batch[batchSize];
            uint32_t rem = min<uint32_t>(sHdr.numPlaces, (query && query[0]) ? sHdr.numPlaces : 500);
            while (rem > 0 && count < maxOut) {
                uint32_t n = min<uint32_t>(rem, batchSize);
                if (!mr.readAt(curOff, batch, n * sizeof(DiskPlace))) break;
                curOff += n * sizeof(DiskPlace);
                for (uint32_t i = 0; i < n && count < maxOut; i++) {
                    if (matchPrefixCaseInsensitive(batch[i].name, query)) {
                        // Avoid duplicating built-in Stuttgart entries already added
                        bool dup = false;
                        for (int k = 0; k < count; k++) {
                            if (strcmp(out[k].name, batch[i].name) == 0) { dup = true; break; }
                        }
                        if (dup) continue;
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
        }
    }

    // 5. Built-in German cities fallback
    for (int i = 0; i < N_BUILTIN_CITIES && count < maxOut; i++) {
        if (matchPrefixCaseInsensitive(BUILTIN_CITIES[i].name, query)) {
            bool dup = false;
            for (int k = 0; k < count; k++) {
                if (strcmp(out[k].name, BUILTIN_CITIES[i].name) == 0) { dup = true; break; }
            }
            if (!dup) out[count++] = BUILTIN_CITIES[i];
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
    int idx = sNumWaypoints;
    if (idx >= MAX_USER_WAYPOINTS) {
        // Shift oldest out
        for (int i = 0; i + 1 < MAX_USER_WAYPOINTS; i++) sWaypoints[i] = sWaypoints[i + 1];
        idx = MAX_USER_WAYPOINTS - 1;
    } else {
        sNumWaypoints++;
    }
    if (label && label[0]) {
        snprintf(sWaypoints[idx].name, sizeof(sWaypoints[idx].name), "%s #%d", label, idx + 1);
    } else {
        snprintf(sWaypoints[idx].name, sizeof(sWaypoints[idx].name), "POI #%d", idx + 1);
    }
    sWaypoints[idx].lat = sStat.curLat;
    sWaypoints[idx].lon = sStat.curLon;
    saveWaypointsToNvs();

    if (sStat.sdReady && sStat.sdWritable) {
        File f = SD.open("/waypoints.csv", FILE_APPEND);
        if (f) {
            f.printf("%lu,%s,%.6f,%.6f,%.1f\n",
                     (unsigned long)time(NULL), sWaypoints[idx].name,
                     sStat.curLat, sStat.curLon, GpsManager::altitudeM());
            f.close();
        }
    }
    nav::markDirty(nav::PG_POSITION);
    return true;
}

// =============================================================================
// Google-Maps-Style Vector Map Renderer (Buildings, Forests, Farmland, Streets, POIs)
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
    if ((x0 < -60 && x1 < -60) || (x0 >= w + 60 && x1 >= w + 60) ||
        (y0 < -60 && y1 < -60) || (y0 >= h + 60 && y1 >= h + 60)) return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int maxSteps = w + h + 240;
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
                    if (tx * tx + ty * ty <= r * r + 1)
                        putPx(buf, w, h, x0 + tx, y0 + ty, col);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Fast Scanline Polygon Fill for Forests, Parks, Farmland, Water Bodies, and Building Footprints
static void drawFilledPolygon(uint16_t* buf, int w, int h, const int16_t* px, const int16_t* py, int npts, uint16_t fillCol) {
    if (npts < 3) return;
    int minY = py[0], maxY = py[0];
    int minX = px[0], maxX = px[0];
    for (int i = 1; i < npts; i++) {
        if (py[i] < minY) minY = py[i];
        if (py[i] > maxY) maxY = py[i];
        if (px[i] < minX) minX = px[i];
        if (px[i] > maxX) maxX = px[i];
    }
    if (maxX < 0 || minX >= w || maxY < 0 || minY >= h) return;
    minY = max(0, minY);
    maxY = min(h - 1, maxY);

    int nodes[32];
    for (int y = minY; y <= maxY; y++) {
        int nNodes = 0;
        int j = npts - 1;
        for (int i = 0; i < npts; i++) {
            int yi = py[i], yj = py[j];
            if ((yi < y && yj >= y) || (yj < y && yi >= y)) {
                if (nNodes < 32) {
                    nodes[nNodes++] = px[i] + (y - yi) * (px[j] - px[i]) / (yj - yi);
                }
            }
            j = i;
        }
        // Insertion sort node X coordinates
        for (int i = 1; i < nNodes; i++) {
            int v = nodes[i];
            int k = i - 1;
            while (k >= 0 && nodes[k] > v) {
                nodes[k + 1] = nodes[k];
                k--;
            }
            nodes[k + 1] = v;
        }
        uint16_t* row = buf + y * w;
        for (int i = 0; i + 1 < nNodes; i += 2) {
            int x0 = max(0, nodes[i]);
            int x1 = min(w - 1, nodes[i + 1]);
            for (int x = x0; x <= x1; x++) {
                row[x] = fillCol;
            }
        }
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

static void drawRoundedBadge(uint16_t* buf, int w, int h, int cx, int cy, int r, uint16_t bgCol, uint16_t borderCol) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= (r - 1) * (r - 1)) putPx(buf, w, h, cx + dx, cy + dy, bgCol);
            else if (d2 <= r * r) putPx(buf, w, h, cx + dx, cy + dy, borderCol);
        }
    }
}

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
        }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 'Z') c = ' ';
        const uint8_t* glyph = MINI_FONT[c - 32];
        for (int colIdx = 0; colIdx < 5; colIdx++) {
            uint8_t bits = glyph[colIdx];
            for (int rowIdx = 0; rowIdx < 7; rowIdx++) {
                if (bits & (1 << rowIdx)) {
                    putPx(buf, w, h, cx + colIdx + 1, y + rowIdx + 1, haloCol);
                    putPx(buf, w, h, cx + colIdx - 1, y + rowIdx, haloCol);
                    putPx(buf, w, h, cx + colIdx, y + rowIdx, col);
                }
            }
        }
        cx += 6;
    }
}

// Tier: 0=Coarse (32x32 DE), 1=Fine (256x256 DE), 2=Ultra (128x128 Stuttgart+Landkreise)
static const uint8_t* getCachedCellData(MapReader& mr, int tier, int cellIdx, uint32_t& outLen) {
    int32_t key = (tier << 20) | cellIdx;
    sCacheClock++;
    for (int i = 0; i < CELL_CACHE_SLOTS; i++) {
        if (sCellCache[i].key == key) {
            sCellCache[i].lastUsed = sCacheClock;
            outLen = sCellCache[i].len;
            return sCellCache[i].data;
        }
    }

    uint32_t baseDir = (tier == 2) ? sHdr.ultraDirOffset :
                       (tier == 1) ? sHdr.fineDirOffset : sHdr.coarseDirOffset;
    if (baseDir == 0) { outLen = 0; return nullptr; }

    uint32_t dirOff = baseDir + cellIdx * sizeof(CellDirEntry);
    CellDirEntry de{};
    if (!mr.readAt(dirOff, &de, sizeof(de)) || de.length == 0 || de.length > 65536) {
        outLen = 0;
        return nullptr;
    }

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

    // 1. Dark Google-Maps-Night-style base canvas
    uint16_t bgCol = (zoom >= 13) ? rgb565(15, 20, 28) : rgb565(10, 14, 22);
    uint16_t gridCol = rgb565(20, 27, 40);
    for (int i = 0; i < w * h; i++) buf[i] = bgCol;

    double lonPerPx = 360.0 / (256.0 * pow(2.0, (double)zoom));
    double cosLat = cos(viewLat * M_PI / 180.0);
    if (cosLat < 0.2) cosLat = 0.2;
    double latPerPx = lonPerPx * cosLat;

    auto project = [&](double lat, double lon, int& sx, int& sy) {
        sx = (int)(w * 0.5 + (lon - viewLon) / lonPerPx);
        sy = (int)(h * 0.5 - (lat - viewLat) / latPerPx);
    };

    // Subtle coordinate grid at low/medium zoom
    if (zoom <= 13) {
        for (int gx = (w / 2) % 48; gx < w; gx += 48) {
            for (int y = 0; y < h; y += 2) buf[y * w + gx] = gridCol;
        }
        for (int gy = (h / 2) % 48; gy < h; gy += 48) {
            for (int x = 0; x < w; x += 2) buf[gy * w + x] = gridCol;
        }
    }

    // 2. Germany border outline at overview zoom levels
    if (zoom <= 11) {
        uint16_t borderCol = rgb565(56, 74, 102);
        for (int i = 0; i + 1 < N_BORDER_PTS; i++) {
            int x0, y0, x1, y1;
            project(DE_BORDER_PTS[i][0] * 1e-5, DE_BORDER_PTS[i][1] * 1e-5, x0, y0);
            project(DE_BORDER_PTS[i + 1][0] * 1e-5, DE_BORDER_PTS[i + 1][1] * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, borderCol, 2);
        }
    }

    double minLat = viewLat - (h * 0.58) * latPerPx;
    double maxLat = viewLat + (h * 0.58) * latPerPx;
    double minLon = viewLon - (w * 0.58) * lonPerPx;
    double maxLon = viewLon + (w * 0.58) * lonPerPx;

    // Helper lambda to render a cell's segments (used for Coarse, Fine, and Ultra Stuttgart tiers)
    auto renderCellBuffer = [&](const uint8_t* cdata, uint32_t clen, double baseLat, double baseLon,
                                double cellLatH, double cellLonW, bool passPolys) {
        if (!cdata || clen < 2) return;
        uint16_t nseg = *(const uint16_t*)cdata;
        size_t pos = 2;
        int16_t polyX[64], polyY[64];

        for (uint16_t s = 0; s < nseg && pos + 2 <= clen; s++) {
            uint8_t rawCls = cdata[pos++];
            uint8_t npts = cdata[pos++];
            if (pos + npts * 2 > clen) break;

            // Remap legacy V1 classes (0..8) to V2 FeatureClass (6..14)
            uint8_t cls = sIsV2 ? rawCls : (rawCls + 6);
            bool isPoly = (cls <= FEAT_BUILDING);

            if (passPolys != isPoly) {
                pos += npts * 2;
                continue;
            }
            if (!passPolys && zoom < 12 && (cls == FEAT_PATH || cls == FEAT_RESIDENTIAL)) {
                pos += npts * 2;
                continue;
            }
            if (passPolys && cls == FEAT_BUILDING && zoom < 13) {
                pos += npts * 2;
                continue;
            }

            int cnt = min<int>(npts, 64);
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
            pos += npts * 2;

            if (passPolys) {
                if (cls == FEAT_FARMLAND) {
                    // Agriculture / Farmland / Vineyards (warm earth-olive tint + field border)
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(26, 36, 20));
                    for (int k = 0; k + 1 < cnt; k++) {
                        drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], rgb565(40, 54, 30), 1);
                    }
                } else if (cls == FEAT_FOREST) {
                    // Forest / Woodland (rich dark emerald green + canopy edge)
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(16, 52, 28));
                    for (int k = 0; k + 1 < cnt; k++) {
                        drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], rgb565(28, 78, 44), 1);
                    }
                } else if (cls == FEAT_PARK) {
                    // City Park / Meadow
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(22, 68, 36));
                } else if (cls == FEAT_URBAN) {
                    // Urban residential/commercial block
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(22, 28, 38));
                } else if (cls == FEAT_WATER_POLY) {
                    // Lake / Neckar river polygon
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, rgb565(20, 88, 156));
                } else if (cls == FEAT_BUILDING) {
                    // 3D-style Building Footprint + Roof Outline!
                    uint16_t bFill = (zoom >= 15) ? rgb565(46, 54, 70) : rgb565(38, 45, 58);
                    uint16_t bEdge = (zoom >= 15) ? rgb565(82, 94, 118) : rgb565(60, 70, 88);
                    drawFilledPolygon(buf, w, h, polyX, polyY, cnt, bFill);
                    if (zoom >= 14) {
                        for (int k = 0; k + 1 < cnt; k++) {
                            drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], bEdge, 1);
                        }
                    }
                }
            } else {
                // Linear features: Roads, Paths, Railways, Waterways
                uint16_t coreCol = rgb565(100, 112, 132);
                uint16_t caseCol = 0;
                int coreW = 1, caseW = 0;

                switch (cls) {
                    case FEAT_WATER:
                        coreCol = rgb565(28, 138, 220);
                        coreW = (zoom >= 13) ? 3 : 2;
                        break;
                    case FEAT_BOUNDARY:
                        coreCol = rgb565(75, 95, 125);
                        coreW = 1;
                        break;
                    case FEAT_RAIL:
                        coreCol = rgb565(115, 125, 155);
                        coreW = (zoom >= 14) ? 2 : 1;
                        break;
                    case FEAT_PATH:
                        // Footpath / Cycleway / Forest Trail
                        coreCol = rgb565(92, 165, 112);
                        coreW = (zoom >= 15) ? 2 : 1;
                        break;
                    case FEAT_RESIDENTIAL:
                        // Google-Maps-style Residential Street with dark casing
                        coreCol = rgb565(132, 145, 168);
                        caseCol = rgb565(32, 38, 50);
                        coreW = (zoom >= 16) ? 4 : (zoom >= 15) ? 3 : (zoom >= 14) ? 2 : 1;
                        caseW = (zoom >= 14) ? (coreW + 2) : 0;
                        break;
                    case FEAT_TERTIARY:
                        coreCol = rgb565(158, 170, 192);
                        caseCol = rgb565(34, 40, 52);
                        coreW = (zoom >= 15) ? 4 : (zoom >= 13) ? 2 : 1;
                        caseW = (zoom >= 13) ? (coreW + 2) : 0;
                        break;
                    case FEAT_SECONDARY:
                        coreCol = rgb565(190, 202, 222);
                        caseCol = rgb565(38, 44, 58);
                        coreW = (zoom >= 15) ? 4 : (zoom >= 12) ? 2 : 1;
                        caseW = (zoom >= 12) ? (coreW + 2) : 0;
                        break;
                    case FEAT_PRIMARY:
                        // Bundesstraße (Google Maps warm yellow-gold)
                        coreCol = rgb565(245, 192, 38);
                        caseCol = rgb565(70, 52, 12);
                        coreW = (zoom >= 14) ? 4 : 2;
                        caseW = (zoom >= 12) ? (coreW + 2) : 0;
                        break;
                    case FEAT_MOTORWAY:
                        // Autobahn (bright orange-amber)
                        coreCol = rgb565(255, 110, 28);
                        caseCol = rgb565(85, 34, 10);
                        coreW = (zoom >= 13) ? 4 : (zoom >= 9) ? 2 : 1;
                        caseW = (zoom >= 11) ? (coreW + 2) : 0;
                        break;
                }

                if (caseW > 0) {
                    for (int k = 0; k + 1 < cnt; k++) {
                        drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], caseCol, caseW);
                    }
                }
                for (int k = 0; k + 1 < cnt; k++) {
                    drawLine(buf, w, h, polyX[k], polyY[k], polyX[k + 1], polyY[k + 1], coreCol, coreW);
                }
            }
        }
    };

    // 3. Render vector tiles from `germany.knav` (SD or Flash)
    if (sStat.mapLoaded) {
        MapReader mr;
        if (mr.open()) {
            bool inStuttgartMetro = sStat.ultraLoaded && (zoom >= 13) &&
                                    (maxLat >= STU_MIN_LAT && minLat <= STU_MAX_LAT &&
                                     maxLon >= STU_MIN_LON && minLon <= STU_MAX_LON);

            if (inStuttgartMetro) {
                double latSpan = STU_MAX_LAT - STU_MIN_LAT;
                double lonSpan = STU_MAX_LON - STU_MIN_LON;
                int cy0 = std::clamp((int)((minLat - STU_MIN_LAT) / latSpan * ULTRA_NY), 0, ULTRA_NY - 1);
                int cy1 = std::clamp((int)((maxLat - STU_MIN_LAT) / latSpan * ULTRA_NY), 0, ULTRA_NY - 1);
                int cx0 = std::clamp((int)((minLon - STU_MIN_LON) / lonSpan * ULTRA_NX), 0, ULTRA_NX - 1);
                int cx1 = std::clamp((int)((maxLon - STU_MIN_LON) / lonSpan * ULTRA_NX), 0, ULTRA_NX - 1);

                if (cy1 - cy0 <= 6 && cx1 - cx0 <= 6) {
                    double cellLatH = latSpan / ULTRA_NY;
                    double cellLonW = lonSpan / ULTRA_NX;
                    // Pass 1: Filled polygons (Farmland, Forests, Parks, Water, Urban, 3D Buildings)
                    for (int cy = cy0; cy <= cy1; cy++) {
                        for (int cx = cx0; cx <= cx1; cx++) {
                            uint32_t clen = 0;
                            const uint8_t* cdata = getCachedCellData(mr, 2, cy * ULTRA_NX + cx, clen);
                            renderCellBuffer(cdata, clen, STU_MIN_LAT + cy * cellLatH,
                                             STU_MIN_LON + cx * cellLonW, cellLatH, cellLonW, true);
                        }
                    }
                    // Pass 2: Cased streets, footpaths, railways, waterways
                    for (int cy = cy0; cy <= cy1; cy++) {
                        for (int cx = cx0; cx <= cx1; cx++) {
                            uint32_t clen = 0;
                            const uint8_t* cdata = getCachedCellData(mr, 2, cy * ULTRA_NX + cx, clen);
                            renderCellBuffer(cdata, clen, STU_MIN_LAT + cy * cellLatH,
                                             STU_MIN_LON + cx * cellLonW, cellLatH, cellLonW, false);
                        }
                    }
                }
            }

            // Also render Fine (256x256) or Coarse (32x32) Germany grid if outside Ultra coverage or zoomed out
            if (!inStuttgartMetro) {
                bool useFine = (zoom >= 10);
                int nx = useFine ? FINE_NX : COARSE_NX;
                int ny = useFine ? FINE_NY : COARSE_NY;
                double latSpan = DE_MAX_LAT - DE_MIN_LAT;
                double lonSpan = DE_MAX_LON - DE_MIN_LON;

                int cy0 = std::clamp((int)((minLat - DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
                int cy1 = std::clamp((int)((maxLat - DE_MIN_LAT) / latSpan * ny), 0, ny - 1);
                int cx0 = std::clamp((int)((minLon - DE_MIN_LON) / lonSpan * nx), 0, nx - 1);
                int cx1 = std::clamp((int)((maxLon - DE_MIN_LON) / lonSpan * nx), 0, nx - 1);

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
                int tier = useFine ? 1 : 0;

                for (int pass = 0; pass < 2; pass++) {
                    bool passPolys = (pass == 0);
                    for (int cy = cy0; cy <= cy1; cy++) {
                        for (int cx = cx0; cx <= cx1; cx++) {
                            uint32_t clen = 0;
                            const uint8_t* cdata = getCachedCellData(mr, tier, cy * nx + cx, clen);
                            renderCellBuffer(cdata, clen, DE_MIN_LAT + cy * cellLatH,
                                             DE_MIN_LON + cx * cellLonW, cellLatH, cellLonW, passPolys);
                        }
                    }
                }
            }

            // 4. Render POIs & Street Names from `poisOffset` at street zoom (zoom >= 13)!
            if (sIsV2 && sHdr.numPois > 0 && sHdr.poisOffset > 0 && zoom >= 13) {
                int32_t minLat5 = (int32_t)(minLat * 1e5);
                int32_t maxLat5 = (int32_t)(maxLat * 1e5);
                int32_t minLon5 = (int32_t)(minLon * 1e5);
                int32_t maxLon5 = (int32_t)(maxLon * 1e5);

                // Binary search first POI with lat_e5 >= minLat5
                uint32_t lo = 0, hi = sHdr.numPois;
                while (lo < hi) {
                    uint32_t mid = (lo + hi) / 2;
                    DiskPoi probe{};
                    if (!mr.readAt(sHdr.poisOffset + mid * sizeof(DiskPoi), &probe, sizeof(probe))) break;
                    if (probe.lat_e5 < minLat5) lo = mid + 1;
                    else hi = mid;
                }

                // Track placed label boxes to prevent overlapping street/POI labels
                struct LabelBox { int x0, y0, x1, y1; };
                LabelBox placed[28];
                int nPlaced = 0;

                const int batchN = 48;
                DiskPoi batch[batchN];
                uint32_t idx = lo;
                bool done = false;
                while (idx < sHdr.numPois && !done && nPlaced < 26) {
                    uint32_t cnt = min<uint32_t>(batchN, sHdr.numPois - idx);
                    if (!mr.readAt(sHdr.poisOffset + idx * sizeof(DiskPoi), batch, cnt * sizeof(DiskPoi))) break;
                    idx += cnt;

                    for (uint32_t i = 0; i < cnt; i++) {
                        const DiskPoi& p = batch[i];
                        if (p.lat_e5 > maxLat5) { done = true; break; }
                        if (p.lon_e5 < minLon5 || p.lon_e5 > maxLon5) continue;
                        if (p.type == 7 && zoom < 14) continue; // Street names at zoom >= 14

                        int sx, sy;
                        project(p.lat_e5 * 1e-5, p.lon_e5 * 1e-5, sx, sy);
                        if (sx < 14 || sx >= w - 50 || sy < 14 || sy >= h - 16) continue;

                        int txtW = min<int>((int)strlen(p.name), 15) * 6 + 14;
                        int bx0 = sx - 6, by0 = sy - 7, bx1 = sx + txtW, by1 = sy + 9;
                        bool overlap = false;
                        for (int b = 0; b < nPlaced; b++) {
                            if (!(bx1 < placed[b].x0 || bx0 > placed[b].x1 ||
                                  by1 < placed[b].y0 || by0 > placed[b].y1)) {
                                overlap = true;
                                break;
                            }
                        }
                        if (overlap) continue;
                        if (nPlaced < 28) placed[nPlaced++] = {bx0, by0, bx1, by1};

                        if (p.type == 7) {
                            // Street Name Label
                            drawMapText(buf, w, h, sx, sy - 3, p.name, rgb565(185, 200, 222), rgb565(10, 14, 22), 16);
                        } else {
                            // Google-Maps-style POI Icon Badge + Label
                            uint16_t badgeCol = rgb565(50, 140, 250);
                            char sym[2] = {'*', 0};
                            switch (p.type) {
                                case 0: badgeCol = rgb565(30, 185, 95);  sym[0] = 'S'; break; // S-Bahn/U-Bahn/Transit
                                case 1: badgeCol = rgb565(40, 130, 245); sym[0] = 'P'; break; // Parking
                                case 2: badgeCol = rgb565(245, 140, 30); sym[0] = 'T'; break; // Tankstelle
                                case 3: badgeCol = rgb565(240, 155, 45); sym[0] = 'R'; break; // Restaurant/Cafe
                                case 4: badgeCol = rgb565(40, 185, 220); sym[0] = 'M'; break; // Supermarkt/Shop
                                case 5: badgeCol = rgb565(245, 65, 75);  sym[0] = '+'; break; // Klinik/Apotheke
                                case 6: badgeCol = rgb565(215, 175, 45); sym[0] = '*'; break; // Sehenswürdigkeit
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

            mr.close();
        }
    }

    // 5. Draw GPX recorded breadcrumb trail (amber line)
    if (sGpxTrail.size() >= 2) {
        uint16_t gpxCol = rgb565(255, 200, 30);
        for (size_t i = 0; i + 1 < sGpxTrail.size(); i++) {
            int x0, y0, x1, y1;
            project(sGpxTrail[i].lat_e5 * 1e-5, sGpxTrail[i].lon_e5 * 1e-5, x0, y0);
            project(sGpxTrail[i + 1].lat_e5 * 1e-5, sGpxTrail[i + 1].lon_e5 * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, gpxCol, 2);
        }
    }

    // 6. Draw Active Navigation Route Polyline (glowing cyan/lime)
    if (sStat.activeRoute && sRoute.size() >= 2) {
        uint16_t routeHalo = rgb565(0, 90, 110);
        uint16_t routeCore = (sStat.mode == NAV_MODE_PED) ? rgb565(50, 235, 110) : rgb565(0, 245, 212);
        for (size_t i = 0; i + 1 < sRoute.size(); i++) {
            int x0, y0, x1, y1;
            project(sRoute[i].lat_e5 * 1e-5, sRoute[i].lon_e5 * 1e-5, x0, y0);
            project(sRoute[i + 1].lat_e5 * 1e-5, sRoute[i + 1].lon_e5 * 1e-5, x1, y1);
            drawLine(buf, w, h, x0, y0, x1, y1, routeHalo, 4);
            drawLine(buf, w, h, x0, y0, x1, y1, routeCore, 2);
        }
        int dx, dy;
        project(sStat.destLat, sStat.destLon, dx, dy);
        drawFilledCircle(buf, w, h, dx, dy, 6, rgb565(255, 60, 60));
        drawRing(buf, w, h, dx, dy, 8, rgb565(255, 255, 255));
        drawMapText(buf, w, h, dx + 10, dy - 4, sStat.destName, rgb565(255, 120, 120), rgb565(0, 0, 0));
    }

    // 7. Draw User-Saved 1-Click POI Waypoints
    for (int i = 0; i < sNumWaypoints; i++) {
        int wx, wy;
        project(sWaypoints[i].lat, sWaypoints[i].lon, wx, wy);
        if (wx >= -10 && wx < w + 10 && wy >= -10 && wy < h + 10) {
            drawRoundedBadge(buf, w, h, wx, wy, 5, rgb565(255, 195, 20), rgb565(255, 255, 255));
            drawMapText(buf, w, h, wx + 8, wy - 3, sWaypoints[i].name, rgb565(255, 215, 60), rgb565(0, 0, 0), 12);
        }
    }

    // 8. Draw City / Town Names & Dots
    uint16_t cityDotCol = rgb565(225, 235, 250);
    uint16_t cityTxtCol = rgb565(210, 225, 245);
    for (int i = 0; i < N_BUILTIN_CITIES; i++) {
        if (zoom <= 7 && BUILTIN_CITIES[i].rank > 1 && i > 0) continue;
        int sx, sy;
        project(BUILTIN_CITIES[i].lat, BUILTIN_CITIES[i].lon, sx, sy);
        if (sx >= 10 && sx < w - 45 && sy >= 10 && sy < h - 10) {
            drawFilledCircle(buf, w, h, sx, sy, 2, cityDotCol);
            drawMapText(buf, w, h, sx + 5, sy - 3, BUILTIN_CITIES[i].name, cityTxtCol, rgb565(5, 8, 14), 16);
        }
    }

    // 9. Draw Live LoRa / Meshtastic Peer Nodes on the Map!
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

    // 10. Draw Current Position Marker ("You are here" GPS / LoRa Mesh Assisted)
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

    // 11. Scale bar in bottom-left corner of map canvas
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

bool NavEngine::beginSdUpload(const char* path, uint32_t totalSize) {
    if (!sStat.sdReady && !instance.installSD()) return false;
    sStat.sdReady = true;
    clearCellCache();
    digitalWrite(LORA_CS, HIGH);
    digitalWrite(NFC_CS, HIGH);
    SD.mkdir("/kronos");
    sUploadFile = SD.open(path, FILE_WRITE, true);
    if (!sUploadFile) {
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

