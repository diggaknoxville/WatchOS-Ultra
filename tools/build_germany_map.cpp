// Fast multi-threaded Germany + Ultra-Detailed Stuttgart & Adjacent Landkreise OSM PBF Compiler (KNAVDE02)
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

// Stuttgart + adjacent Landkreise (Böblingen, Esslingen, Göppingen, Rems-Murr, Ludwigsburg,
// Heilbronn-Süd, Enzkreis-Ost, Calw-Ost, Tübingen/Reutlingen-Nord)
static constexpr double STG_MIN_LAT = 48.42;
static constexpr double STG_MAX_LAT = 49.18;
static constexpr double STG_MIN_LON = 8.75;
static constexpr double STG_MAX_LON = 9.75;

static constexpr int COARSE_NX = 32;
static constexpr int COARSE_NY = 32;
static constexpr int FINE_NX   = 256;
static constexpr int FINE_NY   = 256;
static constexpr int ULTRA_NX  = 128;
static constexpr int ULTRA_NY  = 128;

enum FeatureClass : uint8_t {
    // Area / Polygon features (0..5)
    FEAT_FARMLAND    = 0,   // landuse=farmland, meadow, orchard, vineyard
    FEAT_FOREST      = 1,   // landuse=forest, natural=wood
    FEAT_PARK        = 2,   // leisure=park, garden, landuse=grass
    FEAT_URBAN       = 3,   // landuse=residential, commercial, industrial, retail
    FEAT_WATER_POLY  = 4,   // natural=water, waterway=riverbank, reservoir
    FEAT_BUILDING    = 5,   // building=*
    // Linear features (6..14)
    FEAT_WATER       = 6,   // waterway=river, canal, stream
    FEAT_BOUNDARY    = 7,   // coastline / administrative
    FEAT_RAIL        = 8,   // rail, light_rail, subway, tram
    FEAT_PATH        = 9,   // footway, cycleway, path, track, pedestrian, steps
    FEAT_RESIDENTIAL = 10,  // residential, living_street, unclassified, service
    FEAT_TERTIARY    = 11,  // tertiary
    FEAT_SECONDARY   = 12,  // secondary
    FEAT_PRIMARY     = 13,  // trunk, primary
    FEAT_MOTORWAY    = 14,  // motorway
};

enum PoiCategory : uint8_t {
    POI_NONE       = 0,
    POI_TRANSIT    = 1, // S-Bahn, U-Bahn, Bahnhof, Haltestelle
    POI_PARKING    = 2, // Parkplatz, Parkhaus
    POI_FUEL       = 3, // Tankstelle, Ladesäule
    POI_FOOD       = 4, // Restaurant, Café, Biergarten, Bäckerei
    POI_SHOP       = 5, // Supermarkt, Einkaufszentrum
    POI_HEALTH     = 6, // Krankenhaus, Apotheke, Arzt
    POI_SIGHT      = 7, // Sehenswürdigkeit, Aussichtspunkt, Museum, Burg/Schloss
    POI_STREET     = 8, // Straßennamen-Label
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

static std::string cleanText(const std::string& s, size_t maxLen = 34) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 128) {
            out.push_back((char)c);
        } else if (c == 0xC3 && i + 1 < s.size()) {
            out.push_back((char)c);
            out.push_back(s[i + 1]);
            i++;
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
    if (out.size() > maxLen) out.resize(maxLen);
    return out;
}

struct ExtractedWay {
    FeatureClass cls;
    bool routableCar;
    bool routablePed;
    std::string streetName;
    std::vector<uint64_t> nodes;
};

struct ResolvedWay {
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
    return lat >= STG_MIN_LAT && lat <= STG_MAX_LAT && lon >= STG_MIN_LON && lon <= STG_MAX_LON;
}

static inline bool inStuttgartBuildingsZone(double lat, double lon) {
    return lat >= 48.64 && lat <= 48.92 && lon >= 8.98 && lon <= 9.38;
}

int main(int argc, char** argv) {
    const char* pbfPath = "/home/michael/kronos-mapdata/osm/germany-latest.osm.pbf";
    const char* outPath = "/home/michael/kronos-mapdata/germany.knav";
    bool flashMode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--flash") == 0 || strcmp(argv[i], "--compact") == 0) flashMode = true;
        else if (argv[i][0] != '-') outPath = argv[i];
    }

    FILE* f = fopen(pbfPath, "rb");
    if (!f) { perror("fopen pbf"); return 1; }

    printf("[1/6] Indexing PBF blocks in %s (flashMode=%d)...\n", pbfPath, (int)flashMode);
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

    size_t firstWay = 55210;
    size_t firstRel = 64132;
    const int numThreads = 8;

    // -------------------------------------------------------------------------
    // PASS 1: Scan Way blocks for NATIONWIDE roads/rail/water (fast, ~650k ways)
    // -------------------------------------------------------------------------
    printf("[2/6] Pass 1: Scanning Nationwide Ways (blocks %zu..%zu)...\n", firstWay, firstRel - 1);
    std::vector<std::vector<ExtractedWay>> threadWays(numThreads);
    std::atomic<size_t> nextWayBlock{firstWay};

    auto wayNationWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        std::vector<uint8_t> raw;
        std::vector<std::string> st;
        auto& outWays = threadWays[tid];
        outWays.reserve(100000);

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
                if (field == 1 && wt == 2) { uint64_t l = readVarint(p, end); parseStringTable(p, p + l, st); p += l; }
                else if (field == 2 && wt == 2) { uint64_t l = readVarint(p, end); groups.push_back({p, l}); p += l; }
                else if (wt == 0) readVarint(p, end);
                else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
                else break;
            }

            for (auto& g : groups) {
                const uint8_t *gp = g.first, *gend = gp + g.second;
                while (gp < gend) {
                    uint64_t gtag = readVarint(gp, gend);
                    uint32_t gfield = gtag >> 3, gwt = gtag & 7;
                    if (gfield == 3 && gwt == 2) {
                        uint64_t wlen = readVarint(gp, gend);
                        const uint8_t *wp = gp, *wend = gp + wlen;
                        gp += wlen;

                        std::vector<uint32_t> keys, vals;
                        const uint8_t *refsPtr = nullptr, *refsEnd = nullptr;
                        while (wp < wend) {
                            uint64_t wtag = readVarint(wp, wend);
                            uint32_t wfield = wtag >> 3, wwt = wtag & 7;
                            if (wfield == 2 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend); const uint8_t* kend = wp + l;
                                while (wp < kend) keys.push_back((uint32_t)readVarint(wp, kend));
                            } else if (wfield == 3 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend); const uint8_t* vend = wp + l;
                                while (wp < vend) vals.push_back((uint32_t)readVarint(wp, vend));
                            } else if (wfield == 8 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend); refsPtr = wp; refsEnd = wp + l; wp += l;
                            } else if (wwt == 0) readVarint(wp, wend);
                            else if (wwt == 2) { uint64_t l = readVarint(wp, wend); wp += l; }
                            else break;
                        }
                        if (!refsPtr) continue;

                        int feat = -1;
                        bool rCar = false, rPed = false;
                        const std::string* namePtr = nullptr;
                        size_t nk = std::min(keys.size(), vals.size());
                        for (size_t i = 0; i < nk; i++) {
                            if (keys[i] >= st.size() || vals[i] >= st.size()) continue;
                            const std::string& k = st[keys[i]];
                            const std::string& v = st[vals[i]];
                            if (k == "highway") {
                                if (v == "motorway" || v == "motorway_link") {
                                    feat = FEAT_MOTORWAY; rCar = (v == "motorway");
                                } else if (v == "trunk" || v == "trunk_link" || v == "primary" || v == "primary_link") {
                                    feat = FEAT_PRIMARY; rCar = true; rPed = (v == "primary");
                                } else if (v == "secondary" || v == "secondary_link") {
                                    feat = FEAT_SECONDARY; rCar = (v == "secondary"); rPed = true;
                                } else if (v == "tertiary") {
                                    feat = FEAT_TERTIARY; rCar = true; rPed = true;
                                }
                            } else if (k == "waterway" && (v == "river" || v == "canal")) {
                                if (feat < 0) feat = FEAT_WATER;
                            } else if (k == "natural" && v == "coastline") {
                                if (feat < 0) feat = FEAT_BOUNDARY;
                            } else if (k == "railway" && (v == "rail" || v == "light_rail" || v == "subway")) {
                                if (feat < 0) feat = FEAT_RAIL;
                            } else if (k == "name") {
                                namePtr = &v;
                            }
                        }
                        if (feat < 0) continue;

                        std::vector<uint64_t> refs;
                        int64_t curRef = 0;
                        while (refsPtr < refsEnd) {
                            curRef += zigZag(readVarint(refsPtr, refsEnd));
                            refs.push_back((uint64_t)curRef);
                        }
                        if (refs.size() < 2) continue;

                        ExtractedWay ew;
                        ew.cls = (FeatureClass)feat;
                        ew.routableCar = rCar;
                        ew.routablePed = rPed;
                        if (namePtr && !namePtr->empty()) ew.streetName = cleanText(*namePtr, 26);
                        size_t step = (feat >= FEAT_PRIMARY) ? 2 : 3;
                        ew.nodes.push_back(refs.front());
                        for (size_t r = step; r + 1 < refs.size(); r += step) ew.nodes.push_back(refs[r]);
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
    for (int t = 0; t < numThreads; t++) threads.emplace_back(wayNationWorker, t);
    for (auto& th : threads) th.join();

    std::vector<uint64_t> neededNodes;
    for (auto& tw : threadWays) for (auto& w : tw) for (uint64_t nid : w.nodes) neededNodes.push_back(nid);
    std::sort(neededNodes.begin(), neededNodes.end());
    neededNodes.erase(std::unique(neededNodes.begin(), neededNodes.end()), neededNodes.end());
    printf("      Nationwide ways unique nodes: %zu\n", neededNodes.size());

    std::vector<int32_t> nodeLat(neededNodes.size(), 0);
    std::vector<int32_t> nodeLon(neededNodes.size(), 0);

    // -------------------------------------------------------------------------
    // PASS 2: Scan DenseNodes (1..firstWay-1) -> resolve nationwide nodes + collect ALL Stuttgart-Metro nodes!
    // -------------------------------------------------------------------------
    printf("[3/6] Pass 2: Scanning DenseNodes for Germany coords + Stuttgart Metro nodes, places & POIs...\n");
    std::vector<std::vector<PlaceEntry>> threadPlaces(numThreads);
    std::vector<std::vector<PoiEntry>> threadPois(numThreads);
    std::vector<std::vector<StgNode>> threadStgNodes(numThreads);
    std::atomic<size_t> nextNodeBlock{1};

    auto nodeWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        std::vector<uint8_t> raw;
        std::vector<std::string> st;
        auto& outPlaces = threadPlaces[tid];
        auto& outPois = threadPois[tid];
        auto& outStg = threadStgNodes[tid];
        outStg.reserve(800000);

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
                if (field == 1 && wt == 2) { uint64_t l = readVarint(p, end); parseStringTable(p, p + l, st); p += l; }
                else if (field == 2 && wt == 2) { uint64_t l = readVarint(p, end); groups.push_back({p, l}); p += l; }
                else if (field == 17 && wt == 0) granularity = (int32_t)readVarint(p, end);
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
                    if (gfield == 2 && gwt == 2) {
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

                            int64_t nanoLat = latOffset + (int64_t)granularity * curLat;
                            int64_t nanoLon = lonOffset + (int64_t)granularity * curLon;
                            int32_t lat5 = (int32_t)(nanoLat / 10000);
                            int32_t lon5 = (int32_t)(nanoLon / 10000);
                            double dlat = lat5 * 1e-5, dlon = lon5 * 1e-5;
                            bool isStg = inStuttgartMetro(dlat, dlon);

                            uint64_t uid = (uint64_t)curId;
                            if (isStg) {
                                outStg.push_back({uid, lat5, lon5});
                            }
                            if (itHint != neededNodes.end() && *itHint < uid) {
                                itHint = std::lower_bound(itHint, neededNodes.end(), uid);
                            }
                            if (itHint != neededNodes.end() && *itHint == uid) {
                                size_t idx = (size_t)(itHint - neededNodes.begin());
                                nodeLat[idx] = lat5;
                                nodeLon[idx] = lon5;
                                ++itHint;
                            }

                            if (kvP && kvP < kvE) {
                                uint8_t placeRank = 0;
                                uint8_t poiCat = 0;
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
                                            else if (v == "village" || v == "municipality" || (isStg && (v == "quarter" || v == "neighbourhood" || v == "hamlet"))) placeRank = 4;
                                        } else if (isStg && k == "railway") {
                                            if (v == "station" || v == "halt" || v == "tram_stop" || v == "subway_entrance") poiCat = POI_TRANSIT;
                                        } else if (isStg && k == "amenity") {
                                            if (v == "parking") poiCat = POI_PARKING;
                                            else if (v == "fuel" || v == "charging_station") poiCat = POI_FUEL;
                                            else if (v == "restaurant" || v == "cafe" || v == "biergarten" || v == "fast_food") poiCat = POI_FOOD;
                                            else if (v == "hospital" || v == "pharmacy" || v == "doctors") poiCat = POI_HEALTH;
                                        } else if (isStg && k == "shop") {
                                            if (v == "supermarket" || v == "bakery" || v == "mall") poiCat = POI_SHOP;
                                        } else if (isStg && (k == "tourism" || k == "historic" || k == "natural")) {
                                            if (v == "attraction" || v == "viewpoint" || v == "museum" || v == "castle" || v == "peak") poiCat = POI_SIGHT;
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
                                    pe.pop = isStg ? (pop + 25000) : pop;
                                    pe.rank = placeRank;
                                    pe.name = cleanText(*bestName, 34);
                                    if (!pe.name.empty()) outPlaces.push_back(std::move(pe));
                                } else if (isStg && poiCat > 0 && bestName && !bestName->empty()) {
                                    PoiEntry po;
                                    po.lat_e5 = lat5;
                                    po.lon_e5 = lon5;
                                    po.category = poiCat;
                                    po.name = cleanText(*bestName, 26);
                                    if (!po.name.empty()) outPois.push_back(std::move(po));
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

    // Merge & sort Stuttgart Metro nodes for O(log N) lookup
    size_t totalStgNodes = 0;
    for (auto& ts : threadStgNodes) totalStgNodes += ts.size();
    std::vector<StgNode> stgNodes;
    stgNodes.reserve(totalStgNodes);
    for (auto& ts : threadStgNodes) {
        stgNodes.insert(stgNodes.end(), ts.begin(), ts.end());
        ts.clear();
        ts.shrink_to_fit();
    }
    std::sort(stgNodes.begin(), stgNodes.end());
    printf("      Collected %zu Stuttgart-Metro nodes (%.1f MB).\n",
           stgNodes.size(), stgNodes.size() * sizeof(StgNode) / 1048576.0);

    auto lookupStgNode = [&](uint64_t nid, int32_t& lat5, int32_t& lon5) -> bool {
        StgNode key{nid, 0, 0};
        auto it = std::lower_bound(stgNodes.begin(), stgNodes.end(), key);
        if (it == stgNodes.end() || it->id != nid) return false;
        lat5 = it->lat_e5;
        lon5 = it->lon_e5;
        return true;
    };

    // -------------------------------------------------------------------------
    // PASS 3: Scan Way blocks for Stuttgart-Metro Buildings, Forests, Farmland, Residential, Paths!
    // -------------------------------------------------------------------------
    printf("[4/6] Pass 3: Extracting Stuttgart-Metro Buildings, Forests, Farmland, Streets & Paths...\n");
    std::vector<std::vector<ResolvedWay>> threadStgWays(numThreads);
    nextWayBlock.store(firstWay);

    auto wayStgWorker = [&](int tid) {
        FILE* tf = fopen(pbfPath, "rb");
        std::vector<uint8_t> raw;
        std::vector<std::string> st;
        auto& outWays = threadStgWays[tid];
        outWays.reserve(80000);

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
                if (field == 1 && wt == 2) { uint64_t l = readVarint(p, end); parseStringTable(p, p + l, st); p += l; }
                else if (field == 2 && wt == 2) { uint64_t l = readVarint(p, end); groups.push_back({p, l}); p += l; }
                else if (wt == 0) readVarint(p, end);
                else if (wt == 2) { uint64_t l = readVarint(p, end); p += l; }
                else break;
            }

            for (auto& g : groups) {
                const uint8_t *gp = g.first, *gend = gp + g.second;
                while (gp < gend) {
                    uint64_t gtag = readVarint(gp, gend);
                    uint32_t gfield = gtag >> 3, gwt = gtag & 7;
                    if (gfield == 3 && gwt == 2) {
                        uint64_t wlen = readVarint(gp, gend);
                        const uint8_t *wp = gp, *wend = gp + wlen;
                        gp += wlen;

                        uint64_t wayId = 0;
                        std::vector<uint32_t> keys, vals;
                        const uint8_t *refsPtr = nullptr, *refsEnd = nullptr;
                        while (wp < wend) {
                            uint64_t wtag = readVarint(wp, wend);
                            uint32_t wfield = wtag >> 3, wwt = wtag & 7;
                            if (wfield == 1 && wwt == 0) wayId = readVarint(wp, wend);
                            else if (wfield == 2 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend); const uint8_t* kend = wp + l;
                                while (wp < kend) keys.push_back((uint32_t)readVarint(wp, kend));
                            } else if (wfield == 3 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend); const uint8_t* vend = wp + l;
                                while (wp < vend) vals.push_back((uint32_t)readVarint(wp, vend));
                            } else if (wfield == 8 && wwt == 2) {
                                uint64_t l = readVarint(wp, wend); refsPtr = wp; refsEnd = wp + l; wp += l;
                            } else if (wwt == 0) readVarint(wp, wend);
                            else if (wwt == 2) { uint64_t l = readVarint(wp, wend); wp += l; }
                            else break;
                        }
                        if (!refsPtr) continue;

                        // Quick check: is the FIRST node in Stuttgart Metro?
                        const uint8_t* rpTmp = refsPtr;
                        uint64_t firstNid = (uint64_t)zigZag(readVarint(rpTmp, refsEnd));
                        int32_t lat0, lon0;
                        if (!lookupStgNode(firstNid, lat0, lon0)) continue;

                        int feat = -1;
                        bool rCar = false, rPed = false;
                        const std::string* namePtr = nullptr;
                        size_t nk = std::min(keys.size(), vals.size());
                        for (size_t i = 0; i < nk; i++) {
                            if (keys[i] >= st.size() || vals[i] >= st.size()) continue;
                            const std::string& k = st[keys[i]];
                            const std::string& v = st[vals[i]];
                            if (k == "highway") {
                                if (v == "residential" || v == "unclassified" || v == "living_street" || v == "service") {
                                    feat = FEAT_RESIDENTIAL; rCar = (v != "service"); rPed = true;
                                } else if (v == "footway" || v == "cycleway" || v == "path" || v == "pedestrian" || v == "track" || v == "steps") {
                                    feat = FEAT_PATH; rPed = true;
                                }
                            } else if (k == "building" && v != "no") {
                                if (feat < 0) {
                                    if (!flashMode || (wayId % 2 == 0)) feat = FEAT_BUILDING;
                                }
                            } else if (k == "landuse") {
                                if (feat < 0) {
                                    if (v == "forest") feat = FEAT_FOREST;
                                    else if (v == "farmland" || v == "meadow" || v == "orchard" || v == "vineyard") feat = FEAT_FARMLAND;
                                    else if (v == "grass" || v == "recreation_ground" || v == "village_green" || v == "allotments") feat = FEAT_PARK;
                                    else if (v == "residential" || v == "commercial" || v == "industrial" || v == "retail") feat = FEAT_URBAN;
                                    else if (v == "reservoir" || v == "basin") feat = FEAT_WATER_POLY;
                                }
                            } else if (k == "natural") {
                                if (feat < 0) {
                                    if (v == "wood") feat = FEAT_FOREST;
                                    else if (v == "water") feat = FEAT_WATER_POLY;
                                    else if (v == "grassland" || v == "scrub") feat = FEAT_PARK;
                                }
                            } else if (k == "leisure") {
                                if (feat < 0 && (v == "park" || v == "garden" || v == "nature_reserve" || v == "pitch")) feat = FEAT_PARK;
                            } else if (k == "waterway") {
                                if (feat < 0) {
                                    if (v == "riverbank") feat = FEAT_WATER_POLY;
                                    else if (v == "stream") feat = FEAT_WATER;
                                }
                            } else if (k == "railway" && v == "tram") {
                                if (feat < 0) feat = FEAT_RAIL;
                            } else if (k == "name") {
                                namePtr = &v;
                            }
                        }
                        if (feat < 0) continue;
                        if (flashMode && feat == FEAT_BUILDING && !inStuttgartBuildingsZone(lat0 * 1e-5, lon0 * 1e-5)) continue;

                        std::vector<std::pair<int32_t, int32_t>> coords;
                        int64_t curRef = 0;
                        while (refsPtr < refsEnd) {
                            curRef += zigZag(readVarint(refsPtr, refsEnd));
                            int32_t la, lo;
                            if (lookupStgNode((uint64_t)curRef, la, lo)) {
                                coords.push_back({la, lo});
                            }
                        }
                        if (coords.size() < 2) continue;

                        ResolvedWay rw;
                        rw.cls = (FeatureClass)feat;
                        rw.routableCar = rCar;
                        rw.routablePed = rPed;
                        if (namePtr && !namePtr->empty() && feat >= FEAT_RESIDENTIAL) {
                            rw.streetName = cleanText(*namePtr, 26);
                        }
                        if (feat == FEAT_BUILDING) {
                            size_t bstep = std::max<size_t>(1, (coords.size() - 1) / 5);
                            for (size_t r = 0; r + 1 < coords.size() && rw.coords.size() < 5; r += bstep) {
                                rw.coords.push_back(coords[r]);
                            }
                            rw.coords.push_back(coords.back());
                        } else if (feat <= FEAT_WATER_POLY) {
                            size_t pstep = std::max<size_t>(1, coords.size() / 12);
                            for (size_t r = 0; r + 1 < coords.size() && rw.coords.size() < 14; r += pstep) {
                                rw.coords.push_back(coords[r]);
                            }
                            rw.coords.push_back(coords.back());
                        } else {
                            rw.coords = std::move(coords);
                        }
                        outWays.push_back(std::move(rw));
                    } else if (gwt == 0) readVarint(gp, gend);
                    else if (gwt == 2) { uint64_t l = readVarint(gp, gend); gp += l; }
                    else break;
                }
            }
        }
        fclose(tf);
    };

    threads.clear();
    for (int t = 0; t < numThreads; t++) threads.emplace_back(wayStgWorker, t);
    for (auto& th : threads) th.join();

    size_t stgWayCount = 0;
    for (auto& tsw : threadStgWays) stgWayCount += tsw.size();
    printf("      Extracted %zu Stuttgart-Metro detail ways (buildings/forests/farmland/streets).\n", stgWayCount);

    // Free stgNodes now that Pass 3 is complete
    stgNodes.clear();
    stgNodes.shrink_to_fit();

    // Merge places & POIs
    std::vector<PlaceEntry> allPlaces;
    for (auto& tp : threadPlaces) for (auto& p : tp) allPlaces.push_back(std::move(p));
    std::sort(allPlaces.begin(), allPlaces.end(), [](const PlaceEntry& a, const PlaceEntry& b) {
        bool aStg = inStuttgartMetro(a.lat_e5 * 1e-5, a.lon_e5 * 1e-5);
        bool bStg = inStuttgartMetro(b.lat_e5 * 1e-5, b.lon_e5 * 1e-5);
        if (aStg != bStg && a.rank >= 3 && b.rank >= 3) return aStg > bStg;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.pop != b.pop) return a.pop > b.pop;
        return a.name < b.name;
    });
    size_t maxPlaces = flashMode ? 4500 : 25000;
    if (allPlaces.size() > maxPlaces) allPlaces.resize(maxPlaces);

    std::vector<PoiEntry> allPois;
    for (auto& tpo : threadPois) for (auto& po : tpo) allPois.push_back(std::move(po));

    // -------------------------------------------------------------------------
    // STEP 5: Build Spatial Vector Tiles & Routing Graph
    // -------------------------------------------------------------------------
    printf("[5/6] Building 3-tier spatial vector tiles, street labels & routing graph...\n");

    struct PolySeg {
        uint8_t cls;
        std::vector<std::pair<uint8_t, uint8_t>> pts;
    };
    std::vector<std::vector<PolySeg>> coarseCells(COARSE_NX * COARSE_NY);
    std::vector<std::vector<PolySeg>> fineCells(FINE_NX * FINE_NY);
    std::vector<std::vector<PolySeg>> ultraCells(ULTRA_NX * ULTRA_NY);

    struct RouteEdgeTmp {
        uint32_t to;
        uint16_t dist_hm;
        uint8_t flags;
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
        lat5 = nodeLat[idx];
        lon5 = nodeLon[idx];
        return (lat5 != 0 || lon5 != 0);
    };

    const double latSpan = MAX_LAT - MIN_LAT;
    const double lonSpan = MAX_LON - MIN_LON;
    const double stgLatSpan = STG_MAX_LAT - STG_MIN_LAT;
    const double stgLonSpan = STG_MAX_LON - STG_MIN_LON;
    std::unordered_set<std::string> seenStreetCell;

    auto processWayCoords = [&](FeatureClass cls, bool rCar, bool rPed, const std::string& streetName,
                                const std::vector<std::pair<int32_t, int32_t>>& coords) {
        if (coords.size() < 2) return;
        bool anyInStg = inStuttgartMetro(coords[0].first * 1e-5, coords[0].second * 1e-5) ||
                        inStuttgartMetro(coords.back().first * 1e-5, coords.back().second * 1e-5);

        if (anyInStg && !streetName.empty() && cls >= FEAT_RESIDENTIAL) {
            auto& mid = coords[coords.size() / 2];
            int ucy = (int)((mid.first * 1e-5 - STG_MIN_LAT) / stgLatSpan * 64.0);
            int ucx = (int)((mid.second * 1e-5 - STG_MIN_LON) / stgLonSpan * 64.0);
            std::string skey = streetName + "#" + std::to_string(ucy * 64 + ucx);
            if (seenStreetCell.insert(skey).second) {
                allPois.push_back({mid.first, mid.second, POI_STREET, streetName});
            }
        }

        if ((rCar || rPed) && (cls >= FEAT_TERTIARY || (anyInStg && cls == FEAT_RESIDENTIAL))) {
            uint8_t roadRank = (cls >= FEAT_MOTORWAY) ? 4 :
                               (cls == FEAT_PRIMARY) ? 3 :
                               (cls == FEAT_SECONDARY) ? 2 :
                               (cls == FEAT_TERTIARY) ? 1 : 0;
            uint8_t flags = (rCar ? 1 : 0) | (rPed ? 2 : 0) | (roadRank << 2);
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
                    double dlat = (routeNodes[prevU].lat_e5 - routeNodes[u].lat_e5) * 1e-5 * 111.32;
                    double dlon = (routeNodes[prevU].lon_e5 - routeNodes[u].lon_e5) * 1e-5 * 71.5;
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
            int curCell = -1;
            PolySeg seg{ (uint8_t)cls, {} };
            for (auto& pt : coords) {
                double ny = (pt.first * 1e-5 - MIN_LAT) / latSpan * COARSE_NY;
                double nx = (pt.second * 1e-5 - MIN_LON) / lonSpan * COARSE_NX;
                int cy = std::clamp((int)ny, 0, COARSE_NY - 1);
                int cx = std::clamp((int)nx, 0, COARSE_NX - 1);
                int cell = cy * COARSE_NX + cx;
                uint8_t qx = (uint8_t)std::clamp((int)((nx - cx) * 255.0), 0, 255);
                uint8_t qy = (uint8_t)std::clamp((int)((ny - cy) * 255.0), 0, 255);
                if (cell != curCell) {
                    if (curCell >= 0 && seg.pts.size() >= 2 && coarseCells[curCell].size() < 350) coarseCells[curCell].push_back(seg);
                    seg.pts.clear();
                    curCell = cell;
                }
                if (seg.pts.empty() || seg.pts.back() != std::make_pair(qx, qy)) seg.pts.push_back({qx, qy});
            }
            if (curCell >= 0 && seg.pts.size() >= 2 && coarseCells[curCell].size() < 350) coarseCells[curCell].push_back(seg);
        }

        if (cls >= FEAT_WATER && cls != FEAT_PATH && cls != FEAT_RESIDENTIAL && !(flashMode && cls == FEAT_TERTIARY)) {
            size_t maxSegs = flashMode ? 5 : 70;
            int curCell = -1;
            PolySeg seg{ (uint8_t)cls, {} };
            for (auto& pt : coords) {
                double ny = (pt.first * 1e-5 - MIN_LAT) / latSpan * FINE_NY;
                double nx = (pt.second * 1e-5 - MIN_LON) / lonSpan * FINE_NX;
                int cy = std::clamp((int)ny, 0, FINE_NY - 1);
                int cx = std::clamp((int)nx, 0, FINE_NX - 1);
                int cell = cy * FINE_NX + cx;
                uint8_t qx = (uint8_t)std::clamp((int)((nx - cx) * 255.0), 0, 255);
                uint8_t qy = (uint8_t)std::clamp((int)((ny - cy) * 255.0), 0, 255);
                if (cell != curCell) {
                    if (curCell >= 0 && seg.pts.size() >= 2 && fineCells[curCell].size() < maxSegs) fineCells[curCell].push_back(seg);
                    seg.pts.clear();
                    curCell = cell;
                }
                if (seg.pts.empty() || seg.pts.back() != std::make_pair(qx, qy)) seg.pts.push_back({qx, qy});
            }
            if (curCell >= 0 && seg.pts.size() >= 2 && fineCells[curCell].size() < maxSegs) fineCells[curCell].push_back(seg);
        }

        if (anyInStg) {
            size_t maxUltraCollect = 350;
            int curCell = -1;
            PolySeg seg{ (uint8_t)cls, {} };
            for (auto& pt : coords) {
                double dlat = pt.first * 1e-5, dlon = pt.second * 1e-5;
                if (!inStuttgartMetro(dlat, dlon)) continue;
                double ny = (dlat - STG_MIN_LAT) / stgLatSpan * ULTRA_NY;
                double nx = (dlon - STG_MIN_LON) / stgLonSpan * ULTRA_NX;
                int cy = std::clamp((int)ny, 0, ULTRA_NY - 1);
                int cx = std::clamp((int)nx, 0, ULTRA_NX - 1);
                int cell = cy * ULTRA_NX + cx;
                uint8_t qx = (uint8_t)std::clamp((int)((nx - cx) * 255.0), 0, 255);
                uint8_t qy = (uint8_t)std::clamp((int)((ny - cy) * 255.0), 0, 255);
                if (cell != curCell) {
                    if (curCell >= 0 && seg.pts.size() >= 2 && ultraCells[curCell].size() < maxUltraCollect) ultraCells[curCell].push_back(seg);
                    seg.pts.clear();
                    curCell = cell;
                }
                if (seg.pts.empty() || seg.pts.back() != std::make_pair(qx, qy)) seg.pts.push_back({qx, qy});
            }
            if (curCell >= 0 && seg.pts.size() >= 2 && ultraCells[curCell].size() < maxUltraCollect) ultraCells[curCell].push_back(seg);
        }
    };

    // Process nationwide ways first (so major roads always fit in cells)
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
            processWayCoords(w.cls, w.routableCar, w.routablePed, w.streetName, coords);
        }
    }
    // Process Stuttgart detail ways
    for (auto& tsw : threadStgWays) {
        for (auto& rw : tsw) {
            processWayCoords(rw.cls, rw.routableCar, rw.routablePed, rw.streetName, rw.coords);
        }
    }

    size_t maxUltraFinal = flashMode ? 17 : 220;
    for (auto& cellSegs : ultraCells) {
        if (cellSegs.size() > maxUltraFinal) {
            // Prioritize streets, water, rail, forests/farmland/parks first, then buildings
            std::stable_sort(cellSegs.begin(), cellSegs.end(), [](const PolySeg& a, const PolySeg& b) {
                bool aBld = (a.cls == FEAT_BUILDING);
                bool bBld = (b.cls == FEAT_BUILDING);
                if (aBld != bBld) return !aBld; // non-buildings first
                return a.cls > b.cls;           // higher road classes first
            });
            cellSegs.resize(maxUltraFinal);
        }
        // Final render order: background landuse (0..4), buildings (5), water/rail/paths/streets (6..14)
        std::stable_sort(cellSegs.begin(), cellSegs.end(), [](const PolySeg& a, const PolySeg& b) {
            return a.cls < b.cls;
        });
    }

    if (flashMode && allPois.size() > 10000) {
        std::sort(allPois.begin(), allPois.end(), [](const PoiEntry& a, const PoiEntry& b) {
            if ((a.category == POI_STREET) != (b.category == POI_STREET)) return a.category != POI_STREET;
            return a.category < b.category;
        });
        allPois.resize(10000);
    }
    std::sort(allPois.begin(), allPois.end(), [](const PoiEntry& a, const PoiEntry& b) {
        return a.lat_e5 < b.lat_e5;
    });

    size_t totalEdges = 0;
    for (auto& rn : routeNodes) totalEdges += rn.edges.size();
    printf("      Routing Graph: %zu nodes, %zu directed edges.\n", routeNodes.size(), totalEdges);

    printf("[6/6] Writing %s...\n", outPath);
    FILE* out = fopen(outPath, "wb");
    if (!out) { perror("fopen out"); return 1; }

    struct KnavHeader {
        char magic[8];            // "KNAVDE02"
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
    } hdr{};
    memcpy(hdr.magic, "KNAVDE02", 8);
    fwrite(&hdr, sizeof(hdr), 1, out);

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

    struct CellDirEntry { uint32_t offset; uint32_t length; };
    auto writeGrid = [&](const std::vector<std::vector<PolySeg>>& cells, std::vector<CellDirEntry>& dir, uint32_t& dirOff) {
        dir.assign(cells.size(), {0, 0});
        dirOff = (uint32_t)ftell(out);
        fwrite(dir.data(), sizeof(CellDirEntry), dir.size(), out);
        for (size_t i = 0; i < cells.size(); i++) {
            if (cells[i].empty()) continue;
            dir[i].offset = (uint32_t)ftell(out);
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
            dir[i].length = (uint32_t)ftell(out) - dir[i].offset;
        }
    };

    std::vector<CellDirEntry> coarseDir, fineDir, ultraDir;
    writeGrid(coarseCells, coarseDir, hdr.coarseDirOffset);
    writeGrid(fineCells, fineDir, hdr.fineDirOffset);
    writeGrid(ultraCells, ultraDir, hdr.ultraDirOffset);

    struct DiskPoi {
        int32_t lat_e5;
        int32_t lon_e5;
        uint8_t category;
        char name[27];
    };
    hdr.numPois = (uint32_t)allPois.size();
    hdr.poisOffset = (uint32_t)ftell(out);
    for (auto& po : allPois) {
        DiskPoi dpo{};
        dpo.lat_e5 = po.lat_e5;
        dpo.lon_e5 = po.lon_e5;
        dpo.category = po.category;
        strncpy(dpo.name, po.name.c_str(), sizeof(dpo.name) - 1);
        fwrite(&dpo, sizeof(dpo), 1, out);
    }

    uint32_t totalBytes = (uint32_t)ftell(out);
    fseek(out, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, out);
    fseek(out, hdr.coarseDirOffset, SEEK_SET);
    fwrite(coarseDir.data(), sizeof(CellDirEntry), coarseDir.size(), out);
    fseek(out, hdr.fineDirOffset, SEEK_SET);
    fwrite(fineDir.data(), sizeof(CellDirEntry), fineDir.size(), out);
    fseek(out, hdr.ultraDirOffset, SEEK_SET);
    fwrite(ultraDir.data(), sizeof(CellDirEntry), ultraDir.size(), out);
    fclose(out);

    printf("DONE! Generated %s (%.2f MB, %u places, %u route nodes, %u POIs/streets)\n",
           outPath, totalBytes / 1048576.0, hdr.numPlaces, hdr.numRouteNodes, hdr.numPois);
    return 0;
}
