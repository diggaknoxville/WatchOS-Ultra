// Germany Offline Navigation & GNSS + LoRa/Meshtastic Hybrid Positioning Page
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "GpsManager.h"
#include "NavEngine.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include <math.h>

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

static int sZoom = 7;              // 6=Germany overview .. 14=Street level
static bool sManualPan = false;
static double sViewLat = 51.1657;  // Germany center when zoomed out
static double sViewLon = 10.4515;
static bool sMapNeedsRedraw = true;

static const char* FILTER_LABELS[7] = {"TOP", "MESH", "A-D", "E-K", "L-M", "N-S", "T-Z"};

static void redrawCanvas() {
    if (!sMapBuf || !sMapImg) return;
    const NavStatus& st = NavEngine::status();
    double lat = sManualPan ? sViewLat : ((sZoom <= 7 && !st.activeRoute) ? 51.15 : st.curLat);
    double lon = sManualPan ? sViewLon : ((sZoom <= 7 && !st.activeRoute) ? 10.45 : st.curLon);
    if (!sManualPan && st.activeRoute && sZoom <= 8) {
        lat = (st.curLat + st.destLat) * 0.5;
        lon = (st.curLon + st.destLon) * 0.5;
    }
    sViewLat = lat;
    sViewLon = lon;
    NavEngine::renderMap(sMapBuf, MAP_W, MAP_H, lat, lon, sZoom);
    lv_obj_invalidate(sMapImg);
    sMapNeedsRedraw = false;
}

static void populateSearchList() {
    if (!sSearchList) return;
    lv_obj_clean(sSearchList);

    if (sActiveFilter == 1) {
        // Ensure demo peers exist if none received over air yet so user can test Mesh Navigation immediately
        RadioLoRa::injectDemoPeers();
        sNumResults = NavEngine::searchPlaces("@mesh", sSearchResults, 20);
    } else if (sActiveFilter == 0) {
        sNumResults = NavEngine::searchPlaces("", sSearchResults, 20);
    } else {
        // Filter by starting letter range
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
                if (sZoom < 8) sZoom = 8;
                sMapNeedsRedraw = true;
                setHidden(sSearchModal, true);
                haptic(HAPTIC_SUCCESS);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        bool isMesh = (sSearchResults[i].rank == 0);
        lv_obj_t* ic = label(row, F.icoS, isMesh ? lv_color_hex(0xBF5AF2) : accent(),
                             isMesh ? ICO_PODCASTS : ICO_PLACE);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t* nm = label(row, F.small, cText(), sSearchResults[i].name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 42, 0);

        char distBuf[24];
        const NavStatus& st = NavEngine::status();
        double dlat = (sSearchResults[i].lat - st.curLat) * 111.32;
        double dlon = (sSearchResults[i].lon - st.curLon) * 71.5;
        double km = sqrt(dlat * dlat + dlon * dlon);
        if (isMesh) snprintf(distBuf, sizeof(distBuf), "MESH  %.1f km", km);
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

    // 3. Zoom & GPX status badges
    char zb[16];
    snprintf(zb, sizeof(zb), "Z%d", sZoom);
    setText(sZoomLbl, zb);
    setBgColorIf(sGpxBtn, st.gpxRecording ? cWarn() : cSurf2());
    setText(sGpxTxt, st.gpxRecording ? "REC" : "GPX");

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
        char cb[64];
        snprintf(cb, sizeof(cb), "%.4f° N   %.4f° E   [%s]",
                 fabs(st.curLat), fabs(st.curLon), st.posSourceTag);
        setText(sCoordLbl, cb);

        char mb[64];
        snprintf(mb, sizeof(mb), "SAT %u  ·  %.0f m  ·  %.1f km/h  ·  MESH %u Nodes",
                 GpsManager::satellites(), GpsManager::altitudeM(),
                 GpsManager::speedKmh(), RadioLoRa::getPeerCount());
        setText(sMetaLbl, mb);

        char sb[64];
        if (st.mapLoaded) {
            snprintf(sb, sizeof(sb), "SD %lu GB · DE-KARTE (%lu Orte / %lu Knoten)",
                     (unsigned long)(st.sdTotalMB / 1024),
                     (unsigned long)st.mapPlaces, (unsigned long)st.mapRouteNodes);
        } else if (st.sdReady) {
            snprintf(sb, sizeof(sb), "SD %lu GB bereit · DE-Basiskarte (%lu Städte)",
                     (unsigned long)max<uint32_t>(1, st.sdTotalMB / 1024), (unsigned long)st.mapPlaces);
        } else {
            snprintf(sb, sizeof(sb), "Keine SD · DE-Basiskarte aktiv");
        }
        setText(sSdLbl, sb);
    }

    if (force || sMapNeedsRedraw) {
        redrawCanvas();
    }
}

void buildPosition(lv_obj_t* scr) {
    NavEngine::init();

    lv_obj_t* p = pageRoot(scr);
    sRoot = p;
    title(p, "NAVI · DE");

    // Mode Toggle Pill (AUTO / FUSS / MESH)
    sModePill = box(p, 76, 30);
    lv_obj_align(sModePill, LV_ALIGN_TOP_RIGHT, -(M + 104), 26);
    lv_obj_set_style_radius(sModePill, 15, 0);
    lv_obj_set_style_bg_opa(sModePill, LV_OPA_30, 0);
    lv_obj_add_flag(sModePill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sModePill, [](lv_event_t*) {
        NavMode next = (NavMode)(((int)NavEngine::getMode() + 1) % 3);
        if (next == NAV_MODE_MESH) RadioLoRa::injectDemoPeers();
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

    // Tap on map canvas: edges pan N/S/W/E, center recenters on GPS/Mesh position
    lv_obj_add_flag(sMapCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sMapCard, [](lv_event_t*) {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        int rx = pt.x - M;
        int ry = pt.y - 76;
        double step = 360.0 / (256.0 * pow(2.0, (double)sZoom)) * 75.0;
        if (ry < 55) { sManualPan = true; sViewLat += step * 0.7; }
        else if (ry > MAP_H - 55) { sManualPan = true; sViewLat -= step * 0.7; }
        else if (rx < 75) { sManualPan = true; sViewLon -= step; }
        else if (rx > MAP_W - 75) { sManualPan = true; sViewLon += step; }
        else { sManualPan = false; } // Center tap -> snap back to live GPS/Mesh position
        sMapNeedsRedraw = true;
        haptic(HAPTIC_TICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    // Floating Map Controls: Zoom +, Zoom -, GPX/POI, Search/Route
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
        if (sZoom < 15) sZoom++;
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    });
    makeMapBtn(MAP_W - 48, 54, 38, 38, "-", [](lv_event_t*) {
        if (sZoom > 6) sZoom--;
        sMapNeedsRedraw = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    });

    lv_obj_t* zBadge = box(sMapCard, 38, 24);
    lv_obj_set_pos(zBadge, MAP_W - 48, 96);
    lv_obj_set_style_radius(zBadge, 8, 0);
    lv_obj_set_style_bg_color(zBadge, cBg(), 0);
    lv_obj_set_style_bg_opa(zBadge, LV_OPA_70, 0);
    sZoomLbl = caption(zBadge, "Z7", accent());
    lv_obj_center(sZoomLbl);

    sGpxBtn = makeMapBtn(10, 10, 56, 34, "GPX", [](lv_event_t*) {
        bool rec = !NavEngine::isTrackRecording();
        NavEngine::setTrackRecording(rec);
        NavEngine::markWaypoint("MARK");
        haptic(HAPTIC_CLICK);
        DisplayEngine::showNotification(rec ? "GPX Logger AN" : "GPX Logger AUS",
                                        rec ? "Track & Wegpunkt auf SD gespeichert" : "GPX Aufzeichnung beendet",
                                        2000, ICO_ROUTE);
        refresh(true);
    });
    sGpxTxt = lv_obj_get_child(sGpxBtn, 0);

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

    sTurnDest = label(sNavBox, F.small, accent(), "Ziel: München");
    accReg(sTurnDest, ACC_TEXT);
    lv_obj_set_pos(sTurnDest, 62, 42);

    sTurnMeta = caption(sNavBox, "Rest 0 km · ETA 0m", cText2());
    lv_obj_set_pos(sTurnMeta, 18, 76);

    // Action buttons inside active navigation card: ROUTE ÄNDERN | LORA TEILEN | STOP
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

    // Two prominent buttons in Free Box: ROUTENPLANUNG / ORTSUCHE & MESH RADAR
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
        RadioLoRa::injectDemoPeers();
        sActiveFilter = 1; // MESH tab
        haptic(HAPTIC_CLICK);
        populateSearchList();
        setHidden(sSearchModal, false);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* meshLbl = label(meshBtn, F.small, lv_color_hex(0xBF5AF2), "MESH NAV");
    lv_obj_center(meshLbl);

    // =========================================================================
    // Modal Overlay: Germany Place & LoRa/Meshtastic Node Selector
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

    // Filter Pill Bar (TOP | MESH | A-D | E-K | L-M | N-S | T-Z)
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

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 0;
    d.onShow = [] {
        GpsManager::setLive(true);
        sMapNeedsRedraw = true;
    };
    d.onHide = [] {
        GpsManager::setLive(false);
        if (sSearchModal) setHidden(sSearchModal, true);
    };
    nav::registerPage(nav::PG_POSITION, d);
}

}  // namespace pages
