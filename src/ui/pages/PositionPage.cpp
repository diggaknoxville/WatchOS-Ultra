// Germany & Stuttgart Street-Level Offline Navigation + GNSS Skyplot + LoRa/Meshtastic Hybrid Positioning
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "GpsManager.h"
#include "NavEngine.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace ui;

namespace pages {

static constexpr int MAP_W = 374;
static constexpr int MAP_H = 236;

static lv_obj_t *sRoot = nullptr;
static lv_obj_t *sModePill = nullptr, *sModeTxt = nullptr;
static lv_obj_t *sGpsPill = nullptr, *sGpsTxt = nullptr;

static lv_obj_t *sMapCard = nullptr;
static lv_obj_t *sMapImg = nullptr;
static lv_image_dsc_t sMapDsc{};
static uint16_t *sMapBuf = nullptr;

static lv_obj_t *sZoomLbl = nullptr;
static lv_obj_t *sGpxBtn = nullptr, *sGpxTxt = nullptr;
static lv_obj_t *sRecenterPill = nullptr;

// Bottom Guidance / Telemetry Card
static lv_obj_t *sNavBox = nullptr;
static lv_obj_t *sTurnIcon = nullptr;
static lv_obj_t *sTurnCmd = nullptr;
static lv_obj_t *sTurnDest = nullptr;
static lv_obj_t *sTurnMeta = nullptr;
static lv_obj_t *sFreeBox = nullptr;
static lv_obj_t *sCoordLbl = nullptr;
static lv_obj_t *sMetaLbl = nullptr;
static lv_obj_t *sSdLbl = nullptr;

// Place / Route Search Overlay
static lv_obj_t *sSearchModal = nullptr;
static lv_obj_t *sSearchList = nullptr;
static lv_obj_t *sFilterBtns[7] = {nullptr};
static int sActiveFilter = 0;
static NavPlace sSearchResults[24];
static int sNumResults = 0;

// GNSS Satellite Skyplot Modal
static lv_obj_t *sSkyModal = nullptr;
static lv_obj_t *sSkySub = nullptr;
static lv_obj_t *sSkyRing = nullptr;
static lv_obj_t *sSatDots[16] = {nullptr};
static lv_obj_t *sSatSnrBars[8] = {nullptr};
static lv_obj_t *sSatSnrLbls[8] = {nullptr};

static int sZoom = 15;             // Default 15 = Street & Building level when standing
static bool sAutoZoom = true;      // Speed-adaptive auto-zoom enabled by default
static bool sManualPan = false;
static unsigned long sManualInteractMs = 0;
static double sViewLat = 48.7758;  // Stuttgart Schlossplatz
static double sViewLon = 9.1829;
static double sLastRenderedLat = 0;
static double sLastRenderedLon = 0;
static int sLastRenderedZoom = -1;
static bool sMapNeedsRedraw = true;

static const char* FILTER_LABELS[7] = {"STGT", "MESH", "A-D", "E-K", "L-M", "N-S", "T-Z"};

static int computeSpeedAdaptiveZoom(float speedKmh) {
    if (speedKmh <= 6.5f)  return 15; // Standing / walking: immediate surroundings (~200m, buildings, paths, POIs)
    if (speedKmh <= 22.0f) return 14; // Cycling / slow city (~400m)
    if (speedKmh <= 55.0f) return 13; // City driving / Landstraße (~800m)
    if (speedKmh <= 90.0f) return 11; // Bundesstraße (~3 km)
    return 10;                        // Autobahn (> 90 km/h, ~6 km)
}

static void redrawCanvas() {
    if (!sMapBuf || !sMapImg) return;
    const NavStatus& st = NavEngine::status();

    // Auto-recenter after 15s of manual pan/zoom inactivity
    if ((sManualPan || !sAutoZoom) && (millis() - sManualInteractMs > 15000)) {
        sManualPan = false;
        sAutoZoom = true;
    }

    if (sAutoZoom) {
        float spd = GpsManager::speedKmh();
        sZoom = computeSpeedAdaptiveZoom(spd);
    }

    double lat = sManualPan ? sViewLat : st.curLat;
    double lon = sManualPan ? sViewLon : st.curLon;
    sViewLat = lat;
    sViewLon = lon;

    NavEngine::renderMap(sMapBuf, MAP_W, MAP_H, lat, lon, sZoom);
    lv_obj_invalidate(sMapImg);
    sLastRenderedLat = lat;
    sLastRenderedLon = lon;
    sLastRenderedZoom = sZoom;
    sMapNeedsRedraw = false;
}

static void refreshSkyplot() {
    if (!sSkyModal || lv_obj_has_flag(sSkyModal, LV_OBJ_FLAG_HIDDEN)) return;

    GpsSatInfo sats[16];
    uint8_t n = GpsManager::getSatellitesInView(sats, 16);

    char sb[72];
    snprintf(sb, sizeof(sb), "GNSS: %u Sats in Sicht · Fix: %u Sats · HDOP %.1f",
             (unsigned)n, (unsigned)GpsManager::satellites(),
             GpsManager::hdop() < 50.0f ? GpsManager::hdop() : 0.0f);
    setText(sSkySub, sb);

    // Skyplot radar center inside sSkyRing (200x200, radius 90)
    const int cx = 100, cy = 100, maxR = 84;
    for (int i = 0; i < 16; i++) {
        if (!sSatDots[i]) continue;
        if (i < n) {
            setHidden(sSatDots[i], false);
            float el = constrain((float)sats[i].elevation, 0.0f, 90.0f);
            float azRad = (float)sats[i].azimuth * (float)M_PI / 180.0f;
            float r = (1.0f - (el / 90.0f)) * maxR;
            int sx = cx + (int)(sinf(azRad) * r) - 11;
            int sy = cy - (int)(cosf(azRad) * r) - 11;
            lv_obj_set_pos(sSatDots[i], sx, sy);
            lv_color_t col = (sats[i].snr >= 25) ? cOk() :
                             (sats[i].snr >= 15) ? accent() : cWarn();
            setBgColorIf(sSatDots[i], col);
            char prnTxt[8];
            snprintf(prnTxt, sizeof(prnTxt), "%u", sats[i].prn);
            setText(lv_obj_get_child(sSatDots[i], 0), prnTxt);
        } else {
            setHidden(sSatDots[i], true);
        }
    }

    for (int i = 0; i < 8; i++) {
        if (!sSatSnrBars[i] || !sSatSnrLbls[i]) continue;
        if (i < n) {
            int hBar = constrain((int)sats[i].snr * 2, 6, 96);
            lv_obj_set_height(sSatSnrBars[i], hBar);
            lv_obj_set_y(sSatSnrBars[i], 106 - hBar);
            lv_color_t col = (sats[i].snr >= 25) ? cOk() :
                             (sats[i].snr >= 15) ? accent() : cWarn();
            setBgColorIf(sSatSnrBars[i], col);
            char lb[12];
            snprintf(lb, sizeof(lb), "G%02u\n%udB", sats[i].prn, sats[i].snr);
            setText(sSatSnrLbls[i], lb);
        } else {
            lv_obj_set_height(sSatSnrBars[i], 4);
            lv_obj_set_y(sSatSnrBars[i], 102);
            setBgColorIf(sSatSnrBars[i], cSurf2());
            setText(sSatSnrLbls[i], "--");
        }
    }
}

static void populateSearchList() {
    if (!sSearchList) return;
    lv_obj_clean(sSearchList);

    if (sActiveFilter == 1) {
        // Real Meshtastic peers with coordinates + saved POIs
        sNumResults = NavEngine::searchPlaces("@mesh", sSearchResults, 20);
    } else if (sActiveFilter == 0) {
        sNumResults = NavEngine::searchPlaces("", sSearchResults, 20);
    } else {
        static const char ranges[][2] = {{0,0}, {0,0}, {'A','D'}, {'E','K'}, {'L','M'}, {'N','S'}, {'T','Z'}};
        char cMin = ranges[sActiveFilter][0];
        char cMax = ranges[sActiveFilter][1];
        NavPlace pool[64];
        int nPool = NavEngine::searchPlaces("", pool, 64);
        sNumResults = 0;
        for (int i = 0; i < nPool && sNumResults < 24; i++) {
            char c = pool[i].name[0];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c >= cMin && c <= cMax) {
                sSearchResults[sNumResults++] = pool[i];
            }
        }
    }

    for (int i = 0; i < 7; i++) {
        if (!sFilterBtns[i]) continue;
        setBgColorIf(sFilterBtns[i], (i == sActiveFilter) ? accent() : cSurf2());
    }

    if (sNumResults == 0 && sActiveFilter == 1) {
        lv_obj_t* empty = card(sSearchList, CW - 16, 68);
        lv_obj_t* l1 = label(empty, F.small, cText(), "Noch keine Meshtastic-Position empfangen");
        lv_obj_set_pos(l1, 14, 12);
        lv_obj_t* l2 = caption(empty, "Sende Discovery-Ping auf LORA MESH Seite...", cText2());
        lv_obj_set_pos(l2, 14, 38);
        RadioLoRa::sendNodeInfoDiscovery();
        return;
    }

    for (int i = 0; i < sNumResults; i++) {
        lv_obj_t* row = card(sSearchList, CW - 16, 48);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (idx >= 0 && idx < sNumResults) {
                const NavPlace& pl = sSearchResults[idx];
                if (pl.rank == 0) {
                    NavEngine::setMode(NAV_MODE_MESH);
                } else if (NavEngine::getMode() == NAV_MODE_MESH) {
                    NavEngine::setMode(NAV_MODE_CAR);
                }
                NavEngine::startNavigation(pl.name, pl.lat, pl.lon);
                sManualPan = false;
                sAutoZoom = true;
                sMapNeedsRedraw = true;
                setHidden(sSearchModal, true);
                haptic(HAPTIC_SUCCESS);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        bool isMesh = (sSearchResults[i].rank == 0);
        bool isPoi  = (sSearchResults[i].rank == 5);
        lv_color_t icCol = isMesh ? lv_color_hex(0xBF5AF2) : isPoi ? cWarn() : accent();
        lv_obj_t* ic = label(row, F.icoS, icCol, isMesh ? ICO_PODCASTS : isPoi ? ICO_FLAG : ICO_PLACE);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t* nm = label(row, F.small, cText(), sSearchResults[i].name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 42, 0);

        char distBuf[24];
        const NavStatus& st = NavEngine::status();
        double dlat = (sSearchResults[i].lat - st.curLat) * 111.32;
        double dlon = (sSearchResults[i].lon - st.curLon) * 71.5;
        double km = sqrt(dlat * dlat + dlon * dlon);
        if (isMesh) snprintf(distBuf, sizeof(distBuf), "MESH  %.1f km", km);
        else if (km < 10.0) snprintf(distBuf, sizeof(distBuf), "%.1f km", km);
        else snprintf(distBuf, sizeof(distBuf), "%.0f km", km);
        lv_obj_t* dl = caption(row, distBuf, isMesh ? lv_color_hex(0xBF5AF2) : cText2());
        lv_obj_align(dl, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static void refresh(bool force) {
    const NavStatus& st = NavEngine::status();

    // 1. Mode Pill (AUTO / FUSS / MESH)
    const char* mStr = (st.mode == NAV_MODE_CAR)  ? "AUTO" :
                       (st.mode == NAV_MODE_PED)  ? "FUSS" : "MESH";
    lv_color_t mCol  = (st.mode == NAV_MODE_CAR)  ? accent() :
                       (st.mode == NAV_MODE_PED)  ? cOk() : lv_color_hex(0xBF5AF2);
    setBgColorIf(sModePill, mCol);
    setTextColorIf(sModeTxt, mCol);
    setText(sModeTxt, mStr);

    // 2. GPS / Position Source Pill
    bool en = GpsManager::isEnabled();
    bool fix = GpsManager::hasFix();
    lv_color_t pc = !en ? cCrit() : fix ? cOk() :
                    (st.posSource == POS_SRC_LORA_MESH) ? lv_color_hex(0xBF5AF2) : cWarn();
    setBgColorIf(sGpsPill, pc);
    setTextColorIf(sGpsTxt, pc);
    setText(sGpsTxt, !en ? "GPS AUS" : fix ? "3D FIX" :
                     (st.posSource == POS_SRC_LORA_MESH) ? "LORA POS" : "SUCHE...");

    // Check if speed or position changed enough to trigger auto-zoom / auto-center redraw
    int targetZoom = sAutoZoom ? computeSpeedAdaptiveZoom(GpsManager::speedKmh()) : sZoom;
    if (targetZoom != sLastRenderedZoom ||
        (!sManualPan && (fabs(st.curLat - sLastRenderedLat) > 0.00008 ||
                         fabs(st.curLon - sLastRenderedLon) > 0.00008))) {
        sZoom = targetZoom;
        sMapNeedsRedraw = true;
    }

    // 3. Zoom & GPX status badges
    char zb[16];
    snprintf(zb, sizeof(zb), "%sZ%d", sAutoZoom ? "A·" : "", sZoom);
    setText(sZoomLbl, zb);
    setBgColorIf(sGpxBtn, st.gpxRecording ? cWarn() : cSurf2());
    setText(sGpxTxt, st.gpxRecording ? "REC" : "+POI");
    if (sRecenterPill) {
        setHidden(sRecenterPill, !sManualPan && sAutoZoom);
    }

    // 4. Bottom Guidance vs Free Telemetry
    setHidden(sNavBox, !st.activeRoute);
    setHidden(sFreeBox, st.activeRoute);

    if (st.activeRoute) {
        setText(sTurnIcon, st.nextTurnIcon ? st.nextTurnIcon : ICO_NAVIGATION);
        char cmdBuf[44];
        if (st.nextTurnDistM >= 1000) {
            snprintf(cmdBuf, sizeof(cmdBuf), "%s (%.1f km)", st.nextTurnCmd, st.nextTurnDistM / 1000.0f);
        } else {
            snprintf(cmdBuf, sizeof(cmdBuf), "%s (%lu m)", st.nextTurnCmd, (unsigned long)st.nextTurnDistM);
        }
        setText(sTurnCmd, cmdBuf);

        char destBuf[52];
        snprintf(destBuf, sizeof(destBuf), "Ziel: %s (%s)", st.destName, mStr);
        setText(sTurnDest, destBuf);

        char metaBuf[64];
        if (st.etaMinutes >= 60) {
            snprintf(metaBuf, sizeof(metaBuf), "Rest %.1f km  ·  ETA %uh %02um  ·  %.0f°",
                     st.remDistKm, st.etaMinutes / 60, st.etaMinutes % 60, st.bearingDeg);
        } else {
            snprintf(metaBuf, sizeof(metaBuf), "Rest %.1f km  ·  ETA %u min  ·  %.0f°",
                     st.remDistKm, st.etaMinutes, st.bearingDeg);
        }
        setText(sTurnMeta, metaBuf);
    } else {
        char cb[68];
        snprintf(cb, sizeof(cb), "%.4f° N  %.4f° E  [%s]",
                 fabs(st.curLat), fabs(st.curLon), st.posSourceTag);
        setText(sCoordLbl, cb);

        char mb[68];
        snprintf(mb, sizeof(mb), "SAT %u  ·  %.0f m  ·  %.1f km/h  ·  Auto-Zoom Z%d",
                 GpsManager::satellites(), GpsManager::altitudeM(),
                 GpsManager::speedKmh(), sZoom);
        setText(sMetaLbl, mb);

        char sb[72];
        if (st.ultraLoaded) {
            snprintf(sb, sizeof(sb), "STUTTGART HD (Gebäude/Wald/POI: %lu) · SD %s",
                     (unsigned long)st.mapPois,
                     st.sdWritable ? "R/W OK" : (st.sdReady ? "RO+FLASH" : "FLASH"));
        } else if (st.mapLoaded) {
            snprintf(sb, sizeof(sb), "DE-KARTE (%lu Orte / %lu Knoten)",
                     (unsigned long)st.mapPlaces, (unsigned long)st.mapRouteNodes);
        } else {
            snprintf(sb, sizeof(sb), "DE-Basiskarte aktiv");
        }
        setText(sSdLbl, sb);
    }

    if (force || sMapNeedsRedraw) {
        redrawCanvas();
    }
    refreshSkyplot();
}

void buildPosition(lv_obj_t* scr) {
    NavEngine::init();

    lv_obj_t* p = pageRoot(scr);
    sRoot = p;
    title(p, "NAVI · STGT");

    // Mode Toggle Pill (AUTO / FUSS / MESH)
    sModePill = box(p, 76, 30);
    lv_obj_align(sModePill, LV_ALIGN_TOP_RIGHT, -(M + 104), 26);
    lv_obj_set_style_radius(sModePill, 15, 0);
    lv_obj_set_style_bg_opa(sModePill, LV_OPA_30, 0);
    lv_obj_add_flag(sModePill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sModePill, [](lv_event_t*) {
        NavMode next = (NavMode)(((int)NavEngine::getMode() + 1) % 3);
        NavEngine::setMode(next);
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    sModeTxt = caption(sModePill, "AUTO", accent());
    lv_obj_center(sModeTxt);

    // GPS Toggle Pill
    sGpsPill = box(p, 96, 30);
    lv_obj_align(sGpsPill, LV_ALIGN_TOP_RIGHT, -M, 26);
    lv_obj_set_style_radius(sGpsPill, 15, 0);
    lv_obj_set_style_bg_opa(sGpsPill, LV_OPA_30, 0);
    lv_obj_add_flag(sGpsPill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sGpsPill, [](lv_event_t*) {
        GpsManager::setEnabled(!GpsManager::isEnabled());
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    sGpsTxt = caption(sGpsPill, "", cText2());
    lv_obj_center(sGpsTxt);

    // Map Viewport Card (374 x 236)
    sMapCard = card(p, MAP_W, MAP_H);
    lv_obj_set_pos(sMapCard, M, 76);
    lv_obj_set_style_pad_all(sMapCard, 0, 0);
    lv_obj_set_style_radius(sMapCard, 18, 0);
    lv_obj_set_style_clip_corner(sMapCard, true, 0);

    if (!sMapBuf) {
        sMapBuf = (uint16_t*)heap_caps_malloc(MAP_W * MAP_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    sMapDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    sMapDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    sMapDsc.header.flags = 0;
    sMapDsc.header.w = MAP_W;
    sMapDsc.header.h = MAP_H;
    sMapDsc.header.stride = MAP_W * 2;
    sMapDsc.data_size = MAP_W * MAP_H * 2;
    sMapDsc.data = (const uint8_t*)sMapBuf;

    sMapImg = lv_image_create(sMapCard);
    lv_image_set_src(sMapImg, &sMapDsc);
    lv_obj_set_pos(sMapImg, 0, 0);

    // Smooth 2D Drag-and-Drop Map Panning + Tap to Center
    static lv_point_t sDragStart = {0, 0};
    static double sDragStartLat = 0, sDragStartLon = 0;
    static bool sIsDragging = false;

    lv_obj_add_flag(sMapCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sMapCard, [](lv_event_t* e) {
        lv_event_code_t code = lv_event_get_code(e);
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);

        if (code == LV_EVENT_PRESSED) {
            sDragStart = pt;
            sDragStartLat = sViewLat;
            sDragStartLon = sViewLon;
            sIsDragging = false;
        } else if (code == LV_EVENT_PRESSING) {
            int dx = pt.x - sDragStart.x;
            int dy = pt.y - sDragStart.y;
            if (abs(dx) > 4 || abs(dy) > 4) {
                sIsDragging = true;
                sManualPan = true;
                sManualInteractMs = millis();
                double degPerPixX = 360.0 / (256.0 * pow(2.0, (double)sZoom));
                double degPerPixY = degPerPixX * cos(sViewLat * M_PI / 180.0);
                sViewLon = sDragStartLon - (double)dx * degPerPixX;
                sViewLat = sDragStartLat + (double)dy * degPerPixY;
                sMapNeedsRedraw = true;
                redrawCanvas();
            }
        } else if (code == LV_EVENT_RELEASED) {
            if (!sIsDragging) {
                // Short tap on map center toggles auto-center & auto-zoom
                int rx = pt.x - M;
                int ry = pt.y - 76;
                if (abs(rx - MAP_W / 2) < 60 && abs(ry - MAP_H / 2) < 60) {
                    sManualPan = false;
                    sAutoZoom = true;
                    haptic(HAPTIC_CLICK);
                    sMapNeedsRedraw = true;
                    refresh(true);
                }
            }
            sIsDragging = false;
        }
    }, LV_EVENT_ALL, NULL);

    auto makeMapBtn = [&](int x, int y, int w, int h, const char* txt, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* b = box(sMapCard, w, h);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, cSurf2(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, accent(), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t* l = label(b, F.small, cText(), txt);
        lv_obj_center(l);
        return b;
    };

    makeMapBtn(MAP_W - 48, 10, 38, 38, "+", [](lv_event_t*) {
        if (sZoom < 16) sZoom++;
        sAutoZoom = false;
        sManualInteractMs = millis();
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    });
    makeMapBtn(MAP_W - 48, 54, 38, 38, "-", [](lv_event_t*) {
        if (sZoom > 6) sZoom--;
        sAutoZoom = false;
        sManualInteractMs = millis();
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    });

    lv_obj_t* zBadge = box(sMapCard, 48, 26);
    lv_obj_set_pos(zBadge, MAP_W - 54, 96);
    lv_obj_set_style_radius(zBadge, 8, 0);
    lv_obj_set_style_bg_color(zBadge, cBg(), 0);
    lv_obj_set_style_bg_opa(zBadge, LV_OPA_80, 0);
    lv_obj_add_flag(zBadge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zBadge, [](lv_event_t*) {
        sAutoZoom = true;
        sManualPan = false;
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    sZoomLbl = caption(zBadge, "A·Z15", accent());
    lv_obj_center(sZoomLbl);

    // 1-Click POI Save & GPX Toggle button
    sGpxBtn = makeMapBtn(10, 10, 62, 34, "+POI", [](lv_event_t*) {
        NavEngine::markWaypoint("POI");
        bool rec = !NavEngine::isTrackRecording();
        NavEngine::setTrackRecording(rec);
        sMapNeedsRedraw = true;
        haptic(HAPTIC_SUCCESS);
        DisplayEngine::showNotification("POI & Track gespeichert",
                                        rec ? "1-Klick POI markiert & GPX aktiv" : "1-Klick POI gespeichert",
                                        2000, ICO_PLACE);
        refresh(true);
    });
    sGpxTxt = lv_obj_get_child(sGpxBtn, 0);

    // GNSS Skyplot Button
    makeMapBtn(78, 10, 56, 34, "SAT", [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        setHidden(sSkyModal, false);
        refreshSkyplot();
    });

    // Recenter Pill (visible when user manually panned or zoomed)
    sRecenterPill = makeMapBtn(140, 10, 96, 34, "ZENTRIEREN", [](lv_event_t*) {
        sManualPan = false;
        sAutoZoom = true;
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    });
    setHidden(sRecenterPill, true);

    makeMapBtn(MAP_W - 88, MAP_H - 44, 78, 34, "ZIEL", [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        populateSearchList();
        setHidden(sSearchModal, false);
    });

    // =========================================================================
    // Bottom Dock: Active Turn-by-Turn Box vs Free Telemetry Box
    // =========================================================================
    sNavBox = card(p, CW, 166);
    lv_obj_set_pos(sNavBox, M, 320);

    sTurnIcon = label(sNavBox, F.icoL, accent(), ICO_NAVIGATION);
    accReg(sTurnIcon, ACC_TEXT);
    lv_obj_set_pos(sTurnIcon, 16, 14);

    sTurnCmd = label(sNavBox, F.body, cText(), "GERADEAUS");
    lv_obj_set_pos(sTurnCmd, 62, 12);

    sTurnDest = label(sNavBox, F.small, accent(), "Ziel: Stuttgart");
    accReg(sTurnDest, ACC_TEXT);
    lv_obj_set_pos(sTurnDest, 62, 42);

    sTurnMeta = caption(sNavBox, "Rest 0 km · ETA 0m", cText2());
    lv_obj_set_pos(sTurnMeta, 18, 76);

    auto makeNavAction = [&](int x, int w, const char* txt, lv_color_t col, lv_event_cb_t cb) {
        lv_obj_t* b = box(sNavBox, w, 44);
        lv_obj_set_pos(b, x, 108);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_color(b, cSurf2(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, col, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t* l = label(b, F.small, col, txt);
        lv_obj_center(l);
    };

    makeNavAction(14, 110, "ZIELWAHL", accent(), [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        populateSearchList();
        setHidden(sSearchModal, false);
    });
    makeNavAction(132, 126, "LORA SEND", lv_color_hex(0xBF5AF2), [](lv_event_t*) {
        const NavStatus& st = NavEngine::status();
        RadioLoRa::sendWaypoint(st.destName, st.destLat, st.destLon);
        haptic(HAPTIC_SUCCESS);
        DisplayEngine::showNotification("LoRa / Meshtastic", "Ziel-Wegpunkt ans Mesh gesendet", 2500, ICO_PODCASTS);
    });
    makeNavAction(266, 94, "STOP", cCrit(), [](lv_event_t*) {
        NavEngine::stopNavigation();
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    });

    // Free Telemetry Card (when not actively navigating)
    sFreeBox = card(p, CW, 166);
    lv_obj_set_pos(sFreeBox, M, 320);

    sCoordLbl = label(sFreeBox, F.small, cText(), "");
    lv_obj_set_pos(sCoordLbl, 16, 12);

    sMetaLbl = caption(sFreeBox, "", cText2());
    lv_obj_set_pos(sMetaLbl, 16, 42);

    sSdLbl = caption(sFreeBox, "", accent());
    accReg(sSdLbl, ACC_TEXT);
    lv_obj_set_pos(sSdLbl, 16, 68);

    lv_obj_t* planBtn = box(sFreeBox, 224, 50);
    lv_obj_set_pos(planBtn, 16, 102);
    lv_obj_set_style_radius(planBtn, 16, 0);
    lv_obj_set_style_bg_color(planBtn, accent(), 0);
    lv_obj_set_style_bg_opa(planBtn, LV_OPA_COVER, 0);
    accReg(planBtn, ACC_BG);
    lv_obj_add_flag(planBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(planBtn, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        populateSearchList();
        setHidden(sSearchModal, false);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* planLbl = label(planBtn, F.small, cBg(), "ROUTENPLANUNG (DE)");
    lv_obj_center(planLbl);

    lv_obj_t* meshBtn = box(sFreeBox, 118, 50);
    lv_obj_set_pos(meshBtn, 246, 102);
    lv_obj_set_style_radius(meshBtn, 16, 0);
    lv_obj_set_style_bg_color(meshBtn, cSurf2(), 0);
    lv_obj_set_style_bg_opa(meshBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(meshBtn, 2, 0);
    lv_obj_set_style_border_color(meshBtn, lv_color_hex(0xBF5AF2), 0);
    lv_obj_add_flag(meshBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(meshBtn, [](lv_event_t*) {
        sActiveFilter = 1; // MESH tab
        haptic(HAPTIC_CLICK);
        populateSearchList();
        setHidden(sSearchModal, false);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* meshLbl = label(meshBtn, F.small, lv_color_hex(0xBF5AF2), "MESH NAV");
    lv_obj_center(meshLbl);

    // =========================================================================
    // Modal 1: Germany & Stuttgart Place + LoRa/Meshtastic Node Selector
    // =========================================================================
    sSearchModal = box(p, SCR_W, SCR_H);
    lv_obj_set_pos(sSearchModal, 0, 0);
    lv_obj_set_style_bg_color(sSearchModal, cBg(), 0);
    lv_obj_set_style_bg_opa(sSearchModal, LV_OPA_COVER, 0);

    lv_obj_t* stitle = label(sSearchModal, F.title, cText(), "ZIEL & ORTSUCHE");
    lv_obj_set_pos(stitle, M, 20);

    lv_obj_t* closeBtn = box(sSearchModal, 78, 34);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -M, 18);
    lv_obj_set_style_radius(closeBtn, 14, 0);
    lv_obj_set_style_bg_color(closeBtn, cSurf2(), 0);
    lv_obj_set_style_bg_opa(closeBtn, LV_OPA_COVER, 0);
    lv_obj_add_flag(closeBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(closeBtn, [](lv_event_t*) {
        setHidden(sSearchModal, true);
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* closeTxt = label(closeBtn, F.small, cCrit(), "ZURÜCK");
    lv_obj_center(closeTxt);

    const int fw = (CW - 12) / 7;
    for (int i = 0; i < 7; i++) {
        lv_obj_t* fb = box(sSearchModal, fw - 2, 34);
        lv_obj_set_pos(fb, M + i * fw, 64);
        lv_obj_set_style_radius(fb, 10, 0);
        lv_obj_set_style_bg_color(fb, i == 0 ? accent() : cSurf2(), 0);
        lv_obj_set_style_bg_opa(fb, LV_OPA_COVER, 0);
        lv_obj_add_flag(fb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(fb, [](lv_event_t* e) {
            sActiveFilter = (int)(intptr_t)lv_event_get_user_data(e);
            haptic(HAPTIC_CLICK);
            populateSearchList();
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* fl = caption(fb, FILTER_LABELS[i], cText());
        lv_obj_center(fl);
        sFilterBtns[i] = fb;
    }

    sSearchList = lv_obj_create(sSearchModal);
    lv_obj_set_size(sSearchList, CW, 376);
    lv_obj_set_pos(sSearchList, M, 108);
    lv_obj_set_style_bg_opa(sSearchList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sSearchList, 0, 0);
    lv_obj_set_style_pad_all(sSearchList, 0, 0);
    lv_obj_set_scrollbar_mode(sSearchList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(sSearchList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sSearchList, 8, 0);

    setHidden(sSearchModal, true);

    // =========================================================================
    // Modal 2: GNSS Satellite Skyplot (Polar Radar + SNR Signal Bars)
    // =========================================================================
    sSkyModal = box(p, SCR_W, SCR_H);
    lv_obj_set_pos(sSkyModal, 0, 0);
    lv_obj_set_style_bg_color(sSkyModal, cBg(), 0);
    lv_obj_set_style_bg_opa(sSkyModal, LV_OPA_COVER, 0);

    lv_obj_t* skyTitle = label(sSkyModal, F.title, cText(), "GNSS · SKYPLOT");
    lv_obj_set_pos(skyTitle, M, 20);

    lv_obj_t* skyClose = box(sSkyModal, 78, 34);
    lv_obj_align(skyClose, LV_ALIGN_TOP_RIGHT, -M, 18);
    lv_obj_set_style_radius(skyClose, 14, 0);
    lv_obj_set_style_bg_color(skyClose, cSurf2(), 0);
    lv_obj_set_style_bg_opa(skyClose, LV_OPA_COVER, 0);
    lv_obj_add_flag(skyClose, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(skyClose, [](lv_event_t*) {
        setHidden(sSkyModal, true);
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(skyClose, F.small, cCrit(), "ZURÜCK"));

    sSkySub = caption(sSkyModal, "GNSS: 0 Sats in Sicht", accent());
    lv_obj_set_pos(sSkySub, M, 60);

    // Polar Skyplot Radar Card (left side 208x220)
    lv_obj_t* radarCard = card(sSkyModal, 212, 224);
    lv_obj_set_pos(radarCard, M, 86);
    sSkyRing = box(radarCard, 200, 200);
    lv_obj_center(sSkyRing);
    lv_obj_set_style_radius(sSkyRing, 100, 0);
    lv_obj_set_style_border_width(sSkyRing, 2, 0);
    lv_obj_set_style_border_color(sSkyRing, cSurf2(), 0);

    lv_obj_t* midRing = box(sSkyRing, 116, 116);
    lv_obj_center(midRing);
    lv_obj_set_style_radius(midRing, 58, 0);
    lv_obj_set_style_border_width(midRing, 1, 0);
    lv_obj_set_style_border_color(midRing, cSurf2(), 0);

    lv_obj_align(caption(sSkyRing, "N", cCrit()), LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_align(caption(sSkyRing, "S", cText2()), LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_align(caption(sSkyRing, "O", cText2()), LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_align(caption(sSkyRing, "W", cText2()), LV_ALIGN_LEFT_MID, 6, 0);

    for (int i = 0; i < 16; i++) {
        lv_obj_t* d = box(sSkyRing, 22, 22);
        lv_obj_set_style_radius(d, 11, 0);
        lv_obj_set_style_bg_color(d, cOk(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_t* pl = caption(d, "1", cBg());
        lv_obj_center(pl);
        setHidden(d, true);
        sSatDots[i] = d;
    }

    // SNR Signal Bar Card (bottom 374x168)
    lv_obj_t* snrCard = card(sSkyModal, CW, 168);
    lv_obj_set_pos(snrCard, M, 318);
    lv_obj_t* snrHdr = caption(snrCard, "SIGNALSTÄRKE (C/N0 dB-Hz)", cText2());
    lv_obj_set_pos(snrHdr, 14, 8);

    for (int i = 0; i < 8; i++) {
        int bx = 18 + i * 44;
        lv_obj_t* b = box(snrCard, 26, 10);
        lv_obj_set_pos(b, bx, 96);
        lv_obj_set_style_radius(b, 5, 0);
        lv_obj_set_style_bg_color(b, accent(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        sSatSnrBars[i] = b;

        lv_obj_t* l = caption(snrCard, "--", cText2());
        lv_obj_set_pos(l, bx - 2, 114);
        sSatSnrLbls[i] = l;
    }

    setHidden(sSkyModal, true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 0;
    d.onShow = [] {
        GpsManager::setLive(true);
        sManualPan = false;
        sAutoZoom = true;
        sMapNeedsRedraw = true;
    };
    d.onHide = [] {
        GpsManager::setLive(false);
        if (sSearchModal) setHidden(sSearchModal, true);
        if (sSkyModal) setHidden(sSkyModal, true);
    };
    nav::registerPage(nav::PG_POSITION, d);
}

}  // namespace pages
