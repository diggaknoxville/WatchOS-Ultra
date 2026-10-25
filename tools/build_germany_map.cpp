// =============================================================================
//  build_germany_map - OSM PBF -> KNAV map container for WatchOS Ultra
//
//  Compiles a Germany-wide base map (motorways .. tertiary, rail, rivers,
//  coastline) plus an ultra-detailed Stuttgart metro tier (residential
//  streets, paths, buildings, landuse, POIs, street labels) and a routing
//  graph into one "germany.knav" file. The binary layout lives in
//  ../include/MapFormat.h and is shared with the firmware.
//
//  Build:   g++ -O3 -std=c++17 -pthread tools/build_germany_map.cpp -lz -o build_germany_map
//  Usage:   build_germany_map <input.osm.pbf> <output.knav> [--flash] [--bbox minLat,minLon,maxLat,maxLon]
//             --flash   compact variant that fits the 9.875 MB internal flash partition
//             --bbox    only keep features inside the box (regional extracts / tests)
//
//  Output is deterministic for a given input and option set.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <zlib.h>

#include "../include/MapFormat.h"

#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ----------------------------------------------------------------------------
// Options
// ----------------------------------------------------------------------------
struct Options {
    std::string pbfPath;
    std::string outPath;
    bool flashMode = false;
    bool hasBbox = false;
    double bbMinLat = 0, bbMinLon = 0, bbMaxLat = 0, bbMaxLon = 0;
};

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage: %s <input.osm.pbf> <output.knav> [--flash] [--bbox minLat,minLon,maxLat,maxLon]\n"
            "  --flash   compact build for the internal flash partition\n"
            "  --bbox    keep only features inside the given box\n", argv0);
}

static bool parseArgs(int argc, char** argv, Options& o) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--flash" || a == "--compact") {
            o.flashMode = true;
        } else if (a == "--bbox") {
            if (i + 1 >= argc) return false;
            if (sscanf(argv[++i], "%lf,%lf,%lf,%lf", &o.bbMinLat, &o.bbMinLon, &o.bbMaxLat, &o.bbMaxLon) != 4) return false;
            if (o.bbMinLat >= o.bbMaxLat || o.bbMinLon >= o.bbMaxLon) return false;
            o.hasBbox = true;
        } else if (a == "-h" || a == "--help") {
            return false;
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() != 2) return false;
    o.pbfPath = positional[0];
    o.outPath = positional[1];
    return true;
}

// ----------------------------------------------------------------------------
// Protobuf helpers
// ----------------------------------------------------------------------------
static uint64_t readVarint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0; int shift = 0;
    while (p < end && shift < 64) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}
static int64_t zigZag(uint64_t n) { return (int64_t)((n >> 1) ^ (~(n & 1) + 1)); }

// Skips one field of wire type wt. Returns false on malformed input.
static bool skipField(uint32_t wt, const uint8_t*& p, const uint8_t* end) {
    switch (wt) {
        case 0: readVarint(p, end); return true;
        case 1: p += 8; return p <= end;
        case 2: { uint64_t l = readVarint(p, end); if ((uint64_t)(end - p) < l) return false; p += l; return true; }
        case 5: p += 4; return p <= end;
        default: return false;
    }
}

struct BlockIndex {
    int64_t offset;     // file offset of the 4-byte header length
    uint32_t datasize;  // blob size
    uint8_t types;      // bit0 nodes/dense, bit1 ways, bit2 relations
};
static constexpr uint8_t BT_NODES = 1, BT_WAYS = 2, BT_RELATIONS = 4;

static bool decompressBlock(FILE* f, const BlockIndex& bi, std::vector<uint8_t>& rawOut) {
    fseeko(f, bi.offset, SEEK_SET);
    uint8_t hlenBuf[4];
    if (fread(hlenBuf, 1, 4, f) != 4) return false;
    uint32_t hlen = (hlenBuf[0] << 24) | (hlenBuf[1] << 16) | (hlenBuf[2] << 8) | hlenBuf[3];
    fseeko(f, hlen, SEEK_CUR);
    std::vector<uint8_t> bbuf(bi.datasize);
    if (fread(bbuf.data(), 1, bi.datasize, f) != bi.datasize) return false;
    const uint8_t *p = bbuf.data(), *end = p + bi.datasize;
    uint32_t rawSize = 0, zlen = 0, rlen = 0;
    const uint8_t *zptr = nullptr, *rptr = nullptr;
    while (p < end) {
        uint64_t tag = readVarint(p, end);
        uint32_t field = (uint32_t)(tag >> 3), wt = tag & 7;
        if (field == 1 && wt == 2) { rlen = (uint32_t)readVarint(p, end); rptr = p; p += rlen; }
        else if (field == 2 && wt == 0) rawSize = (uint32_t)readVarint(p, end);
        else if (field == 3 && wt == 2) { zlen = (uint32_t)readVarint(p, end); zptr = p; p += zlen; }
        else if (!skipField(wt, p, end)) break;
    }
    if (zptr && rawSize > 0 && zptr + zlen <= end) {
        rawOut.resize(rawSize);
        uLongf destLen = rawSize;
        return uncompress(rawOut.data(), &destLen, zptr, zlen) == Z_OK;
    } else if (rptr && rlen > 0 && rptr + rlen <= end) {
        rawOut.assign(rptr, rptr + rlen);
        return true;
    }
    return false;
}

static void parseStringTable(const uint8_t* p, const uint8_t* end, std::vector<std::string>& st) {
    st.clear();
    while (p < end) {
        uint64_t tag = readVarint(p, end);
        uint32_t field = (uint32_t)(tag >> 3), wt = tag & 7;
        if (field == 1 && wt == 2) {
            uint64_t l = readVarint(p, end);
            if ((uint64_t)(end - p) < l) break;
            st.emplace_back((const char*)p, l);
            p += l;
        } else if (!skipField(wt, p, end)) break;
    }
}

// Splits a PrimitiveBlock into string table, groups and dense-node params.
struct PrimitiveBlockView {
    std::vector<std::string> st;
    std::vector<std::pair<const uint8_t*, uint64_t>> groups;
    int32_t granularity = 100;
    int64_t latOffset = 0, lonOffset = 0;
};
static void parsePrimitiveBlock(const std::vector<uint8_t>& raw, PrimitiveBlockView& v) {
    v.st.clear(); v.groups.clear();
    v.granularity = 100; v.latOffset = v.lonOffset = 0;
    const uint8_t *p = raw.data(), *end = p + raw.size();
    while (p < end) {
        uint64_t tag = readVarint(p, end);
        uint32_t field = (uint32_t)(tag >> 3), wt = tag & 7;
        if (field == 1 && wt == 2) { uint64_t l = readVarint(p, end); if ((uint64_t)(end - p) < l) break; parseStringTable(p, p + l, v.st); p += l; }
        else if (field == 2 && wt == 2) { uint64_t l = readVarint(p, end); if ((uint64_t)(end - p) < l) break; v.groups.push_back({p, l}); p += l; }
        else if (field == 17 && wt == 0) v.granularity = (int32_t)readVarint(p, end);
        else if (field == 19 && wt == 0) v.latOffset = (int64_t)readVarint(p, end);
        else if (field == 20 && wt == 0) v.lonOffset = (int64_t)readVarint(p, end);
        else if (!skipField(wt, p, end)) break;
    }
}

// Element types contained in a block (from the fields of every PrimitiveGroup).
static uint8_t classifyBlock(const std::vector<uint8_t>& raw) {
    PrimitiveBlockView v;
    parsePrimitiveBlock(raw, v);
    uint8_t types = 0;
    for (auto& g : v.groups) {
        const uint8_t *gp = g.first, *gend = gp + g.second;
        while (gp < gend) {
            uint64_t gtag = readVarint(gp, gend);
            uint32_t gfield = (uint32_t)(gtag >> 3), gwt = gtag & 7;
            if (gfield == 1 || gfield == 2) types |= BT_NODES;        // Node / DenseNodes
            else if (gfield == 3) types |= BT_WAYS;
            else if (gfield == 4) types |= BT_RELATIONS;
            if (!skipField(gwt, gp, gend)) break;
        }
    }
    return types;
}

// Iterates the ways of a block.
struct WayView {
    uint64_t id = 0;
    std::vector<uint32_t> keys, vals;
    const uint8_t* refsPtr = nullptr;
    const uint8_t* refsEnd = nullptr;
};
static void forEachWay(const PrimitiveBlockView& v, const std::function<void(const WayView&)>& cb) {
    WayView w;
    for (auto& g : v.groups) {
        const uint8_t *gp = g.first, *gend = gp + g.second;
        while (gp < gend) {
            uint64_t gtag = readVarint(gp, gend);
            uint32_t gfield = (uint32_t)(gtag >> 3), gwt = gtag & 7;
            if (gfield == 3 && gwt == 2) {
                uint64_t wlen = readVarint(gp, gend);
                if ((uint64_t)(gend - gp) < wlen) return;
                const uint8_t *wp = gp, *wend = gp + wlen;
                gp += wlen;
                w.id = 0; w.keys.clear(); w.vals.clear(); w.refsPtr = w.refsEnd = nullptr;
                while (wp < wend) {
                    uint64_t wtag = readVarint(wp, wend);
                    uint32_t wfield = (uint32_t)(wtag >> 3), wwt = wtag & 7;
                    if (wfield == 1 && wwt == 0) w.id = readVarint(wp, wend);
                    else if (wfield == 2 && wwt == 2) {
                        uint64_t l = readVarint(wp, wend); if ((uint64_t)(wend - wp) < l) break;
                        const uint8_t* kend = wp + l;
                        while (wp < kend) w.keys.push_back((uint32_t)readVarint(wp, kend));
                    } else if (wfield == 3 && wwt == 2) {
                        uint64_t l = readVarint(wp, wend); if ((uint64_t)(wend - wp) < l) break;
                        const uint8_t* vend = wp + l;
                        while (wp < vend) w.vals.push_back((uint32_t)readVarint(wp, vend));
                    } else if (wfield == 8 && wwt == 2) {
                        uint64_t l = readVarint(wp, wend); if ((uint64_t)(wend - wp) < l) break;
                        w.refsPtr = wp; w.refsEnd = wp + l; wp += l;
                    } else if (!skipField(wwt, wp, wend)) break;
                }
                if (w.refsPtr) cb(w);
            } else if (!skipField(gwt, gp, gend)) break;
        }
    }
}

// ----------------------------------------------------------------------------
// Text
// ----------------------------------------------------------------------------
static std::string cleanText(const std::string& s, size_t maxLen = 34) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 128) {
            if (c >= 32) out.push_back((char)c);
        } else if (c == 0xC3 && i + 1 < s.size()) {
            out.push_back((char)c);
            out.push_back(s[i + 1]);
            i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            i += 1;   // other 2-byte sequence: drop
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            i += 2;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            i += 3;
        }
    }
    auto rep = [&](const char* from, const char* to) {
        size_t pos = out.find(from);
        if (pos != std::string::npos) out.replace(pos, strlen(from), to);
    };
    rep("straße", "str.");
    rep("Straße", "Str.");
    rep("platz", "pl.");
    rep("Platz", "Pl.");
    if (out.size() > maxLen) {
        out.resize(maxLen);
        // never cut inside a 2-byte umlaut
        if (!out.empty() && (uint8_t)out.back() == 0xC3) out.pop_back();
    }
    return out;
}

// ----------------------------------------------------------------------------
// Data model
// ----------------------------------------------------------------------------
struct ExtractedWay {
    uint64_t id;
    FeatureClass cls;
    bool routableCar;
    bool routablePed;
    std::string streetName;
    std::vector<uint64_t> nodes;
};

struct ResolvedWay {
    uint64_t id;
    FeatureClass cls;
    bool routableCar;
    bool routablePed;
    std::string streetName;
    std::vector<std::pair<int32_t, int32_t>> coords;
};

struct PlaceEntry {
    int32_t lat_e5;
    int32_t lon_e5;
    uint32_t pop;
    uint8_t rank;
    std::string name;
};

struct PoiEntry {
    int32_t lat_e5;
    int32_t lon_e5;
    uint8_t category;
    std::string name;
};

struct StgNode {
    uint64_t id;
    int32_t lat_e5;
    int32_t lon_e5;
    bool operator<(const StgNode& o) const { return id < o.id; }
};

static inline bool inStuttgartMetro(double lat, double lon) {
    return lat >= KNAV_STG_MIN_LAT && lat <= KNAV_STG_MAX_LAT && lon >= KNAV_STG_MIN_LON && lon <= KNAV_STG_MAX_LON;
}
static inline bool inStuttgartBuildingsZone(double lat, double lon) {
    return lat >= 48.64 && lat <= 48.92 && lon >= 8.98 && lon <= 9.38;
}

// highway=* -> feature class and routability. Returns -1 for ignored values.
static int classifyHighway(const std::string& v, bool& rCar, bool& rPed, bool metroDetail) {
    rCar = rPed = false;
    if (v == "motorway" || v == "motorway_link") { rCar = true; return FEAT_MOTORWAY; }
    if (v == "trunk" || v == "trunk_link")       { rCar = true; return FEAT_PRIMARY; }
    if (v == "primary" || v == "primary_link")   { rCar = true; rPed = true; return FEAT_PRIMARY; }
    if (v == "secondary" || v == "secondary_link") { rCar = true; rPed = true; return FEAT_SECONDARY; }
    if (v == "tertiary" || v == "tertiary_link") { rCar = true; rPed = true; return FEAT_TERTIARY; }
    if (!metroDetail) return -1;
    if (v == "residential" || v == "unclassified" || v == "living_street") { rCar = true; rPed = true; return FEAT_RESIDENTIAL; }
    if (v == "service") { rCar = false; rPed = true; return FEAT_RESIDENTIAL; }
    if (v == "footway" || v == "cycleway" || v == "path" || v == "pedestrian" || v == "track" || v == "steps") { rPed = true; return FEAT_PATH; }
    return -1;
}

// ----------------------------------------------------------------------------
// Cell encoding: Douglas-Peucker + class dropping to stay below the cap
// ----------------------------------------------------------------------------
struct PolySeg {
    uint8_t cls;
    std::vector<std::pair<uint8_t, uint8_t>> pts;
};

static size_t encodedSize(const std::vector<PolySeg>& segs) {
    size_t n = 2;
    for (auto& s : segs) n += 2 + 2 * std::min<size_t>(s.pts.size(), 255);
    return n;
}

static double pointLineDist(const std::pair<uint8_t, uint8_t>& p, const std::pair<uint8_t, uint8_t>& a, const std::pair<uint8_t, uint8_t>& b) {
    double ax = a.first, ay = a.second, bx = b.first, by = b.second, px = p.first, py = p.second;
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    if (len2 < 1e-9) return std::hypot(px - ax, py - ay);
    double t = ((px - ax) * dx + (py - ay) * dy) / len2;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

static void douglasPeucker(const std::vector<std::pair<uint8_t, uint8_t>>& pts, size_t i0, size_t i1, double tol, std::vector<char>& keep) {
    if (i1 <= i0 + 1) return;
    double maxD = -1;
    size_t maxI = i0;
    for (size_t i = i0 + 1; i < i1; i++) {
        double d = pointLineDist(pts[i], pts[i0], pts[i1]);
        if (d > maxD) { maxD = d; maxI = i; }
    }
    if (maxD > tol) {
        keep[maxI] = 1;
        douglasPeucker(pts, i0, maxI, tol, keep);
        douglasPeucker(pts, maxI, i1, tol, keep);
    }
}

static void simplifySeg(PolySeg& s, double tol) {
    if (s.pts.size() < 3) return;
    std::vector<char> keep(s.pts.size(), 0);
    keep.front() = keep.back() = 1;
    douglasPeucker(s.pts, 0, s.pts.size() - 1, tol, keep);
    std::vector<std::pair<uint8_t, uint8_t>> out;
    out.reserve(s.pts.size());
    for (size_t i = 0; i < s.pts.size(); i++) if (keep[i]) out.push_back(s.pts[i]);
    s.pts.swap(out);
}

// Drop order when a cell is still too big after simplification (lowest priority first)
static const uint8_t DROP_ORDER[] = {
    FEAT_BUILDING, FEAT_PATH, FEAT_FARMLAND, FEAT_PARK, FEAT_URBAN, FEAT_RESIDENTIAL,
    FEAT_FOREST, FEAT_WATER_POLY, FEAT_RAIL, FEAT_TERTIARY, FEAT_BOUNDARY, FEAT_WATER,
    FEAT_SECONDARY, FEAT_PRIMARY, FEAT_MOTORWAY
};
static const char* FEAT_NAMES[FEAT_CLASS_COUNT] = {
    "farmland", "forest", "park", "urban", "water-poly", "building", "water", "boundary",
    "rail", "path", "residential", "tertiary", "secondary", "primary", "motorway"
};

struct CapStats {
    size_t cellsSimplified = 0, cellsDropped = 0, segsDropped = 0, cellsTruncated = 0;
    size_t droppedPerClass[FEAT_CLASS_COUNT] = {0};
};

static void fitCell(std::vector<PolySeg>& segs, int tier, size_t cellIdx, CapStats& st) {
    if (segs.empty()) return;
    // tier-dependent base tolerance in quantised units (1 unit = cell / 255)
    double tol = (tier == 2) ? 0.75 : 1.0;
    for (auto& s : segs) simplifySeg(s, tol);
    for (auto& s : segs) {
        if (s.pts.size() > 255) s.pts.resize(255);
    }
    size_t before = encodedSize(segs);
    if (before <= KNAV_CELL_TARGET_BYTES) return;
    st.cellsSimplified++;
    for (int pass = 0; pass < 2 && encodedSize(segs) > KNAV_CELL_TARGET_BYTES; pass++) {
        tol *= 2.0;
        for (auto& s : segs) simplifySeg(s, tol);
    }
    size_t afterSimplify = encodedSize(segs);
    if (afterSimplify <= KNAV_CELL_TARGET_BYTES) {
        printf("      tier%d cell %zu: simplified %zu -> %zu bytes\n", tier, cellIdx, before, afterSimplify);
        return;
    }
    st.cellsDropped++;
    for (uint8_t cls : DROP_ORDER) {
        if (encodedSize(segs) <= KNAV_CELL_TARGET_BYTES) break;
        size_t n = 0;
        for (auto& s : segs) if (s.cls == cls) n++;
        if (!n) continue;
        segs.erase(std::remove_if(segs.begin(), segs.end(), [&](const PolySeg& s) { return s.cls == cls; }), segs.end());
        st.segsDropped += n;
        st.droppedPerClass[cls] += n;
        printf("      tier%d cell %zu: dropped %zu x %s -> %zu bytes\n", tier, cellIdx, n, FEAT_NAMES[cls], encodedSize(segs));
    }
    while (encodedSize(segs) > KNAV_CELL_TARGET_BYTES && !segs.empty()) {
        segs.pop_back();
        st.cellsTruncated++;
    }
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(argv[0]); return 2; }
    const char* pbfPath = opt.pbfPath.c_str();
    const bool flashMode = opt.flashMode;

    auto inBbox = [&](double lat, double lon) -> bool {
        if (!opt.hasBbox) return true;
        return lat >= opt.bbMinLat && lat <= opt.bbMaxLat && lon >= opt.bbMinLon && lon <= opt.bbMaxLon;
    };

    FILE* f = fopen(pbfPath, "rb");
    if (!f) { perror("fopen pbf"); return 1; }

    unsigned hc = std::thread::hardware_concurrency();
    const int numThreads = (int)std::max(1u, std::min(8u, hc ? hc : 4u));

    printf("[1/7] Indexing PBF blocks in %s (flash=%d, bbox=%d, threads=%d)...\n",
           pbfPath, (int)flashMode, (int)opt.hasBbox, numThreads);
    std::vector<BlockIndex> blocks;
    blocks.reserve(65000);
    std::vector<uint8_t> hbuf;
    while (true) {
        int64_t pos = ftello(f);
        uint8_t hlenBuf[4];
        if (fread(hlenBuf, 1, 4, f) != 4) break;
        uint32_t hlen = (hlenBuf[0] << 24) | (hlenBuf[1] << 16) | (hlenBuf[2] << 8) | hlenBuf[3];
        if (hlen > 65536) { fprintf(stderr, "bad blob header length at %lld\n", (long long)pos); break; }
        hbuf.resize(hlen);
        if (fread(hbuf.data(), 1, hlen, f) != hlen) break;
        const uint8_t *p = hbuf.data(), *end = p + hlen;
        uint32_t datasize = 0;
        std::string type;
        while (p < end) {
            uint64_t tag = readVarint(p, end);
            uint32_t field = (uint32_t)(tag >> 3), wt = tag & 7;
            if (field == 1 && wt == 2) { uint64_t l = readVarint(p, end); if ((uint64_t)(end - p) < l) break; type.assign((const char*)p, l); p += l; }
            else if (field == 3 && wt == 0) datasize = (uint32_t)readVarint(p, end);
            else if (!skipField(wt, p, end)) break;
        }
        if (type == "OSMData") blocks.push_back({pos, datasize, 0});
        fseeko(f, datasize, SEEK_CUR);
    }
    fclose(f);
    printf("      %zu data blocks\n", blocks.size());

    // Block type detection (dense=2, ways=3, relations=4 inside PrimitiveGroup)
    {
        std::atomic<size_t> next{0};
        std::atomic<size_t> nNodeB{0}, nWayB{0}, nRelB{0};
        auto worker = [&]() {
            FILE* tf = fopen(pbfPath, "rb");
            if (!tf) return;
            std::vector<uint8_t> raw;
            while (true) {
                size_t i = next.fetch_add(1);
                if (i >= blocks.size()) break;
                if (!decompressBlock(tf, blocks[i], raw)) continue;
                uint8_t t = classifyBlock(raw);
                blocks[i].types = t;
                if (t & BT_NODES) nNodeB++;
                if (t & BT_WAYS) nWayB++;
                if (t & BT_RELATIONS) nRelB++;
            }
            fclose(tf);
        };
        std::vector<std::thread> th;
        for (int t = 0; t < numThreads; t++) th.emplace_back(worker);
        for (auto& t : th) t.join();
        printf("      block types: %zu node, %zu way, %zu relation blocks\n", nNodeB.load(), nWayB.load(), nRelB.load());
    }
    std::vector<size_t> nodeBlocks, wayBlocks;
    for (size_t i = 0; i < blocks.size(); i++) {
        if (blocks[i].types & BT_NODES) nodeBlocks.push_back(i);
        if (blocks[i].types & BT_WAYS) wayBlocks.push_back(i);
    }

    // -------------------------------------------------------------------------
    // PASS 1: nationwide roads / rail / water from way blocks
    // -------------------------------------------------------------------------
    printf("[2/7] Pass 1: scanning nationwide ways (%zu blocks)...\n", wayBlocks.size());
    std::vector<std::vector<ExtractedWay>> threadWays(numThreads);
    std::atomic<size_t> nextWayBlock{0};

    auto wayNationWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        if (!tf) return;
        std::vector<uint8_t> raw;
        PrimitiveBlockView v;
        auto& outWays = threadWays[tid];
        outWays.reserve(100000);

        while (true) {
            size_t bi = nextWayBlock.fetch_add(1);
            if (bi >= wayBlocks.size()) break;
            if (!decompressBlock(tf, blocks[wayBlocks[bi]], raw)) continue;
            parsePrimitiveBlock(raw, v);
            forEachWay(v, [&](const WayView& w) {
                int feat = -1;
                bool rCar = false, rPed = false;
                const std::string* namePtr = nullptr;
                size_t nk = std::min(w.keys.size(), w.vals.size());
                for (size_t i = 0; i < nk; i++) {
                    if (w.keys[i] >= v.st.size() || w.vals[i] >= v.st.size()) continue;
                    const std::string& k = v.st[w.keys[i]];
                    const std::string& val = v.st[w.vals[i]];
                    if (k == "highway") {
                        bool c, p;
                        int hf = classifyHighway(val, c, p, false);
                        if (hf >= 0) { feat = hf; rCar = c; rPed = p; }
                    } else if (k == "waterway" && (val == "river" || val == "canal")) {
                        if (feat < 0) feat = FEAT_WATER;
                    } else if (k == "natural" && val == "coastline") {
                        if (feat < 0) feat = FEAT_BOUNDARY;
                    } else if (k == "railway" && (val == "rail" || val == "light_rail" || val == "subway")) {
                        if (feat < 0) feat = FEAT_RAIL;
                    } else if (k == "name") {
                        namePtr = &val;
                    }
                }
                if (feat < 0) return;

                std::vector<uint64_t> refs;
                int64_t curRef = 0;
                const uint8_t* rp = w.refsPtr;
                while (rp < w.refsEnd) {
                    curRef += zigZag(readVarint(rp, w.refsEnd));
                    refs.push_back((uint64_t)curRef);
                }
                if (refs.size() < 2) return;

                ExtractedWay ew;
                ew.id = w.id;
                ew.cls = (FeatureClass)feat;
                ew.routableCar = rCar;
                ew.routablePed = rPed;
                if (namePtr && !namePtr->empty()) ew.streetName = cleanText(*namePtr, 26);
                size_t step = (feat >= FEAT_PRIMARY) ? 2 : 3;
                ew.nodes.push_back(refs.front());
                for (size_t r = step; r + 1 < refs.size(); r += step) ew.nodes.push_back(refs[r]);
                ew.nodes.push_back(refs.back());
                outWays.push_back(std::move(ew));
            });
        }
        fclose(tf);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; t++) threads.emplace_back(wayNationWorker, t);
    for (auto& th : threads) th.join();

    std::vector<ExtractedWay> nationWays;
    {
        size_t total = 0;
        for (auto& tw : threadWays) total += tw.size();
        nationWays.reserve(total);
        for (auto& tw : threadWays) { for (auto& w : tw) nationWays.push_back(std::move(w)); tw.clear(); tw.shrink_to_fit(); }
        std::sort(nationWays.begin(), nationWays.end(), [](const ExtractedWay& a, const ExtractedWay& b) { return a.id < b.id; });
    }

    std::vector<uint64_t> neededNodes;
    for (auto& w : nationWays) for (uint64_t nid : w.nodes) neededNodes.push_back(nid);
    std::sort(neededNodes.begin(), neededNodes.end());
    neededNodes.erase(std::unique(neededNodes.begin(), neededNodes.end()), neededNodes.end());
    printf("      %zu nationwide ways, %zu unique nodes\n", nationWays.size(), neededNodes.size());

    std::vector<int32_t> nodeLat(neededNodes.size(), 0);
    std::vector<int32_t> nodeLon(neededNodes.size(), 0);

    // -------------------------------------------------------------------------
    // PASS 2: dense nodes -> nationwide coordinates, metro nodes, places, POIs
    // -------------------------------------------------------------------------
    printf("[3/7] Pass 2: scanning dense nodes (%zu blocks)...\n", nodeBlocks.size());
    std::vector<std::vector<PlaceEntry>> threadPlaces(numThreads);
    std::vector<std::vector<PoiEntry>> threadPois(numThreads);
    std::vector<std::vector<StgNode>> threadStgNodes(numThreads);
    std::atomic<size_t> nextNodeBlock{0};
    std::mutex coordMutex;   // nodeLat/nodeLon writes are disjoint per node id; mutex keeps it simple

    auto nodeWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        if (!tf) return;
        std::vector<uint8_t> raw;
        PrimitiveBlockView v;
        auto& outPlaces = threadPlaces[tid];
        auto& outPois = threadPois[tid];
        auto& outStg = threadStgNodes[tid];
        outStg.reserve(800000);

        while (true) {
            size_t bi = nextNodeBlock.fetch_add(1);
            if (bi >= nodeBlocks.size()) break;
            if (!decompressBlock(tf, blocks[nodeBlocks[bi]], raw)) continue;
            parsePrimitiveBlock(raw, v);

            for (auto& g : v.groups) {
                const uint8_t *gp = g.first, *gend = gp + g.second;
                while (gp < gend) {
                    uint64_t gtag = readVarint(gp, gend);
                    uint32_t gfield = (uint32_t)(gtag >> 3), gwt = gtag & 7;
                    if (gfield == 2 && gwt == 2) {
                        uint64_t dlen = readVarint(gp, gend);
                        if ((uint64_t)(gend - gp) < dlen) break;
                        const uint8_t *dp = gp, *dend = gp + dlen;
                        gp += dlen;

                        const uint8_t *idP = nullptr, *idE = nullptr, *latP = nullptr, *latE = nullptr;
                        const uint8_t *lonP = nullptr, *lonE = nullptr, *kvP = nullptr, *kvE = nullptr;
                        while (dp < dend) {
                            uint64_t dtag = readVarint(dp, dend);
                            uint32_t dfield = (uint32_t)(dtag >> 3), dwt = dtag & 7;
                            if (dwt == 2) {
                                uint64_t l = readVarint(dp, dend);
                                if ((uint64_t)(dend - dp) < l) break;
                                if (dfield == 1) { idP = dp; idE = dp + l; }
                                else if (dfield == 8) { latP = dp; latE = dp + l; }
                                else if (dfield == 9) { lonP = dp; lonE = dp + l; }
                                else if (dfield == 10) { kvP = dp; kvE = dp + l; }
                                dp += l;
                            } else if (!skipField(dwt, dp, dend)) break;
                        }
                        if (!idP || !latP || !lonP) continue;

                        int64_t curId = 0, curLat = 0, curLon = 0;
                        auto itHint = neededNodes.begin();
                        while (idP < idE && latP < latE && lonP < lonE) {
                            curId  += zigZag(readVarint(idP, idE));
                            curLat += zigZag(readVarint(latP, latE));
                            curLon += zigZag(readVarint(lonP, lonE));

                            int64_t nanoLat = v.latOffset + (int64_t)v.granularity * curLat;
                            int64_t nanoLon = v.lonOffset + (int64_t)v.granularity * curLon;
                            int32_t lat5 = (int32_t)(nanoLat / 10000);
                            int32_t lon5 = (int32_t)(nanoLon / 10000);
                            double dlat = lat5 * 1e-5, dlon = lon5 * 1e-5;
                            bool isStg = inStuttgartMetro(dlat, dlon) && inBbox(dlat, dlon);
                            uint64_t uid = (uint64_t)curId;

                            if (isStg) outStg.push_back({uid, lat5, lon5});

                            if (itHint != neededNodes.end() && *itHint < uid) itHint = std::lower_bound(itHint, neededNodes.end(), uid);
                            if (itHint != neededNodes.end() && *itHint == uid) {
                                size_t idx = (size_t)(itHint - neededNodes.begin());
                                std::lock_guard<std::mutex> lk(coordMutex);
                                nodeLat[idx] = lat5;
                                nodeLon[idx] = lon5;
                                ++itHint;
                            }

                            // tags (key/value index pairs, 0-terminated per node)
                            uint8_t placeRank = 0, poiCat = POI_NONE;
                            uint32_t pop = 0;
                            const std::string* namePtr = nullptr;
                            const std::string* deNamePtr = nullptr;
                            if (kvP && kvP < kvE) {
                                while (kvP < kvE) {
                                    uint32_t kIdx = (uint32_t)readVarint(kvP, kvE);
                                    if (kIdx == 0) break;
                                    uint32_t vIdx = (uint32_t)readVarint(kvP, kvE);
                                    if (kIdx >= v.st.size() || vIdx >= v.st.size()) continue;
                                    const std::string& k = v.st[kIdx];
                                    const std::string& val = v.st[vIdx];
                                    if (k == "place") {
                                        if (val == "city") placeRank = 1;
                                        else if (val == "town") placeRank = 2;
                                        else if (val == "suburb" || val == "borough") placeRank = 3;
                                        else if (val == "village" || val == "municipality" ||
                                                 (isStg && (val == "quarter" || val == "neighbourhood" || val == "hamlet"))) placeRank = 4;
                                    } else if (isStg && k == "railway") {
                                        if (val == "station" || val == "halt" || val == "tram_stop" || val == "subway_entrance") poiCat = POI_TRANSIT;
                                    } else if (isStg && k == "amenity") {
                                        if (val == "parking") poiCat = POI_PARKING;
                                        else if (val == "fuel" || val == "charging_station") poiCat = POI_FUEL;
                                        else if (val == "restaurant" || val == "cafe" || val == "biergarten" || val == "fast_food") poiCat = POI_FOOD;
                                        else if (val == "hospital" || val == "pharmacy" || val == "doctors") poiCat = POI_HEALTH;
                                    } else if (isStg && k == "shop") {
                                        if (val == "supermarket" || val == "bakery" || val == "mall") poiCat = POI_SHOP;
                                    } else if (isStg && (k == "tourism" || k == "historic" || k == "natural")) {
                                        if (val == "attraction" || val == "viewpoint" || val == "museum" || val == "castle" || val == "peak") poiCat = POI_SIGHT;
                                    } else if (k == "name") {
                                        namePtr = &val;
                                    } else if (k == "name:de") {
                                        deNamePtr = &val;
                                    } else if (k == "population") {
                                        pop = (uint32_t)strtoul(val.c_str(), nullptr, 10);
                                    }
                                }
                            }
                            if (!inBbox(dlat, dlon)) continue;
                            const std::string* bestName = deNamePtr ? deNamePtr : namePtr;
                            if (placeRank > 0 && bestName && !bestName->empty()) {
                                PlaceEntry pe;
                                pe.lat_e5 = lat5; pe.lon_e5 = lon5;
                                pe.pop = isStg ? (pop + 25000) : pop;
                                pe.rank = placeRank;
                                pe.name = cleanText(*bestName, 34);
                                if (!pe.name.empty()) outPlaces.push_back(std::move(pe));
                            } else if (isStg && poiCat != POI_NONE && bestName && !bestName->empty()) {
                                PoiEntry po;
                                po.lat_e5 = lat5; po.lon_e5 = lon5;
                                po.category = poiCat;
                                po.name = cleanText(*bestName, 26);
                                if (!po.name.empty()) outPois.push_back(std::move(po));
                            }
                        }
                    } else if (!skipField(gwt, gp, gend)) break;
                }
            }
        }
        fclose(tf);
    };

    threads.clear();
    for (int t = 0; t < numThreads; t++) threads.emplace_back(nodeWorker, t);
    for (auto& th : threads) th.join();

    std::vector<StgNode> stgNodes;
    {
        size_t total = 0;
        for (auto& ts : threadStgNodes) total += ts.size();
        stgNodes.reserve(total);
        for (auto& ts : threadStgNodes) { stgNodes.insert(stgNodes.end(), ts.begin(), ts.end()); ts.clear(); ts.shrink_to_fit(); }
        std::sort(stgNodes.begin(), stgNodes.end());
    }
    printf("      %zu Stuttgart-metro nodes (%.1f MB)\n", stgNodes.size(), stgNodes.size() * sizeof(StgNode) / 1048576.0);

    auto lookupStgNode = [&](uint64_t nid, int32_t& lat5, int32_t& lon5) -> bool {
        StgNode key{nid, 0, 0};
        auto it = std::lower_bound(stgNodes.begin(), stgNodes.end(), key);
        if (it == stgNodes.end() || it->id != nid) return false;
        lat5 = it->lat_e5; lon5 = it->lon_e5;
        return true;
    };

    // -------------------------------------------------------------------------
    // PASS 3: Stuttgart-metro detail ways
    // -------------------------------------------------------------------------
    printf("[4/7] Pass 3: extracting Stuttgart-metro buildings, landuse, streets & paths...\n");
    std::vector<std::vector<ResolvedWay>> threadStgWays(numThreads);
    nextWayBlock.store(0);

    auto wayStgWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        if (!tf) return;
        std::vector<uint8_t> raw;
        PrimitiveBlockView v;
        auto& outWays = threadStgWays[tid];
        outWays.reserve(80000);

        while (true) {
            size_t bi = nextWayBlock.fetch_add(1);
            if (bi >= wayBlocks.size()) break;
            if (!decompressBlock(tf, blocks[wayBlocks[bi]], raw)) continue;
            parsePrimitiveBlock(raw, v);
            forEachWay(v, [&](const WayView& w) {
                const uint8_t* rpTmp = w.refsPtr;
                uint64_t firstNid = (uint64_t)zigZag(readVarint(rpTmp, w.refsEnd));
                int32_t lat0, lon0;
                if (!lookupStgNode(firstNid, lat0, lon0)) return;

                int feat = -1;
                bool rCar = false, rPed = false;
                const std::string* namePtr = nullptr;
                size_t nk = std::min(w.keys.size(), w.vals.size());
                for (size_t i = 0; i < nk; i++) {
                    if (w.keys[i] >= v.st.size() || w.vals[i] >= v.st.size()) continue;
                    const std::string& k = v.st[w.keys[i]];
                    const std::string& val = v.st[w.vals[i]];
                    if (k == "highway") {
                        bool c, p;
                        int hf = classifyHighway(val, c, p, true);
                        // nationwide classes are already covered by pass 1
                        if (hf >= 0 && hf <= FEAT_RESIDENTIAL) { feat = hf; rCar = c; rPed = p; }
                    } else if (k == "building" && val != "no") {
                        if (feat < 0 && (!flashMode || (w.id % 2 == 0))) feat = FEAT_BUILDING;
                    } else if (k == "landuse") {
                        if (feat < 0) {
                            if (val == "forest") feat = FEAT_FOREST;
                            else if (val == "farmland" || val == "meadow" || val == "orchard" || val == "vineyard") feat = FEAT_FARMLAND;
                            else if (val == "grass" || val == "recreation_ground" || val == "village_green" || val == "allotments") feat = FEAT_PARK;
                            else if (val == "residential" || val == "commercial" || val == "industrial" || val == "retail") feat = FEAT_URBAN;
                            else if (val == "reservoir" || val == "basin") feat = FEAT_WATER_POLY;
                        }
                    } else if (k == "natural") {
                        if (feat < 0) {
                            if (val == "wood") feat = FEAT_FOREST;
                            else if (val == "water") feat = FEAT_WATER_POLY;
                            else if (val == "grassland" || val == "scrub") feat = FEAT_PARK;
                        }
                    } else if (k == "leisure") {
                        if (feat < 0 && (val == "park" || val == "garden" || val == "nature_reserve" || val == "pitch")) feat = FEAT_PARK;
                    } else if (k == "waterway") {
                        if (feat < 0) {
                            if (val == "riverbank") feat = FEAT_WATER_POLY;
                            else if (val == "stream") feat = FEAT_WATER;
                        }
                    } else if (k == "railway" && val == "tram") {
                        if (feat < 0) feat = FEAT_RAIL;
                    } else if (k == "name") {
                        namePtr = &val;
                    }
                }
                if (feat < 0) return;
                if (flashMode && feat == FEAT_BUILDING && !inStuttgartBuildingsZone(lat0 * 1e-5, lon0 * 1e-5)) return;

                std::vector<std::pair<int32_t, int32_t>> coords;
                int64_t curRef = 0;
                const uint8_t* rp = w.refsPtr;
                while (rp < w.refsEnd) {
                    curRef += zigZag(readVarint(rp, w.refsEnd));
                    int32_t la, lo;
                    if (lookupStgNode((uint64_t)curRef, la, lo)) coords.push_back({la, lo});
                }
                if (coords.size() < 2) return;

                ResolvedWay rw;
                rw.id = w.id;
                rw.cls = (FeatureClass)feat;
                rw.routableCar = rCar;
                rw.routablePed = rPed;
                if (namePtr && !namePtr->empty() && feat >= FEAT_RESIDENTIAL) rw.streetName = cleanText(*namePtr, 26);
                if (feat == FEAT_BUILDING) {
                    size_t bstep = std::max<size_t>(1, (coords.size() - 1) / 5);
                    for (size_t r = 0; r + 1 < coords.size() && rw.coords.size() < 5; r += bstep) rw.coords.push_back(coords[r]);
                    rw.coords.push_back(coords.back());
                } else if (feat <= FEAT_WATER_POLY) {
                    size_t pstep = std::max<size_t>(1, coords.size() / 12);
                    for (size_t r = 0; r + 1 < coords.size() && rw.coords.size() < 14; r += pstep) rw.coords.push_back(coords[r]);
                    rw.coords.push_back(coords.back());
                } else {
                    rw.coords = std::move(coords);
                }
                outWays.push_back(std::move(rw));
            });
        }
        fclose(tf);
    };

    threads.clear();
    for (int t = 0; t < numThreads; t++) threads.emplace_back(wayStgWorker, t);
    for (auto& th : threads) th.join();

    std::vector<ResolvedWay> stgWays;
    {
        size_t total = 0;
        for (auto& tw : threadStgWays) total += tw.size();
        stgWays.reserve(total);
        for (auto& tw : threadStgWays) { for (auto& w : tw) stgWays.push_back(std::move(w)); tw.clear(); tw.shrink_to_fit(); }
        std::sort(stgWays.begin(), stgWays.end(), [](const ResolvedWay& a, const ResolvedWay& b) { return a.id < b.id; });
    }
    printf("      %zu Stuttgart-metro detail ways\n", stgWays.size());
    stgNodes.clear();
    stgNodes.shrink_to_fit();

    // Merge places & POIs (deterministic order)
    std::vector<PlaceEntry> allPlaces;
    for (auto& tp : threadPlaces) for (auto& p : tp) allPlaces.push_back(std::move(p));
    std::sort(allPlaces.begin(), allPlaces.end(), [](const PlaceEntry& a, const PlaceEntry& b) {
        bool aStg = inStuttgartMetro(a.lat_e5 * 1e-5, a.lon_e5 * 1e-5);
        bool bStg = inStuttgartMetro(b.lat_e5 * 1e-5, b.lon_e5 * 1e-5);
        if (aStg != bStg && a.rank >= 3 && b.rank >= 3) return aStg > bStg;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.pop != b.pop) return a.pop > b.pop;
        if (a.name != b.name) return a.name < b.name;
        if (a.lat_e5 != b.lat_e5) return a.lat_e5 < b.lat_e5;
        return a.lon_e5 < b.lon_e5;
    });
    size_t maxPlaces = flashMode ? 4500 : 25000;
    if (allPlaces.size() > maxPlaces) allPlaces.resize(maxPlaces);
    if (allPlaces.size() > KNAV_MAX_PLACES) allPlaces.resize(KNAV_MAX_PLACES);

    std::vector<PoiEntry> allPois;
    for (auto& tpo : threadPois) for (auto& po : tpo) allPois.push_back(std::move(po));
    std::sort(allPois.begin(), allPois.end(), [](const PoiEntry& a, const PoiEntry& b) {
        if (a.lat_e5 != b.lat_e5) return a.lat_e5 < b.lat_e5;
        if (a.lon_e5 != b.lon_e5) return a.lon_e5 < b.lon_e5;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });

    // -------------------------------------------------------------------------
    // STEP 5: spatial tiles, street labels, routing graph
    // -------------------------------------------------------------------------
    printf("[5/7] Building 3-tier vector tiles, street labels & routing graph...\n");

    std::vector<std::vector<PolySeg>> coarseCells(KNAV_COARSE_CELLS);
    std::vector<std::vector<PolySeg>> fineCells(KNAV_FINE_CELLS);
    std::vector<std::vector<PolySeg>> ultraCells(KNAV_ULTRA_CELLS);

    struct RouteEdgeTmp { uint32_t to; uint16_t dist_hm; uint8_t flags; };
    struct RouteNodeTmp { int32_t lat_e5; int32_t lon_e5; std::vector<RouteEdgeTmp> edges; };
    std::unordered_map<uint64_t, uint32_t> junctionMap;
    std::vector<RouteNodeTmp> routeNodes;
    routeNodes.reserve(60000);

    // NOTE: the graph is grid based: ways touching the same junction cell are
    // considered connected (0.5-0.8 km metro, 2.2-3.6 km nationwide). This is
    // a coarse approximation, not a topological road graph.
    auto getJunctionNode = [&](int32_t lat5, int32_t lon5) -> uint32_t {
        bool isStg = inStuttgartMetro(lat5 * 1e-5, lon5 * 1e-5);
        int32_t gy = isStg ? (lat5 / (flashMode ? 800 : 500)) : (lat5 / (flashMode ? 3600 : 2200) + 1000000);
        int32_t gx = isStg ? (lon5 / (flashMode ? 1150 : 720)) : (lon5 / (flashMode ? 5200 : 3200) + 1000000);
        uint64_t key = ((uint64_t)(uint32_t)gy << 32) | (uint32_t)gx;
        auto it = junctionMap.find(key);
        if (it != junctionMap.end()) return it->second;
        uint32_t id = (uint32_t)routeNodes.size();
        routeNodes.push_back({lat5, lon5, {}});
        junctionMap[key] = id;
        return id;
    };

    auto lookupCoord = [&](uint64_t nid, int32_t& lat5, int32_t& lon5) -> bool {
        auto it = std::lower_bound(neededNodes.begin(), neededNodes.end(), nid);
        if (it == neededNodes.end() || *it != nid) return false;
        size_t idx = (size_t)(it - neededNodes.begin());
        lat5 = nodeLat[idx]; lon5 = nodeLon[idx];
        return (lat5 != 0 || lon5 != 0);
    };

    const double latSpan = KNAV_DE_MAX_LAT - KNAV_DE_MIN_LAT;
    const double lonSpan = KNAV_DE_MAX_LON - KNAV_DE_MIN_LON;
    const double stgLatSpan = KNAV_STG_MAX_LAT - KNAV_STG_MIN_LAT;
    const double stgLonSpan = KNAV_STG_MAX_LON - KNAV_STG_MIN_LON;
    std::unordered_set<std::string> seenStreetCell;

    // data bounding box (for the V3 header)
    int32_t bbMinLat5 = INT32_MAX, bbMinLon5 = INT32_MAX, bbMaxLat5 = INT32_MIN, bbMaxLon5 = INT32_MIN;
    auto bbAdd = [&](int32_t la, int32_t lo) {
        bbMinLat5 = std::min(bbMinLat5, la); bbMaxLat5 = std::max(bbMaxLat5, la);
        bbMinLon5 = std::min(bbMinLon5, lo); bbMaxLon5 = std::max(bbMaxLon5, lo);
    };

    auto addToGrid = [&](std::vector<std::vector<PolySeg>>& cells, int nx, int ny, double minLat, double minLon,
                         double spanLat, double spanLon, uint8_t cls, const std::vector<std::pair<int32_t, int32_t>>& coords,
                         size_t maxSegs, bool clipToBox) {
        int curCell = -1;
        PolySeg seg{cls, {}};
        for (auto& pt : coords) {
            double dlat = pt.first * 1e-5, dlon = pt.second * 1e-5;
            if (clipToBox && (dlat < minLat || dlat > minLat + spanLat || dlon < minLon || dlon > minLon + spanLon)) continue;
            double fy = (dlat - minLat) / spanLat * ny;
            double fx = (dlon - minLon) / spanLon * nx;
            int cy = std::clamp((int)fy, 0, ny - 1);
            int cx = std::clamp((int)fx, 0, nx - 1);
            int cell = cy * nx + cx;
            uint8_t qx = (uint8_t)std::clamp((int)((fx - cx) * 255.0), 0, 255);
            uint8_t qy = (uint8_t)std::clamp((int)((fy - cy) * 255.0), 0, 255);
            if (cell != curCell) {
                if (curCell >= 0 && seg.pts.size() >= 2 && cells[curCell].size() < maxSegs) cells[curCell].push_back(seg);
                seg.pts.clear();
                curCell = cell;
            }
            if (seg.pts.empty() || seg.pts.back() != std::make_pair(qx, qy)) seg.pts.push_back({qx, qy});
        }
        if (curCell >= 0 && seg.pts.size() >= 2 && cells[curCell].size() < maxSegs) cells[curCell].push_back(seg);
    };

    auto processWayCoords = [&](FeatureClass cls, bool rCar, bool rPed, const std::string& streetName,
                                const std::vector<std::pair<int32_t, int32_t>>& coords) {
        if (coords.size() < 2) return;
        for (auto& c : coords) bbAdd(c.first, c.second);
        bool anyInStg = inStuttgartMetro(coords[0].first * 1e-5, coords[0].second * 1e-5) ||
                        inStuttgartMetro(coords.back().first * 1e-5, coords.back().second * 1e-5);

        if (anyInStg && !streetName.empty() && cls >= FEAT_RESIDENTIAL) {
            auto& mid = coords[coords.size() / 2];
            int ucy = (int)((mid.first * 1e-5 - KNAV_STG_MIN_LAT) / stgLatSpan * 64.0);
            int ucx = (int)((mid.second * 1e-5 - KNAV_STG_MIN_LON) / stgLonSpan * 64.0);
            std::string skey = streetName + "#" + std::to_string(ucy * 64 + ucx);
            if (seenStreetCell.insert(skey).second) allPois.push_back({mid.first, mid.second, POI_STREET, streetName});
        }

        if ((rCar || rPed) && (cls >= FEAT_TERTIARY || (anyInStg && cls == FEAT_RESIDENTIAL))) {
            uint8_t flags = knavEdgeFlags(rCar, rPed, knavRoadRankForClass(cls));
            auto addEdge = [&](uint32_t a, uint32_t b, uint16_t hm) {
                for (auto& e : routeNodes[a].edges) {
                    if (e.to == b) {
                        if (hm < e.dist_hm) e.dist_hm = hm;
                        e.flags |= flags;
                        return;
                    }
                }
                if (routeNodes[a].edges.size() < 10) routeNodes[a].edges.push_back({b, hm, flags});
            };
            uint32_t prevU = getJunctionNode(coords[0].first, coords[0].second);
            for (size_t i = 1; i < coords.size(); i++) {
                uint32_t u = getJunctionNode(coords[i].first, coords[i].second);
                if (u != prevU) {
                    double midLat = (routeNodes[prevU].lat_e5 + routeNodes[u].lat_e5) * 0.5e-5;
                    double dlat = (routeNodes[prevU].lat_e5 - routeNodes[u].lat_e5) * 1e-5 * 111.32;
                    double dlon = (routeNodes[prevU].lon_e5 - routeNodes[u].lon_e5) * 1e-5 * 111.32 * std::cos(midLat * M_PI / 180.0);
                    double km = std::sqrt(dlat * dlat + dlon * dlon);
                    if (km < 25.0) {
                        uint16_t hm = (uint16_t)std::max(1.0, std::min(65535.0, km * 10.0));
                        addEdge(prevU, u, hm);
                        addEdge(u, prevU, hm);
                    }
                    prevU = u;
                }
            }
        }

        if (cls == FEAT_MOTORWAY || cls == FEAT_PRIMARY || cls == FEAT_WATER || cls == FEAT_BOUNDARY) {
            addToGrid(coarseCells, KNAV_COARSE_NX, KNAV_COARSE_NY, KNAV_DE_MIN_LAT, KNAV_DE_MIN_LON, latSpan, lonSpan, (uint8_t)cls, coords, 350, false);
        }
        if (cls >= FEAT_WATER && cls != FEAT_PATH && cls != FEAT_RESIDENTIAL && !(flashMode && cls == FEAT_TERTIARY)) {
            addToGrid(fineCells, KNAV_FINE_NX, KNAV_FINE_NY, KNAV_DE_MIN_LAT, KNAV_DE_MIN_LON, latSpan, lonSpan, (uint8_t)cls, coords, flashMode ? 5 : 70, false);
        }
        if (anyInStg) {
            addToGrid(ultraCells, KNAV_ULTRA_NX, KNAV_ULTRA_NY, KNAV_STG_MIN_LAT, KNAV_STG_MIN_LON, stgLatSpan, stgLonSpan, (uint8_t)cls, coords, 350, true);
        }
    };

    // nationwide ways first (sorted by id) so major roads always fit
    for (auto& w : nationWays) {
        std::vector<std::pair<int32_t, int32_t>> coords;
        coords.reserve(w.nodes.size());
        for (uint64_t nid : w.nodes) {
            int32_t la, lo;
            if (lookupCoord(nid, la, lo)) {
                double dlat = la * 1e-5, dlon = lo * 1e-5;
                if (dlat >= KNAV_DE_MIN_LAT && dlat <= KNAV_DE_MAX_LAT && dlon >= KNAV_DE_MIN_LON && dlon <= KNAV_DE_MAX_LON && inBbox(dlat, dlon)) {
                    coords.push_back({la, lo});
                }
            }
        }
        processWayCoords(w.cls, w.routableCar, w.routablePed, w.streetName, coords);
    }
    for (auto& rw : stgWays) processWayCoords(rw.cls, rw.routableCar, rw.routablePed, rw.streetName, rw.coords);

    // per-cell segment caps (ultra) + render order
    size_t maxUltraFinal = flashMode ? 17 : 220;
    for (auto& cellSegs : ultraCells) {
        if (cellSegs.size() > maxUltraFinal) {
            std::stable_sort(cellSegs.begin(), cellSegs.end(), [](const PolySeg& a, const PolySeg& b) {
                bool aBld = (a.cls == FEAT_BUILDING), bBld = (b.cls == FEAT_BUILDING);
                if (aBld != bBld) return !aBld;
                return a.cls > b.cls;
            });
            cellSegs.resize(maxUltraFinal);
        }
        std::stable_sort(cellSegs.begin(), cellSegs.end(), [](const PolySeg& a, const PolySeg& b) { return a.cls < b.cls; });
    }
    for (auto& cellSegs : fineCells)   std::stable_sort(cellSegs.begin(), cellSegs.end(), [](const PolySeg& a, const PolySeg& b) { return a.cls < b.cls; });
    for (auto& cellSegs : coarseCells) std::stable_sort(cellSegs.begin(), cellSegs.end(), [](const PolySeg& a, const PolySeg& b) { return a.cls < b.cls; });

    // byte cap per cell: simplify, then drop low classes
    printf("[6/7] Capping cells at %u bytes (simplify, then drop low classes)...\n", (unsigned)KNAV_CELL_TARGET_BYTES);
    CapStats capStats;
    for (size_t i = 0; i < coarseCells.size(); i++) fitCell(coarseCells[i], 0, i, capStats);
    for (size_t i = 0; i < fineCells.size(); i++)   fitCell(fineCells[i], 1, i, capStats);
    for (size_t i = 0; i < ultraCells.size(); i++)  fitCell(ultraCells[i], 2, i, capStats);
    printf("      %zu cells needed extra simplification, %zu cells lost classes (%zu segments), %zu truncations\n",
           capStats.cellsSimplified, capStats.cellsDropped, capStats.segsDropped, capStats.cellsTruncated);
    for (int c = 0; c < FEAT_CLASS_COUNT; c++) {
        if (capStats.droppedPerClass[c]) printf("        dropped %zu x %s\n", capStats.droppedPerClass[c], FEAT_NAMES[c]);
    }

    if (flashMode && allPois.size() > 10000) {
        std::stable_sort(allPois.begin(), allPois.end(), [](const PoiEntry& a, const PoiEntry& b) {
            if ((a.category == POI_STREET) != (b.category == POI_STREET)) return a.category != POI_STREET;
            return a.category < b.category;
        });
        allPois.resize(10000);
    }
    std::sort(allPois.begin(), allPois.end(), [](const PoiEntry& a, const PoiEntry& b) {
        if (a.lat_e5 != b.lat_e5) return a.lat_e5 < b.lat_e5;
        if (a.lon_e5 != b.lon_e5) return a.lon_e5 < b.lon_e5;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });
    if (allPois.size() > KNAV_MAX_POIS) allPois.resize(KNAV_MAX_POIS);
    for (auto& p : allPlaces) bbAdd(p.lat_e5, p.lon_e5);
    for (auto& p : allPois) bbAdd(p.lat_e5, p.lon_e5);

    size_t totalEdges = 0;
    for (auto& rn : routeNodes) totalEdges += rn.edges.size();
    printf("      Routing graph: %zu nodes, %zu directed edges.\n", routeNodes.size(), totalEdges);
    if (routeNodes.size() > KNAV_MAX_ROUTE_NODES || totalEdges > KNAV_MAX_ROUTE_EDGES) {
        fprintf(stderr, "routing graph exceeds format limits\n");
        return 1;
    }

    // -------------------------------------------------------------------------
    // STEP 7: write
    // -------------------------------------------------------------------------
    printf("[7/7] Writing %s...\n", opt.outPath.c_str());
    FILE* out = fopen(opt.outPath.c_str(), "wb");
    if (!out) { perror("fopen out"); return 1; }

    KnavHeader hdr{};
    memcpy(hdr.magic, KNAV_MAGIC_V3, KNAV_MAGIC_LEN);
    fwrite(&hdr, sizeof(hdr), 1, out);    // placeholder, rewritten at the end

    hdr.numPlaces = (uint32_t)allPlaces.size();
    hdr.placesOffset = (uint32_t)ftello(out);
    for (auto& p : allPlaces) {
        DiskPlace dp{};
        dp.lat_e5 = p.lat_e5; dp.lon_e5 = p.lon_e5; dp.pop = p.pop; dp.rank = p.rank;
        strncpy(dp.name, p.name.c_str(), sizeof(dp.name) - 1);
        fwrite(&dp, sizeof(dp), 1, out);
    }

    hdr.numRouteNodes = (uint32_t)routeNodes.size();
    hdr.numRouteEdges = (uint32_t)totalEdges;
    hdr.routeNodesOffset = (uint32_t)ftello(out);
    uint32_t curEdgeIdx = 0;
    for (auto& rn : routeNodes) {
        DiskRouteNode drn{rn.lat_e5, rn.lon_e5, curEdgeIdx, (uint16_t)rn.edges.size(), 0};
        fwrite(&drn, sizeof(drn), 1, out);
        curEdgeIdx += (uint32_t)rn.edges.size();
    }
    hdr.routeEdgesOffset = (uint32_t)ftello(out);
    for (auto& rn : routeNodes) {
        for (auto& e : rn.edges) {
            DiskRouteEdge dre{e.to, e.dist_hm, e.flags, 0};
            fwrite(&dre, sizeof(dre), 1, out);
        }
    }

    size_t oversize = 0;
    auto writeGrid = [&](const std::vector<std::vector<PolySeg>>& cells, std::vector<CellDirEntry>& dir, uint32_t& dirOff) {
        dir.assign(cells.size(), {0, 0});
        dirOff = (uint32_t)ftello(out);
        fwrite(dir.data(), sizeof(CellDirEntry), dir.size(), out);
        for (size_t i = 0; i < cells.size(); i++) {
            if (cells[i].empty()) continue;
            dir[i].offset = (uint32_t)ftello(out);
            uint16_t nseg = (uint16_t)std::min<size_t>(cells[i].size(), 65535);
            fwrite(&nseg, 2, 1, out);
            for (uint16_t s = 0; s < nseg; s++) {
                auto& seg = cells[i][s];
                uint8_t cls = seg.cls;
                uint8_t npts = (uint8_t)std::min<size_t>(seg.pts.size(), 255);
                fwrite(&cls, 1, 1, out);
                fwrite(&npts, 1, 1, out);
                for (uint8_t k = 0; k < npts; k++) {
                    uint8_t xy[2] = {seg.pts[k].first, seg.pts[k].second};
                    fwrite(xy, 1, 2, out);
                }
            }
            dir[i].length = (uint32_t)ftello(out) - dir[i].offset;
            if (dir[i].length > KNAV_MAX_CELL_BYTES) oversize++;
        }
    };

    std::vector<CellDirEntry> coarseDir, fineDir, ultraDir;
    writeGrid(coarseCells, coarseDir, hdr.coarseDirOffset);
    writeGrid(fineCells, fineDir, hdr.fineDirOffset);
    writeGrid(ultraCells, ultraDir, hdr.ultraDirOffset);
    if (oversize) fprintf(stderr, "WARNING: %zu cells exceed %u bytes (firmware will skip them)\n", oversize, (unsigned)KNAV_MAX_CELL_BYTES);

    hdr.numPois = (uint32_t)allPois.size();
    hdr.poisOffset = (uint32_t)ftello(out);
    for (auto& po : allPois) {
        DiskPoi dpo{};
        dpo.lat_e5 = po.lat_e5; dpo.lon_e5 = po.lon_e5; dpo.category = po.category;
        strncpy(dpo.name, po.name.c_str(), sizeof(dpo.name) - 1);
        fwrite(&dpo, sizeof(dpo), 1, out);
    }

    int64_t totalBytes = ftello(out);
    if (totalBytes > 0xFFFFFFFFll) { fprintf(stderr, "output exceeds 4 GB\n"); fclose(out); return 1; }
    hdr.fileSize = (uint32_t)totalBytes;
    if (bbMinLat5 <= bbMaxLat5 && bbMinLon5 <= bbMaxLon5) {
        hdr.bboxMinLat_e5 = bbMinLat5; hdr.bboxMinLon_e5 = bbMinLon5;
        hdr.bboxMaxLat_e5 = bbMaxLat5; hdr.bboxMaxLon_e5 = bbMaxLon5;
    }

    fseeko(out, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, out);
    fseeko(out, hdr.coarseDirOffset, SEEK_SET);
    fwrite(coarseDir.data(), sizeof(CellDirEntry), coarseDir.size(), out);
    fseeko(out, hdr.fineDirOffset, SEEK_SET);
    fwrite(fineDir.data(), sizeof(CellDirEntry), fineDir.size(), out);
    fseeko(out, hdr.ultraDirOffset, SEEK_SET);
    fwrite(ultraDir.data(), sizeof(CellDirEntry), ultraDir.size(), out);
    fclose(out);

    printf("DONE: %s (%.2f MB, %s, %u places, %u route nodes, %u edges, %u POIs/streets, bbox %.3f,%.3f..%.3f,%.3f)\n",
           opt.outPath.c_str(), totalBytes / 1048576.0, KNAV_MAGIC_V3, hdr.numPlaces, hdr.numRouteNodes, hdr.numRouteEdges, hdr.numPois,
           hdr.bboxMinLat_e5 * 1e-5, hdr.bboxMinLon_e5 * 1e-5, hdr.bboxMaxLat_e5 * 1e-5, hdr.bboxMaxLon_e5 * 1e-5);
    if (flashMode && totalBytes > 9875ll * 1024) {
        fprintf(stderr, "WARNING: flash build is larger than the 9.875 MB partition\n");
    }
    return 0;
}
