// =============================================================================
//  WatchOS Ultra - KNAV offline map container ("germany.knav")
//
//  Single source of truth for the on-disk layout, shared by
//    * the firmware reader        (src/NavEngine.cpp)
//    * the host-side map compiler (tools/build_germany_map.cpp)
//
//  Plain C++17, no Arduino / ESP-IDF dependencies. All structures are packed
//  and little-endian (both the ESP32-S3 and x86 hosts are little-endian; the
//  builder does not byte-swap).
//
//  File layout (all offsets are absolute byte offsets from the file start):
//
//    KnavHeader                       52 bytes (V2) / 72 bytes (V3)
//    DiskPlace[numPlaces]             48 bytes each, sorted by rank asc, pop desc
//    DiskRouteNode[numRouteNodes]     16 bytes each
//    DiskRouteEdge[numRouteEdges]      8 bytes each, grouped per node
//    CellDirEntry[COARSE_NX*COARSE_NY] + cell payloads   (coarseDirOffset)
//    CellDirEntry[FINE_NX*FINE_NY]     + cell payloads   (fineDirOffset)
//    CellDirEntry[ULTRA_NX*ULTRA_NY]   + cell payloads   (ultraDirOffset, may be 0)
//    DiskPoi[numPois]                 36 bytes each, sorted by lat_e5 asc
//
//  Cell payload:
//    uint16_t nseg
//    nseg x { uint8_t cls (FeatureClass); uint8_t npts; npts x { uint8_t qx; uint8_t qy; } }
//    qx/qy are the point position inside the cell, 0..255 from the cell's
//    south-west corner (qx east, qy north).
//
//  Versions:
//    "KNAVDE02": 52-byte header, no fileSize/bbox. The firmware computes the
//                real data end from the directories.
//    "KNAVDE03": 72-byte header = V2 fields + fileSize + data bounding box.
//                Everything after the header is byte-identical to V2, so a V3
//                reader handles V2 files by reading only the first 52 bytes.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// --- Magic ------------------------------------------------------------------
#define KNAV_MAGIC_V2        "KNAVDE02"
#define KNAV_MAGIC_V3        "KNAVDE03"
#define KNAV_MAGIC           KNAV_MAGIC_V3     // written by the current builder
#define KNAV_MAGIC_LEN       8

// --- Grid geometry (fixed by the format, not by the data) -------------------
// Germany bounding box used by the coarse and fine grids.
static constexpr double KNAV_DE_MIN_LAT = 47.20;
static constexpr double KNAV_DE_MAX_LAT = 55.10;
static constexpr double KNAV_DE_MIN_LON = 5.80;
static constexpr double KNAV_DE_MAX_LON = 15.10;

// Stuttgart metro bounding box used by the ultra grid.
static constexpr double KNAV_STG_MIN_LAT = 48.42;
static constexpr double KNAV_STG_MAX_LAT = 49.18;
static constexpr double KNAV_STG_MIN_LON = 8.75;
static constexpr double KNAV_STG_MAX_LON = 9.75;

static constexpr int KNAV_COARSE_NX = 32;   // tier 0: whole of Germany, ~29 x 27 km cells
static constexpr int KNAV_COARSE_NY = 32;
static constexpr int KNAV_FINE_NX   = 256;  // tier 1: whole of Germany, ~3.6 x 3.4 km cells
static constexpr int KNAV_FINE_NY   = 256;
static constexpr int KNAV_ULTRA_NX  = 128;  // tier 2: Stuttgart metro, ~0.57 x 0.66 km cells
static constexpr int KNAV_ULTRA_NY  = 128;

static constexpr uint32_t KNAV_COARSE_CELLS = (uint32_t)KNAV_COARSE_NX * KNAV_COARSE_NY;
static constexpr uint32_t KNAV_FINE_CELLS   = (uint32_t)KNAV_FINE_NX * KNAV_FINE_NY;
static constexpr uint32_t KNAV_ULTRA_CELLS  = (uint32_t)KNAV_ULTRA_NX * KNAV_ULTRA_NY;

// Maximum cell payload the firmware reads (hard limit, larger cells are skipped
// with a warning). The builder targets KNAV_CELL_TARGET_BYTES and simplifies /
// drops low-priority classes until a cell fits.
static constexpr uint32_t KNAV_MAX_CELL_BYTES    = 65536;
static constexpr uint32_t KNAV_CELL_TARGET_BYTES = 60 * 1024;

// Sanity limits for header fields (protects the firmware against truncated or
// corrupted uploads; nothing real comes anywhere near these).
static constexpr uint32_t KNAV_MAX_PLACES      = 200000;
static constexpr uint32_t KNAV_MAX_ROUTE_NODES = 2000000;
static constexpr uint32_t KNAV_MAX_ROUTE_EDGES = 4000000;
static constexpr uint32_t KNAV_MAX_POIS        = 500000;

// --- Feature classes (cell segment "cls" byte) ------------------------------
enum FeatureClass : uint8_t {
    // Area / polygon features (0..5), drawn in pass 1
    FEAT_FARMLAND    = 0,   // landuse=farmland, meadow, orchard, vineyard
    FEAT_FOREST      = 1,   // landuse=forest, natural=wood
    FEAT_PARK        = 2,   // leisure=park, garden, landuse=grass
    FEAT_URBAN       = 3,   // landuse=residential, commercial, industrial, retail
    FEAT_WATER_POLY  = 4,   // natural=water, waterway=riverbank, reservoir
    FEAT_BUILDING    = 5,   // building=*
    // Linear features (6..14), drawn in pass 2
    FEAT_WATER       = 6,   // waterway=river, canal, stream
    FEAT_BOUNDARY    = 7,   // coastline / administrative
    FEAT_RAIL        = 8,   // rail, light_rail, subway, tram
    FEAT_PATH        = 9,   // footway, cycleway, path, track, pedestrian, steps
    FEAT_RESIDENTIAL = 10,  // residential, living_street, unclassified, service
    FEAT_TERTIARY    = 11,  // tertiary (+link)
    FEAT_SECONDARY   = 12,  // secondary (+link)
    FEAT_PRIMARY     = 13,  // trunk, primary (+link)
    FEAT_MOTORWAY    = 14,  // motorway (+link)
    FEAT_CLASS_COUNT = 15
};
static constexpr uint8_t FEAT_LAST_POLYGON = FEAT_BUILDING;   // cls <= 5 is an area

// --- POI categories (DiskPoi::category) -------------------------------------
// Builder numbering. 0 is never written to disk.
enum PoiCategory : uint8_t {
    POI_NONE       = 0,
    POI_TRANSIT    = 1, // S-Bahn, U-Bahn, Bahnhof, Haltestelle
    POI_PARKING    = 2, // Parkplatz, Parkhaus
    POI_FUEL       = 3, // Tankstelle, Ladesaeule
    POI_FOOD       = 4, // Restaurant, Cafe, Biergarten, Baeckerei
    POI_SHOP       = 5, // Supermarkt, Einkaufszentrum
    POI_HEALTH     = 6, // Krankenhaus, Apotheke, Arzt
    POI_SIGHT      = 7, // Sehenswuerdigkeit, Aussichtspunkt, Museum, Burg/Schloss
    POI_STREET     = 8, // Strassennamen-Label (not a POI badge)
    POI_CATEGORY_COUNT = 9
};

// --- Route edge flags (DiskRouteEdge::flags) --------------------------------
//   bit 0      car routable
//   bit 1      pedestrian / bicycle routable
//   bits 2..5  roadRank 0..4 (0 = residential/service, 1 = tertiary,
//              2 = secondary, 3 = primary/trunk, 4 = motorway)
//   bits 6..7  reserved (0)
static constexpr uint8_t KNAV_EDGE_CAR       = 0x01;
static constexpr uint8_t KNAV_EDGE_PED       = 0x02;
static constexpr uint8_t KNAV_EDGE_RANK_SHIFT = 2;
static constexpr uint8_t KNAV_EDGE_RANK_MASK  = 0x0F;
static constexpr uint8_t KNAV_ROAD_RANK_MAX   = 4;

static inline uint8_t knavEdgeRoadRank(uint8_t flags) {
    uint8_t r = (uint8_t)((flags >> KNAV_EDGE_RANK_SHIFT) & KNAV_EDGE_RANK_MASK);
    return r > KNAV_ROAD_RANK_MAX ? KNAV_ROAD_RANK_MAX : r;
}
static inline uint8_t knavEdgeFlags(bool car, bool ped, uint8_t roadRank) {
    if (roadRank > KNAV_ROAD_RANK_MAX) roadRank = KNAV_ROAD_RANK_MAX;
    return (uint8_t)((car ? KNAV_EDGE_CAR : 0) | (ped ? KNAV_EDGE_PED : 0) |
                     (uint8_t)(roadRank << KNAV_EDGE_RANK_SHIFT));
}
static inline uint8_t knavRoadRankForClass(uint8_t cls) {
    return (cls >= FEAT_MOTORWAY)  ? 4 :
           (cls == FEAT_PRIMARY)   ? 3 :
           (cls == FEAT_SECONDARY) ? 2 :
           (cls == FEAT_TERTIARY)  ? 1 : 0;
}

// --- Place ranks (DiskPlace::rank) ------------------------------------------
//   1 = city, 2 = town, 3 = suburb/borough, 4 = village/quarter/hamlet
//   (0 and 5 are used by the firmware for mesh peers / saved waypoints only
//    and never appear on disk.)

// --- On-disk structures -----------------------------------------------------
#pragma pack(push, 1)

struct KnavHeader {
    char     magic[KNAV_MAGIC_LEN];  // "KNAVDE02" / "KNAVDE03"
    uint32_t numPlaces;
    uint32_t placesOffset;
    uint32_t numRouteNodes;
    uint32_t numRouteEdges;
    uint32_t routeNodesOffset;
    uint32_t routeEdgesOffset;
    uint32_t coarseDirOffset;        // 0 = tier absent
    uint32_t fineDirOffset;          // 0 = tier absent
    uint32_t ultraDirOffset;         // 0 = tier absent
    uint32_t numPois;
    uint32_t poisOffset;
    // ---- V3 additions (not present in a V2 file) ----
    uint32_t fileSize;               // total file length in bytes (0 in V2 -> compute from directories)
    int32_t  bboxMinLat_e5;          // bounding box of the data actually contained
    int32_t  bboxMinLon_e5;
    int32_t  bboxMaxLat_e5;
    int32_t  bboxMaxLon_e5;
};
static constexpr size_t KNAV_HEADER_V2_SIZE = 52;
static constexpr size_t KNAV_HEADER_V3_SIZE = 72;

struct DiskPlace {
    int32_t  lat_e5;
    int32_t  lon_e5;
    uint32_t pop;
    uint8_t  rank;                   // 1..4, see above
    char     name[35];               // UTF-8 (ASCII + 2-byte umlauts), NUL padded
};

struct DiskPoi {
    int32_t  lat_e5;
    int32_t  lon_e5;
    uint8_t  category;               // PoiCategory
    char     name[27];               // UTF-8, NUL padded
};

struct DiskRouteNode {
    int32_t  lat_e5;
    int32_t  lon_e5;
    uint32_t firstEdge;              // index into the edge table
    uint16_t edgeCount;
    uint16_t pad;                    // 0
};

struct DiskRouteEdge {
    uint32_t toNode;                 // index into the node table
    uint16_t dist_hm;                // edge length in hectometres (km * 10), >= 1
    uint8_t  flags;                  // see KNAV_EDGE_*
    uint8_t  pad;                    // 0
};

struct CellDirEntry {
    uint32_t offset;                 // absolute file offset of the payload, 0 = empty cell
    uint32_t length;                 // payload length in bytes, 0 = empty cell
};

#pragma pack(pop)

static_assert(sizeof(KnavHeader)    == KNAV_HEADER_V3_SIZE, "KnavHeader must be 72 bytes");
static_assert(sizeof(DiskPlace)     == 48, "DiskPlace must be 48 bytes");
static_assert(sizeof(DiskPoi)       == 36, "DiskPoi must be 36 bytes");
static_assert(sizeof(DiskRouteNode) == 16, "DiskRouteNode must be 16 bytes");
static_assert(sizeof(DiskRouteEdge) == 8,  "DiskRouteEdge must be 8 bytes");
static_assert(sizeof(CellDirEntry)  == 8,  "CellDirEntry must be 8 bytes");
static_assert(KNAV_HEADER_V2_SIZE == 8 + 11 * 4, "V2 header is magic + 11 uint32");

// --- Small helpers usable on both sides -------------------------------------
static inline bool knavMagicIsV2(const char* m) {
    for (int i = 0; i < KNAV_MAGIC_LEN; i++) if (m[i] != KNAV_MAGIC_V2[i]) return false;
    return true;
}
static inline bool knavMagicIsV3(const char* m) {
    for (int i = 0; i < KNAV_MAGIC_LEN; i++) if (m[i] != KNAV_MAGIC_V3[i]) return false;
    return true;
}
static inline size_t knavDirEntries(int tier) {
    return tier == 2 ? KNAV_ULTRA_CELLS : tier == 1 ? KNAV_FINE_CELLS : KNAV_COARSE_CELLS;
}
