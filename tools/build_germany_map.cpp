// Fast multi-threaded Germany OSM PBF -> KRONOS Navigation Database (germany.knav) compiler
// Extracts:
//  1. Gazetteer (all German cities, towns, villages, suburbs with population & coordinates)
//  2. Multi-mode Offline Routing Graph (Car vs. Pedestrian/Bike A* graph across Germany)
//  3. Two-tier Spatial Vector Map Tiles (Overview 32x32 + Detail 256x256 cells for Germany)
//
// Build: g++ -O3 -std=c++17 -pthread tools/build_germany_map.cpp -lz -o /tmp/build_germany_map

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
#include <zlib.h>

static constexpr double MIN_LAT = 47.20;
static constexpr double MAX_LAT = 55.10;
static constexpr double MIN_LON = 5.80;
static constexpr double MAX_LON = 15.10;

static constexpr int COARSE_NX = 32;
static constexpr int COARSE_NY = 32;
static constexpr int FINE_NX   = 256;
static constexpr int FINE_NY   = 256;

enum FeatureClass : uint8_t {
    FEAT_WATER       = 0,
    FEAT_BOUNDARY    = 1,
    FEAT_RAIL        = 2,
    FEAT_PATH        = 3,   // footway, cycleway, path, track, pedestrian
    FEAT_RESIDENTIAL = 4,   // residential, living_street, unclassified
    FEAT_TERTIARY    = 5,   // tertiary
    FEAT_SECONDARY   = 6,   // secondary
    FEAT_PRIMARY     = 7,   // trunk, primary
    FEAT_MOTORWAY    = 8,   // motorway
};

struct BlockIndex {
    int64_t offset;
    uint32_t datasize;
};

static uint64_t readVarint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}
static int64_t zigZag(uint64_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

static bool decompressBlock(FILE* f, const BlockIndex& bi, std::vector<uint8_t>& rawOut) {
    fseeko(f, bi.offset, SEEK_SET);
    uint8_t hlenBuf[4];
    if (fread(hlenBuf, 1, 4, f) != 4) return false;
    uint32_t hlen = (hlenBuf[0]<<24)|(hlenBuf[1]<<16)|(hlenBuf[2]<<8)|hlenBuf[3];
    fseeko(f, hlen, SEEK_CUR);
    std::vector<uint8_t> bbuf(bi.datasize);
    if (fread(bbuf.data(), 1, bi.datasize, f) != bi.datasize) return false;
    const uint8_t *p = bbuf.data(), *end = p + bi.datasize;
    uint32_t rawSize = 0, zlen = 0;
    const uint8_t *zptr = nullptr, *rptr = nullptr;
    uint32_t rlen = 0;
    while (p < end) {
        uint64_t tag = readVarint(p, end);
        uint32_t field = tag >> 3, wt = tag & 7;
        if (field == 1 && wt == 2) { rlen = readVarint(p, end); rptr = p; p += rlen; }
        else if (field == 2 && wt == 0) rawSize = readVarint(p, end);
        else if (field == 3 && wt == 2) { zlen = readVarint(p, end); zptr = p; p += zlen; }
        else if (wt == 0) readVarint(p, end);
        else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
        else break;
    }
    if (zptr && rawSize > 0) {
        rawOut.resize(rawSize);
        uLongf destLen = rawSize;
        return uncompress(rawOut.data(), &destLen, zptr, zlen) == Z_OK;
    } else if (rptr && rlen > 0) {
        rawOut.assign(rptr, rptr + rlen);
        return true;
    }
    return false;
}

static void parseStringTable(const uint8_t* p, const uint8_t* end, std::vector<std::string>& st) {
    st.clear();
    while (p < end) {
        uint64_t tag = readVarint(p, end);
        uint32_t field = tag >> 3, wt = tag & 7;
        if (field == 1 && wt == 2) {
            uint64_t l = readVarint(p, end);
            st.emplace_back((const char*)p, l);
            p += l;
        } else if (wt == 0) readVarint(p, end);
        else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
        else break;
    }
}

struct ExtractedWay {
    FeatureClass cls;
    bool routableCar;
    bool routablePed;
    std::vector<uint64_t> nodes;
};

struct PlaceEntry {
    int32_t lat_e5;
    int32_t lon_e5;
    uint32_t pop;
    uint8_t rank; // 1=city, 2=town, 3=suburb, 4=village
    std::string name;
};

// Normalize UTF-8 German umlauts for clean display and search while keeping readable characters
static std::string cleanPlaceName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 128) {
            out.push_back((char)c);
        } else if (c == 0xC3 && i + 1 < s.size()) {
            uint8_t c2 = (uint8_t)s[i + 1];
            // Keep standard Latin-1 UTF-8 (ä, ö, ü, Ä, Ö, Ü, ß, é, etc.) supported by our TTF font
            out.push_back((char)c);
            out.push_back((char)c2);
            i++;
        } else {
            // Skip exotic multi-byte sequences
        }
    }
    if (out.size() > 38) out.resize(38);
    return out;
}

int main(int argc, char** argv) {
    const char* pbfPath = "/home/michael/kronos-mapdata/osm/germany-latest.osm.pbf";
    const char* outPath = "/home/michael/kronos-mapdata/germany.knav";
    bool compactMode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compact") == 0) compactMode = true;
        else if (argv[i][0] != '-') outPath = argv[i];
    }

    FILE* f = fopen(pbfPath, "rb");
    if (!f) { perror("fopen pbf"); return 1; }

    printf("[1/5] Indexing PBF blocks in %s...\n", pbfPath);
    std::vector<BlockIndex> blocks;
    blocks.reserve(65000);
    while (true) {
        int64_t pos = ftello(f);
        uint8_t hlenBuf[4];
        if (fread(hlenBuf, 1, 4, f) != 4) break;
        uint32_t hlen = (hlenBuf[0]<<24)|(hlenBuf[1]<<16)|(hlenBuf[2]<<8)|hlenBuf[3];
        if (hlen > 65536) break;
        uint8_t hbuf[65536];
        if (fread(hbuf, 1, hlen, f) != hlen) break;
        const uint8_t *p = hbuf, *end = p + hlen;
        uint32_t datasize = 0;
        while (p < end) {
            uint64_t tag = readVarint(p, end);
            uint32_t field = tag >> 3, wt = tag & 7;
            if (field == 3 && wt == 0) datasize = readVarint(p, end);
            else if (wt == 0) readVarint(p, end);
            else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
        }
        blocks.push_back({pos, datasize});
        fseeko(f, datasize, SEEK_CUR);
    }
    fclose(f);

    // Known boundaries from our block inspection (or binary search)
    size_t firstWay = 55210;
    size_t firstRel = 64132;
    if (blocks.size() != 64247) {
        printf("Unexpected block count %zu\n", blocks.size());
        return 1;
    }
    printf("      %zu total blocks (Nodes: 1..%zu, Ways: %zu..%zu)\n",
           blocks.size(), firstWay - 1, firstWay, firstRel - 1);

    // -------------------------------------------------------------------------
    // PASS 1: Scan Way blocks (55210..64131) across 8 threads
    // -------------------------------------------------------------------------
    printf("[2/5] Scanning Ways (blocks %zu..%zu) on 8 threads...\n", firstWay, firstRel - 1);
    const int numThreads = 8;
    std::vector<std::vector<ExtractedWay>> threadWays(numThreads);
    std::atomic<size_t> nextWayBlock{firstWay};

    auto wayWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        std::vector<uint8_t> raw;
        std::vector<std::string> st;
        auto& outWays = threadWays[tid];
        outWays.reserve(150000);

        while (true) {
            size_t bidx = nextWayBlock.fetch_add(1);
            if (bidx >= firstRel) break;
            if (!decompressBlock(tf, blocks[bidx], raw)) continue;

            const uint8_t *p = raw.data(), *end = p + raw.size();
            st.clear();
            std::vector<std::pair<const uint8_t*, uint64_t>> groups;
            while (p < end) {
                uint64_t tag = readVarint(p, end);
                uint32_t field = tag >> 3, wt = tag & 7;
                if (field == 1 && wt == 2) {
                    uint64_t l = readVarint(p, end);
                    parseStringTable(p, p + l, st);
                    p += l;
                } else if (field == 2 && wt == 2) {
                    uint64_t l = readVarint(p, end);
                    groups.push_back({p, l});
                    p += l;
                } else if (wt == 0) readVarint(p, end);
                else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
                else break;
            }

            for (auto& g : groups) {
                const uint8_t *gp = g.first, *gend = gp + g.second;
                while (gp < gend) {
                    uint64_t gtag = readVarint(gp, gend);
                    uint32_t gfield = gtag >> 3, gwt = gtag & 7;
                    if (gfield == 3 && gwt == 2) { // Way
                        uint64_t wlen = readVarint(gp, gend);
                        const uint8_t *wp = gp, *wend = gp + wlen;
                        gp += wlen;

                        std::vector<uint32_t> keys, vals;
                        const uint8_t *refsPtr = nullptr, *refsEnd = nullptr;
                        while (wp < wend) {
                            uint64_t wtag = readVarint(wp, wend);
                            uint32_t wfield = wtag >> 3, wwt = wtag & 7;
                            if (wfield == 2 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend);
                                const uint8_t* kend = wp + l;
                                while (wp < kend) keys.push_back((uint32_t)readVarint(wp, kend));
                            } else if (wfield == 3 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend);
                                const uint8_t* vend = wp + l;
                                while (wp < vend) vals.push_back((uint32_t)readVarint(wp, vend));
                            } else if (wfield == 8 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend);
                                refsPtr = wp; refsEnd = wp + l;
                                wp += l;
                            } else if (wwt == 0) readVarint(wp, wend);
                            else if (wwt == 2) { uint64_t l = readVarint(wp, wend); wp += l; }
                            else break;
                        }
                        if (!refsPtr) continue;

                        int feat = -1;
                        bool rCar = false, rPed = false;
                        size_t nk = std::min(keys.size(), vals.size());
                        for (size_t i = 0; i < nk; i++) {
                            if (keys[i] >= st.size() || vals[i] >= st.size()) continue;
                            const std::string& k = st[keys[i]];
                            const std::string& v = st[vals[i]];
                            if (k == "highway") {
                                if (v == "motorway" || v == "motorway_link") {
                                    feat = FEAT_MOTORWAY; rCar = (v == "motorway");
                                } else if (v == "trunk" || v == "trunk_link" || v == "primary" || v == "primary_link") {
                                    feat = FEAT_PRIMARY; rCar = (v == "trunk" || v == "primary"); rPed = (v == "primary");
                                } else if (v == "secondary" || v == "secondary_link") {
                                    feat = FEAT_SECONDARY; rCar = (v == "secondary"); rPed = (v == "secondary");
                                } else if (v == "tertiary") {
                                    feat = FEAT_TERTIARY; rCar = true; rPed = true;
                                } else if (v == "residential" || v == "unclassified" || v == "living_street") {
                                    feat = FEAT_RESIDENTIAL;
                                } else if (v == "footway" || v == "cycleway" || v == "path" || v == "pedestrian") {
                                    feat = FEAT_PATH;
                                }
                            } else if (k == "waterway" && (v == "river" || v == "canal")) {
                                if (feat < 0) feat = FEAT_WATER;
                            } else if (k == "natural" && v == "coastline") {
                                if (feat < 0) feat = FEAT_BOUNDARY;
                            } else if (k == "railway" && v == "rail") {
                                if (feat < 0) feat = FEAT_RAIL;
                            }
                        }
                        if (feat < 0) continue;
                        if (compactMode && (feat == FEAT_PATH || feat == FEAT_RESIDENTIAL)) continue;

                        // Decode node refs
                        std::vector<uint64_t> refs;
                        int64_t curRef = 0;
                        while (refsPtr < refsEnd) {
                            curRef += zigZag(readVarint(refsPtr, refsEnd));
                            refs.push_back((uint64_t)curRef);
                        }
                        if (refs.size() < 2) continue;

                        // Subsample dense geometry to keep file compact while preserving endpoints
                        ExtractedWay ew;
                        ew.cls = (FeatureClass)feat;
                        ew.routableCar = rCar;
                        ew.routablePed = rPed;
                        size_t step = compactMode ? 3 : 1;
                        if (feat == FEAT_PATH || feat == FEAT_RESIDENTIAL) step = 3;
                        else if (feat == FEAT_TERTIARY || feat == FEAT_RAIL) step = compactMode ? 4 : 2;
                        else if (feat == FEAT_WATER || feat == FEAT_BOUNDARY) step = compactMode ? 6 : 4;

                        ew.nodes.push_back(refs.front());
                        for (size_t r = step; r + 1 < refs.size(); r += step) {
                            ew.nodes.push_back(refs[r]);
                        }
                        ew.nodes.push_back(refs.back());
                        outWays.push_back(std::move(ew));
                    } else if (gwt == 0) readVarint(gp, gend);
                    else if (gwt == 2) { uint64_t l = readVarint(gp, gend); gp += l; }
                    else break;
                }
            }
        }
        fclose(tf);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; t++) threads.emplace_back(wayWorker, t);
    for (auto& th : threads) th.join();

    size_t totalWays = 0;
    for (auto& tw : threadWays) totalWays += tw.size();
    printf("      Extracted %zu ways.\n", totalWays);

    // Collect unique node IDs needed by ways into a sorted flat vector (much lower RAM than hash table!)
    std::vector<uint64_t> neededNodes;
    neededNodes.reserve(totalWays * 3);
    for (auto& tw : threadWays) {
        for (auto& w : tw) {
            for (uint64_t nid : w.nodes) neededNodes.push_back(nid);
        }
    }
    printf("      Sorting & deduplicating %zu node references...\n", neededNodes.size());
    std::sort(neededNodes.begin(), neededNodes.end());
    neededNodes.erase(std::unique(neededNodes.begin(), neededNodes.end()), neededNodes.end());
    printf("      Unique needed nodes: %zu (%.1f MB)\n",
           neededNodes.size(), neededNodes.size() * 8.0 / 1048576.0);

    // Allocate parallel coordinate arrays (lat_e5, lon_e5) indexed by binary search in neededNodes
    std::vector<int32_t> nodeLat(neededNodes.size(), 0);
    std::vector<int32_t> nodeLon(neededNodes.size(), 0);

    // -------------------------------------------------------------------------
    // PASS 2: Scan DenseNodes blocks (1..firstWay-1) across 8 threads
    // -------------------------------------------------------------------------
    printf("[3/5] Scanning DenseNodes (blocks 1..%zu) for coordinates & places on 8 threads...\n", firstWay - 1);
    std::vector<std::vector<PlaceEntry>> threadPlaces(numThreads);
    std::atomic<size_t> nextNodeBlock{1};

    auto nodeWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        std::vector<uint8_t> raw;
        std::vector<std::string> st;
        auto& outPlaces = threadPlaces[tid];

        while (true) {
            size_t bidx = nextNodeBlock.fetch_add(1);
            if (bidx >= firstWay) break;
            if (!decompressBlock(tf, blocks[bidx], raw)) continue;

            const uint8_t *p = raw.data(), *end = p + raw.size();
            st.clear();
            int32_t granularity = 100;
            int64_t latOffset = 0, lonOffset = 0;
            std::vector<std::pair<const uint8_t*, uint64_t>> groups;

            while (p < end) {
                uint64_t tag = readVarint(p, end);
                uint32_t field = tag >> 3, wt = tag & 7;
                if (field == 1 && wt == 2) {
                    uint64_t l = readVarint(p, end);
                    parseStringTable(p, p + l, st);
                    p += l;
                } else if (field == 2 && wt == 2) {
                    uint64_t l = readVarint(p, end);
                    groups.push_back({p, l});
                    p += l;
                } else if (field == 17 && wt == 0) granularity = (int32_t)readVarint(p, end);
                else if (field == 19 && wt == 0) latOffset = (int64_t)readVarint(p, end);
                else if (field == 20 && wt == 0) lonOffset = (int64_t)readVarint(p, end);
                else if (wt == 0) readVarint(p, end);
                else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
                else break;
            }

            for (auto& g : groups) {
                const uint8_t *gp = g.first, *gend = gp + g.second;
                while (gp < gend) {
                    uint64_t gtag = readVarint(gp, gend);
                    uint32_t gfield = gtag >> 3, gwt = gtag & 7;
                    if (gfield == 2 && gwt == 2) { // DenseNodes
                        uint64_t dlen = readVarint(gp, gend);
                        const uint8_t *dp = gp, *dend = gp + dlen;
                        gp += dlen;

                        const uint8_t *idP = nullptr, *idE = nullptr;
                        const uint8_t *latP = nullptr, *latE = nullptr;
                        const uint8_t *lonP = nullptr, *lonE = nullptr;
                        const uint8_t *kvP = nullptr, *kvE = nullptr;

                        while (dp < dend) {
                            uint64_t dtag = readVarint(dp, dend);
                            uint32_t dfield = dtag >> 3, dwt = dtag & 7;
                            if (dfield == 1 && dwt == 2) { uint64_t l = readVarint(dp, dend); idP = dp; idE = dp + l; dp += l; }
                            else if (dfield == 8 && dwt == 2) { uint64_t l = readVarint(dp, dend); latP = dp; latE = dp + l; dp += l; }
                            else if (dfield == 9 && dwt == 2) { uint64_t l = readVarint(dp, dend); lonP = dp; lonE = dp + l; dp += l; }
                            else if (dfield == 10 && dwt == 2) { uint64_t l = readVarint(dp, dend); kvP = dp; kvE = dp + l; dp += l; }
                            else if (dwt == 0) readVarint(dp, dend);
                            else if (dwt == 2) { uint64_t l = readVarint(dp, dend); dp += l; }
                            else break;
                        }
                        if (!idP || !latP || !lonP) continue;

                        int64_t curId = 0, curLat = 0, curLon = 0;
                        auto itHint = neededNodes.begin();
                        while (idP < idE && latP < latE && lonP < lonE) {
                            curId  += zigZag(readVarint(idP, idE));
                            curLat += zigZag(readVarint(latP, latE));
                            curLon += zigZag(readVarint(lonP, lonE));

                            // Convert nanodegrees to 1e-5 degrees (int32_t)
                            int64_t nanoLat = latOffset + (int64_t)granularity * curLat;
                            int64_t nanoLon = lonOffset + (int64_t)granularity * curLon;
                            int32_t lat5 = (int32_t)(nanoLat / 10000);
                            int32_t lon5 = (int32_t)(nanoLon / 10000);

                            // Since curId is strictly increasing within DenseNodes, advance itHint
                            uint64_t uid = (uint64_t)curId;
                            if (itHint != neededNodes.end() && *itHint < uid) {
                                itHint = std::lower_bound(itHint, neededNodes.end(), uid);
                            }
                            if (itHint != neededNodes.end() && *itHint == uid) {
                                size_t idx = (size_t)(itHint - neededNodes.begin());
                                nodeLat[idx] = lat5;
                                nodeLon[idx] = lon5;
                                ++itHint;
                            }

                            // Parse keys_vals for this node to see if it's a named place
                            if (kvP && kvP < kvE) {
                                uint8_t placeRank = 0;
                                uint32_t pop = 0;
                                const std::string* namePtr = nullptr;
                                const std::string* deNamePtr = nullptr;
                                while (kvP < kvE) {
                                    uint32_t kIdx = (uint32_t)readVarint(kvP, kvE);
                                    if (kIdx == 0) break;
                                    uint32_t vIdx = (uint32_t)readVarint(kvP, kvE);
                                    if (kIdx < st.size() && vIdx < st.size()) {
                                        const std::string& k = st[kIdx];
                                        const std::string& v = st[vIdx];
                                        if (k == "place") {
                                            if (v == "city") placeRank = 1;
                                            else if (v == "town") placeRank = 2;
                                            else if (v == "suburb" || v == "borough") placeRank = 3;
                                            else if (v == "village" || v == "municipality") placeRank = 4;
                                        } else if (k == "name") {
                                            namePtr = &v;
                                        } else if (k == "name:de") {
                                            deNamePtr = &v;
                                        } else if (k == "population") {
                                            pop = (uint32_t)atoi(v.c_str());
                                        }
                                    }
                                }
                                const std::string* bestName = deNamePtr ? deNamePtr : namePtr;
                                if (placeRank > 0 && bestName && !bestName->empty()) {
                                    PlaceEntry pe;
                                    pe.lat_e5 = lat5;
                                    pe.lon_e5 = lon5;
                                    pe.pop = pop;
                                    pe.rank = placeRank;
                                    pe.name = cleanPlaceName(*bestName);
                                    if (!pe.name.empty()) outPlaces.push_back(std::move(pe));
                                }
                            }
                        }
                    } else if (gwt == 0) readVarint(gp, gend);
                    else if (gwt == 2) { uint64_t l = readVarint(gp, gend); gp += l; }
                    else break;
                }
            }
        }
        fclose(tf);
    };

    threads.clear();
    for (int t = 0; t < numThreads; t++) threads.emplace_back(nodeWorker, t);
    for (auto& th : threads) th.join();

    // Merge places
    std::vector<PlaceEntry> allPlaces;
    for (auto& tp : threadPlaces) {
        for (auto& p : tp) allPlaces.push_back(std::move(p));
    }
    // Sort places by rank (cities first), then population descending, then name
    std::sort(allPlaces.begin(), allPlaces.end(), [](const PlaceEntry& a, const PlaceEntry& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.pop != b.pop) return a.pop > b.pop;
        return a.name < b.name;
    });
    size_t maxPlaces = compactMode ? 4500 : 25000;
    if (allPlaces.size() > maxPlaces) allPlaces.resize(maxPlaces);
    printf("      Collected %zu places (cities/towns/villages).\n", allPlaces.size());

    // -------------------------------------------------------------------------
    // STEP 4: Build Spatial Vector Tiles (Coarse 32x32 + Fine 256x256) & Routing Graph
    // -------------------------------------------------------------------------
    printf("[4/5] Building spatial tiles & routing graph...\n");

    struct PolySeg {
        uint8_t cls;
        std::vector<std::pair<uint8_t, uint8_t>> pts; // quantized 0..255 inside cell
    };
    std::vector<std::vector<PolySeg>> coarseCells(COARSE_NX * COARSE_NY);
    std::vector<std::vector<PolySeg>> fineCells(FINE_NX * FINE_NY);

    // For routing graph: grid-snapped junctions at ~500m resolution for major roads across Germany
    // Key: ((uint64_t)(lat_e5 / 450) << 32) | (uint32_t)(lon_e5 / 650)
    struct RouteEdgeTmp {
        uint32_t to;
        uint16_t dist_hm; // in 100m units (hectometers)
        uint8_t flags;    // bit0=car, bit1=ped, bits2..4=FeatureClass
    };
    struct RouteNodeTmp {
        int32_t lat_e5;
        int32_t lon_e5;
        std::vector<RouteEdgeTmp> edges;
    };
    std::unordered_map<uint64_t, uint32_t> junctionMap;
    std::vector<RouteNodeTmp> routeNodes;
    routeNodes.reserve(60000);

    auto getJunctionNode = [&](int32_t lat5, int32_t lon5) -> uint32_t {
        int32_t gy = lat5 / (compactMode ? 3200 : 2000);
        int32_t gx = lon5 / (compactMode ? 4800 : 3000);
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
        lat5 = nodeLat[idx];
        lon5 = nodeLon[idx];
        return (lat5 != 0 || lon5 != 0);
    };

    const double latSpan = MAX_LAT - MIN_LAT;
    const double lonSpan = MAX_LON - MIN_LON;

    for (auto& tw : threadWays) {
        for (auto& w : tw) {
            std::vector<std::pair<int32_t, int32_t>> coords;
            coords.reserve(w.nodes.size());
            for (uint64_t nid : w.nodes) {
                int32_t la, lo;
                if (lookupCoord(nid, la, lo)) {
                    double dlat = la * 1e-5, dlon = lo * 1e-5;
                    if (dlat >= MIN_LAT && dlat <= MAX_LAT && dlon >= MIN_LON && dlon <= MAX_LON) {
                        coords.push_back({la, lo});
                    }
                }
            }
            if (coords.size() < 2) continue;

            // Add to routing graph if routable (Motorway, Primary, Secondary, Tertiary)
            // Step through ALL vertices along the way so every intermediate intersection connects!
            if (w.routableCar || w.routablePed) {
                uint8_t flags = (w.routableCar ? 1 : 0) | (w.routablePed ? 2 : 0) | ((uint8_t)w.cls << 2);
                auto addEdge = [&](uint32_t a, uint32_t b, uint16_t hm) {
                    for (auto& e : routeNodes[a].edges) {
                        if (e.to == b) {
                            if (hm < e.dist_hm) e.dist_hm = hm;
                            e.flags |= flags;
                            return;
                        }
                    }
                    if (routeNodes[a].edges.size() < 10) {
                        routeNodes[a].edges.push_back({b, hm, flags});
                    }
                };

                uint32_t prevU = getJunctionNode(coords[0].first, coords[0].second);
                for (size_t i = 1; i < coords.size(); i++) {
                    uint32_t u = getJunctionNode(coords[i].first, coords[i].second);
                    if (u != prevU) {
                        double dlat = (routeNodes[prevU].lat_e5 - routeNodes[u].lat_e5) * 1e-5 * 111.32;
                        double dlon = (routeNodes[prevU].lon_e5 - routeNodes[u].lon_e5) * 1e-5 * 71.5;
                        double km = std::sqrt(dlat * dlat + dlon * dlon);
                        if (km < 25.0) { // sanity cap against corrupt OSM ways
                            uint16_t hm = (uint16_t)std::max(1.0, std::min(65535.0, km * 10.0));
                            addEdge(prevU, u, hm);
                            addEdge(u, prevU, hm);
                        }
                        prevU = u;
                    }
                }
            }

            // Add to Coarse Grid (32x32) if major feature
            if (w.cls == FEAT_MOTORWAY || w.cls == FEAT_PRIMARY || w.cls == FEAT_WATER || w.cls == FEAT_BOUNDARY) {
                int curCell = -1;
                PolySeg seg;
                seg.cls = (uint8_t)w.cls;
                for (auto& pt : coords) {
                    double ny = (pt.first * 1e-5 - MIN_LAT) / latSpan * COARSE_NY;
                    double nx = (pt.second * 1e-5 - MIN_LON) / lonSpan * COARSE_NX;
                    int cy = std::clamp((int)ny, 0, COARSE_NY - 1);
                    int cx = std::clamp((int)nx, 0, COARSE_NX - 1);
                    int cell = cy * COARSE_NX + cx;
                    uint8_t qx = (uint8_t)std::clamp((int)((nx - cx) * 255.0), 0, 255);
                    uint8_t qy = (uint8_t)std::clamp((int)((ny - cy) * 255.0), 0, 255);
                    if (cell != curCell) {
                        if (curCell >= 0 && seg.pts.size() >= 2 && coarseCells[curCell].size() < 450) {
                            coarseCells[curCell].push_back(seg);
                        }
                        seg.pts.clear();
                        curCell = cell;
                    }
                    if (seg.pts.empty() || seg.pts.back() != std::make_pair(qx, qy)) {
                        seg.pts.push_back({qx, qy});
                    }
                }
                if (curCell >= 0 && seg.pts.size() >= 2 && coarseCells[curCell].size() < 450) {
                    coarseCells[curCell].push_back(seg);
                }
            }

            // Add to Fine Grid (256x256) - prioritize higher classes and cap per-cell segments
            {
                size_t maxSegs = compactMode ? 22 : ((w.cls >= FEAT_TERTIARY || w.cls <= FEAT_RAIL) ? 90 : 55);
                int curCell = -1;
                PolySeg seg;
                seg.cls = (uint8_t)w.cls;
                for (auto& pt : coords) {
                    double ny = (pt.first * 1e-5 - MIN_LAT) / latSpan * FINE_NY;
                    double nx = (pt.second * 1e-5 - MIN_LON) / lonSpan * FINE_NX;
                    int cy = std::clamp((int)ny, 0, FINE_NY - 1);
                    int cx = std::clamp((int)nx, 0, FINE_NX - 1);
                    int cell = cy * FINE_NX + cx;
                    uint8_t qx = (uint8_t)std::clamp((int)((nx - cx) * 255.0), 0, 255);
                    uint8_t qy = (uint8_t)std::clamp((int)((ny - cy) * 255.0), 0, 255);
                    if (cell != curCell) {
                        if (curCell >= 0 && seg.pts.size() >= 2 && fineCells[curCell].size() < maxSegs) {
                            fineCells[curCell].push_back(seg);
                        }
                        seg.pts.clear();
                        curCell = cell;
                    }
                    if (seg.pts.empty() || seg.pts.back() != std::make_pair(qx, qy)) {
                        seg.pts.push_back({qx, qy});
                    }
                }
                if (curCell >= 0 && seg.pts.size() >= 2 && fineCells[curCell].size() < maxSegs) {
                    fineCells[curCell].push_back(seg);
                }
            }
        }
    }

    size_t totalEdges = 0;
    for (auto& rn : routeNodes) totalEdges += rn.edges.size();
    printf("      Routing Graph: %zu nodes, %zu directed edges.\n", routeNodes.size(), totalEdges);

    // -------------------------------------------------------------------------
    // STEP 5: Write binary file `germany.knav`
    // -------------------------------------------------------------------------
    printf("[5/5] Writing %s...\n", outPath);
    FILE* out = fopen(outPath, "wb");
    if (!out) { perror("fopen out"); return 1; }

    struct KnavHeader {
        char magic[8];          // "KNAVDE01"
        uint32_t numPlaces;
        uint32_t placesOffset;
        uint32_t numRouteNodes;
        uint32_t numRouteEdges;
        uint32_t routeNodesOffset;
        uint32_t routeEdgesOffset;
        uint32_t coarseDirOffset; // 32*32 entries of (offset, size)
        uint32_t fineDirOffset;   // 256*256 entries of (offset, size)
    } hdr{};
    memcpy(hdr.magic, "KNAVDE01", 8);
    fwrite(&hdr, sizeof(hdr), 1, out);

    // 1. Write Places (fixed 48-byte records for instant O(1) random access on ESP32!)
    struct DiskPlace {
        int32_t lat_e5;
        int32_t lon_e5;
        uint32_t pop;
        uint8_t rank;
        char name[35];
    };
    hdr.numPlaces = (uint32_t)allPlaces.size();
    hdr.placesOffset = (uint32_t)ftell(out);
    for (auto& p : allPlaces) {
        DiskPlace dp{};
        dp.lat_e5 = p.lat_e5;
        dp.lon_e5 = p.lon_e5;
        dp.pop = p.pop;
        dp.rank = p.rank;
        strncpy(dp.name, p.name.c_str(), sizeof(dp.name) - 1);
        fwrite(&dp, sizeof(dp), 1, out);
    }

    // 2. Write Routing Graph
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
    hdr.numRouteNodes = (uint32_t)routeNodes.size();
    hdr.numRouteEdges = (uint32_t)totalEdges;
    hdr.routeNodesOffset = (uint32_t)ftell(out);
    uint32_t curEdgeIdx = 0;
    for (auto& rn : routeNodes) {
        DiskRouteNode drn{rn.lat_e5, rn.lon_e5, curEdgeIdx, (uint16_t)rn.edges.size(), 0};
        fwrite(&drn, sizeof(drn), 1, out);
        curEdgeIdx += (uint32_t)rn.edges.size();
    }
    hdr.routeEdgesOffset = (uint32_t)ftell(out);
    for (auto& rn : routeNodes) {
        for (auto& e : rn.edges) {
            DiskRouteEdge dre{e.to, e.dist_hm, e.flags, 0};
            fwrite(&dre, sizeof(dre), 1, out);
        }
    }

    // 3. Write Coarse Cells Directory & Payloads
    struct CellDirEntry { uint32_t offset; uint32_t length; };
    std::vector<CellDirEntry> coarseDir(COARSE_NX * COARSE_NY);
    hdr.coarseDirOffset = (uint32_t)ftell(out);
    fwrite(coarseDir.data(), sizeof(CellDirEntry), coarseDir.size(), out);

    for (size_t i = 0; i < coarseCells.size(); i++) {
        coarseDir[i].offset = (uint32_t)ftell(out);
        uint16_t nseg = (uint16_t)std::min<size_t>(coarseCells[i].size(), 65535);
        fwrite(&nseg, 2, 1, out);
        for (uint16_t s = 0; s < nseg; s++) {
            auto& seg = coarseCells[i][s];
            uint8_t cls = seg.cls;
            uint8_t npts = (uint8_t)std::min<size_t>(seg.pts.size(), 255);
            fwrite(&cls, 1, 1, out);
            fwrite(&npts, 1, 1, out);
            for (uint8_t k = 0; k < npts; k++) {
                uint8_t xy[2] = {seg.pts[k].first, seg.pts[k].second};
                fwrite(xy, 1, 2, out);
            }
        }
        coarseDir[i].length = (uint32_t)ftell(out) - coarseDir[i].offset;
    }

    // 4. Write Fine Cells Directory & Payloads
    std::vector<CellDirEntry> fineDir(FINE_NX * FINE_NY);
    hdr.fineDirOffset = (uint32_t)ftell(out);
    fwrite(fineDir.data(), sizeof(CellDirEntry), fineDir.size(), out);

    for (size_t i = 0; i < fineCells.size(); i++) {
        if (fineCells[i].empty()) {
            fineDir[i] = {0, 0};
            continue;
        }
        fineDir[i].offset = (uint32_t)ftell(out);
        uint16_t nseg = (uint16_t)std::min<size_t>(fineCells[i].size(), 65535);
        fwrite(&nseg, 2, 1, out);
        for (uint16_t s = 0; s < nseg; s++) {
            auto& seg = fineCells[i][s];
            uint8_t cls = seg.cls;
            uint8_t npts = (uint8_t)std::min<size_t>(seg.pts.size(), 255);
            fwrite(&cls, 1, 1, out);
            fwrite(&npts, 1, 1, out);
            for (uint8_t k = 0; k < npts; k++) {
                uint8_t xy[2] = {seg.pts[k].first, seg.pts[k].second};
                fwrite(xy, 1, 2, out);
            }
        }
        fineDir[i].length = (uint32_t)ftell(out) - fineDir[i].offset;
    }

    // Rewrite header and directory tables
    uint32_t totalBytes = (uint32_t)ftell(out);
    fseek(out, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, out);
    fseek(out, hdr.coarseDirOffset, SEEK_SET);
    fwrite(coarseDir.data(), sizeof(CellDirEntry), coarseDir.size(), out);
    fseek(out, hdr.fineDirOffset, SEEK_SET);
    fwrite(fineDir.data(), sizeof(CellDirEntry), fineDir.size(), out);
    fclose(out);

    printf("DONE! Generated %s (%.2f MB)\n", outPath, totalBytes / 1048576.0);
    return 0;
}
