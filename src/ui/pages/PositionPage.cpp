// Position: offline vector map with HUD (source, zoom, scale), honest position
// states, async routing feedback, GNSS skyplot and place / mesh-node search.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "GpsManager.h"
#include "NavEngine.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include <esp_heap_caps.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace ui;

namespace pages {

static constexpr int MAP_W = CW;            // 366
static constexpr int MAP_H = 224;
static constexpr int MAP_Y = CONTENT_Y;     // 82
static constexpr int INFO_Y = MAP_Y + MAP_H + 6;   // 312
static constexpr int INFO_H = 98;
static constexpr int BAR_Y = INFO_Y + INFO_H + 8;  // 418
static constexpr int BAR_H = 50;

static lv_obj_t *sRoot, *sGpsPill, *sSatBtn;
static lv_obj_t *sMapCard, *sMapImg;
static lv_image_dsc_t sMapDsc{};
static uint16_t* sMapBuf = nullptr;

// HUD on the map
static lv_obj_t *sSrcPill, *sRoutePill, *sZoomBadge, *sZoomLbl, *sScaleBar, *sScaleLbl, *sRecenterBtn;
static lv_obj_t *sNoPosOverlay;

// info card (free) / guidance card (navigating)
static lv_obj_t *sFreeBox, *sCoordLbl, *sMetaLbl, *sMapLbl;
static lv_obj_t *sNavBox, *sTurnIcon, *sTurnCmd, *sTurnDest, *sTurnMeta, *sMeshSendBtn;

// action bar
static lv_obj_t *sDestBtn, *sModeTabs, *sRecBtn, *sWpBtn;

// search modal
static lv_obj_t *sSearchModal, *sSearchList, *sFilterTabs;
static int sActiveFilter = 0;
static NavPlace sSearchResults[24];
static NavPlace* sPool = nullptr;           // 64 entries in PSRAM (filter scans)
static constexpr int POOL_N = 64;
static int sNumResults = 0;

// skyplot modal
static lv_obj_t *sSkyModal, *sSkySub, *sSkyRing, *sSatDots[16], *sSatSnrBars[8], *sSatSnrLbls[8];

static int sZoom = 15;
static bool sAutoZoom = true;
static bool sManualPan = false;
static bool sDragging = false;
static unsigned long sManualInteractMs = 0;
static double sViewLat = 0, sViewLon = 0;
static double sLastRenderedLat = 0, sLastRenderedLon = 0;
static int sLastRenderedZoom = -1;
static bool sMapNeedsRedraw = true;
static uint32_t sLastRefreshMs = 0;

static const char* FILTER_LABELS[7] = {"STGT", "MESH", "A-D", "E-K", "L-M", "N-S", "T-Z"};

static int computeSpeedAdaptiveZoom(float speedKmh) {
    if (speedKmh <= 6.5f)  return 15;
    if (speedKmh <= 22.0f) return 14;
    if (speedKmh <= 55.0f) return 13;
    if (speedKmh <= 90.0f) return 11;
    return 10;
}

static bool modalVisible() {
    return (sSearchModal && !lv_obj_has_flag(sSearchModal, LV_OBJ_FLAG_HIDDEN)) ||
           (sSkyModal && !lv_obj_has_flag(sSkyModal, LV_OBJ_FLAG_HIDDEN));
}

// horizontal drags inside the map pan the map; the screen edges still swipe pages
static bool allowHorizontal(int x, int y) {
    if (modalVisible()) return false;
    bool insideMap = (y >= MAP_Y && y <= MAP_Y + MAP_H && x >= M && x <= M + MAP_W);
    if (insideMap && x >= 36 && x <= SCR_W - 36) return false;
    return true;
}

static void updateScaleBar(double lat) {
    static const int cand[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};
    double mpp = 156543.03 * cos(lat * M_PI / 180.0) / pow(2.0, (double)sZoom);
    if (mpp <= 0) return;
    int best = cand[0];
    for (int c : cand) { if (c / mpp <= 110.0) best = c; else break; }
    int px = (int)(best / mpp);
    if (px < 10) px = 10;
    if (lv_obj_get_width(sScaleBar) != px) lv_obj_set_width(sScaleBar, px);
    char b[16];
    if (best >= 1000) snprintf(b, sizeof(b), "%d km", best / 1000);
    else snprintf(b, sizeof(b), "%d m", best);
    setText(sScaleLbl, b);
}

// one software render per refresh frame at most (never from touch events)
static void redrawCanvas(const NavStatus& st) {
    if (!sMapBuf || !sMapImg) return;
    if ((sManualPan || !sAutoZoom) && !sDragging && (millis() - sManualInteractMs > 15000)) {
        sManualPan = false;
        sAutoZoom = true;
    }
    if (sAutoZoom) sZoom = computeSpeedAdaptiveZoom(GpsManager::speedKmh());
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
    updateScaleBar(lat);
}

static void refreshSkyplot() {
    if (!sSkyModal || lv_obj_has_flag(sSkyModal, LV_OBJ_FLAG_HIDDEN)) return;
    static GpsSatInfo sats[16];
    uint8_t n = GpsManager::getSatellitesInView(sats, 16);
    char sb[72];
    snprintf(sb, sizeof(sb), "%u Satelliten sichtbar · Fix mit %u · HDOP %.1f", (unsigned)n,
             (unsigned)GpsManager::satellites(), GpsManager::hdop() < 50.0f ? GpsManager::hdop() : 0.0f);
    setText(sSkySub, sb);
    const int cx = 100, cy = 100, maxR = 84;
    for (int i = 0; i < 16; i++) {
        if (i < n) {
            setHidden(sSatDots[i], false);
            float el = constrain((float)sats[i].elevation, 0.0f, 90.0f);
            float azRad = (float)sats[i].azimuth * (float)M_PI / 180.0f;
            float r = (1.0f - (el / 90.0f)) * maxR;
            lv_obj_set_pos(sSatDots[i], cx + (int)(sinf(azRad) * r) - 11, cy - (int)(cosf(azRad) * r) - 11);
            setBgColorIf(sSatDots[i], (sats[i].snr >= 25) ? cOk() : (sats[i].snr >= 15) ? accent() : cWarn());
            char prn[8];
            snprintf(prn, sizeof(prn), "%u", sats[i].prn);
            setText(lv_obj_get_child(sSatDots[i], 0), prn);
        } else {
            setHidden(sSatDots[i], true);
        }
    }
    for (int i = 0; i < 8; i++) {
        if (i < n) {
            int hBar = constrain((int)sats[i].snr * 2, 6, 96);
            lv_obj_set_height(sSatSnrBars[i], hBar);
            lv_obj_set_y(sSatSnrBars[i], 106 - hBar);
            setBgColorIf(sSatSnrBars[i], (sats[i].snr >= 25) ? cOk() : (sats[i].snr >= 15) ? accent() : cWarn());
            char lb[12];
            snprintf(lb, sizeof(lb), "%02u\n%udB", sats[i].prn, sats[i].snr);
            setText(sSatSnrLbls[i], lb);
        } else {
            lv_obj_set_height(sSatSnrBars[i], 4);
            lv_obj_set_y(sSatSnrBars[i], 102);
            setBgColorIf(sSatSnrBars[i], cSurf2());
            setText(sSatSnrLbls[i], "--");
        }
    }
}

static void populateSearchList();

static void openSearch(int filter) {
    sActiveFilter = filter;
    populateSearchList();
    setHidden(sSearchModal, false);
}

// HUD pills keep a dark backdrop over the map; only text + colour change
static void setHudPill(lv_obj_t* p, const char* text, lv_color_t col) {
    lv_obj_t* l = pillLabel(p);
    if (!l) return;
    setText(l, text);
    setTextColorIf(l, col);
}

static void populateSearchList() {
    if (!sSearchList) return;
    lv_obj_clean(sSearchList);
    setTabActive(sFilterTabs, sActiveFilter);

    if (sActiveFilter == 1) {
        sNumResults = NavEngine::searchPlaces("@mesh", sSearchResults, 20);
    } else if (sActiveFilter == 0) {
        sNumResults = NavEngine::searchPlaces("", sSearchResults, 20);
    } else {
        static const char ranges[][2] = {{0, 0}, {0, 0}, {'A', 'D'}, {'E', 'K'}, {'L', 'M'}, {'N', 'S'}, {'T', 'Z'}};
        char cMin = ranges[sActiveFilter][0], cMax = ranges[sActiveFilter][1];
        if (!sPool) sPool = (NavPlace*)heap_caps_malloc(sizeof(NavPlace) * POOL_N, MALLOC_CAP_SPIRAM);
        sNumResults = 0;
        if (sPool) {
            int nPool = NavEngine::searchPlaces("", sPool, POOL_N);
            for (int i = 0; i < nPool && sNumResults < 24; i++) {
                char c = sPool[i].name[0];
                if (c >= 'a' && c <= 'z') c -= 32;
                if (c >= cMin && c <= cMax) sSearchResults[sNumResults++] = sPool[i];
            }
        }
    }

    if (sNumResults == 0) {
        lv_obj_t* empty = card(sSearchList, CW, 150);
        lv_obj_t* es = emptyState(empty, sActiveFilter == 1 ? ICO_CELL_TOWER : ICO_SEARCH,
                                  sActiveFilter == 1 ? "Keine Knoten mit Position" : "Keine Orte",
                                  sActiveFilter == 1 ? "Discovery fragt Knoten nach ihrer Position" : "Karte auf SD-Karte prüfen");
        lv_obj_align(es, LV_ALIGN_TOP_MID, 0, 6);
        if (sActiveFilter == 1) {
            lv_obj_t* disc = secondaryButton(sSearchList, CW, 48, ICO_PODCASTS, "Discovery senden");
            lv_obj_add_event_cb(disc, [](lv_event_t*) {
                haptic(HAPTIC_CLICK);
                bool ok = RadioLoRa::sendNodeInfoDiscovery();
                DisplayEngine::showToast("Mesh Discovery",
                                         ok ? "NodeInfo-Anfrage gesendet"
                                            : (RadioLoRa::isEnabled() ? "Bereits gesendet - bitte warten" : "LoRa ist aus"),
                                         2200, ICO_PODCASTS);
            }, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    NavStatus st = NavEngine::status();
    bool havePos = st.posSource != POS_SRC_DEFAULT;
    for (int i = 0; i < sNumResults; i++) {
        lv_obj_t* row = card(sSearchList, CW, 48);
        lv_obj_set_style_radius(row, 14, 0);
        pressable(row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (idx < 0 || idx >= sNumResults) return;
            const NavPlace& pl = sSearchResults[idx];
            if (pl.rank == 0) NavEngine::setMode(NAV_MODE_MESH);
            else if (NavEngine::getMode() == NAV_MODE_MESH) NavEngine::setMode(NAV_MODE_CAR);
            bool accepted = NavEngine::startNavigation(pl.name, pl.lat, pl.lon);
            if (!accepted) DisplayEngine::showToast("Navigation", "Route kann gerade nicht gestartet werden", 2200, ICO_ROUTE);
            sManualPan = false;
            sAutoZoom = true;
            sMapNeedsRedraw = true;
            setHidden(sSearchModal, true);
            haptic(HAPTIC_SUCCESS);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        bool isMesh = (sSearchResults[i].rank == 0);
        bool isPoi  = (sSearchResults[i].rank == 5);
        lv_color_t icCol = isMesh ? cMesh() : isPoi ? cWarn() : accent();
        lv_obj_t* ic = label(row, F.icoS, icCol, isMesh ? ICO_PODCASTS : isPoi ? ICO_FLAG : ICO_PLACE);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t* nm = label(row, F.small, cText(), sSearchResults[i].name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, 210);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 42, 0);
        char distBuf[24];
        if (havePos) {
            double dlat = (sSearchResults[i].lat - st.curLat) * 111.32;
            double dlon = (sSearchResults[i].lon - st.curLon) * 111.32 * cos(st.curLat * M_PI / 180.0);
            double km = sqrt(dlat * dlat + dlon * dlon);
            if (km < 10.0) snprintf(distBuf, sizeof(distBuf), "%.1f km", km);
            else snprintf(distBuf, sizeof(distBuf), "%.0f km", km);
        } else {
            snprintf(distBuf, sizeof(distBuf), "--");
        }
        lv_obj_t* dl = caption(row, distBuf, isMesh ? cMesh() : cText2());
        lv_obj_align(dl, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static void refresh(bool force) {
    NavStatus st = NavEngine::status();
    uint32_t now = millis();
    // after a sleep (PowerManager stops live GNSS) re-enable it while this page is open
    if (now - sLastRefreshMs > 3000 && nav::current() == nav::PG_POSITION) GpsManager::setLive(true);
    sLastRefreshMs = now;

    // header: GPS pill + source pill on the map
    bool en = GpsManager::isEnabled();
    bool fix = GpsManager::hasFix();
    setPill(sGpsPill, !en ? "GPS AUS" : fix ? "GPS FIX" : "GPS SUCHT", !en ? cText2() : fix ? cOk() : cWarn());
    lv_color_t srcCol = (st.posSource == POS_SRC_GPS_LIVE) ? cOk() :
                        (st.posSource == POS_SRC_LORA_MESH) ? cMesh() :
                        (st.posSource == POS_SRC_SAVED_FIX) ? cWarn() : cCrit();
    char sp[40];
    if (st.posSource != POS_SRC_GPS_LIVE && st.posSource != POS_SRC_DEFAULT && st.posAgeSec) {
        if (st.posAgeSec < 3600) snprintf(sp, sizeof(sp), "%s · %lu MIN", st.posSourceTag, (unsigned long)(st.posAgeSec / 60));
        else snprintf(sp, sizeof(sp), "%s · %lu H", st.posSourceTag, (unsigned long)(st.posAgeSec / 3600));
    } else {
        snprintf(sp, sizeof(sp), "%s", st.posSourceTag);
    }
    setHudPill(sSrcPill, sp, srcCol);
    setHidden(sNoPosOverlay, st.posSource != POS_SRC_DEFAULT);

    // routing state pill
    if (st.routingBusy) { setHudPill(sRoutePill, "ROUTE WIRD BERECHNET...", accent()); setHidden(sRoutePill, false); }
    else if (st.activeRoute && st.routeStraightLine) { setHudPill(sRoutePill, "LUFTLINIE", cWarn()); setHidden(sRoutePill, false); }
    else if (st.routeFailed) { setHudPill(sRoutePill, "KEINE ROUTE", cCrit()); setHidden(sRoutePill, false); }
    else setHidden(sRoutePill, true);

    // auto-zoom / auto-centre trigger
    int targetZoom = sAutoZoom ? computeSpeedAdaptiveZoom(GpsManager::speedKmh()) : sZoom;
    if (targetZoom != sLastRenderedZoom ||
        (!sManualPan && (fabs(st.curLat - sLastRenderedLat) > 0.00008 || fabs(st.curLon - sLastRenderedLon) > 0.00008))) {
        sZoom = targetZoom;
        sMapNeedsRedraw = true;
    }

    char b[80];
    snprintf(b, sizeof(b), "%sZ%d", sAutoZoom ? "A " : "", sZoom);
    setText(sZoomLbl, b);
    setHidden(sRecenterBtn, !sManualPan && sAutoZoom);

    // action bar state
    setTabActive(sModeTabs, (int)st.mode);
    setBgColorIf(sRecBtn, st.gpxRecording ? cCrit() : cSurf2());
    setButtonText(sRecBtn, st.gpxRecording ? "REC" : "GPX");
    setButtonEnabled(sRecBtn, st.posSource == POS_SRC_GPS_LIVE || st.gpxRecording);
    setButtonEnabled(sWpBtn, st.posSource != POS_SRC_DEFAULT);
    setButtonText(sDestBtn, st.activeRoute ? "Stopp" : "Ziel");
    setBgColorIf(sDestBtn, st.activeRoute ? cCrit() : accent());
    lv_obj_t* dl = buttonLabel(sDestBtn);
    if (dl) setTextColorIf(dl, st.activeRoute ? cText() : cBg());

    // info vs guidance
    setHidden(sNavBox, !st.activeRoute);
    setHidden(sFreeBox, st.activeRoute);
    const char* mStr = (st.mode == NAV_MODE_CAR) ? "AUTO" : (st.mode == NAV_MODE_PED) ? "FUSS" : "MESH";
    if (st.activeRoute) {
        setText(sTurnIcon, st.nextTurnIcon ? st.nextTurnIcon : ICO_NAVIGATION);
        if (st.routingBusy) snprintf(b, sizeof(b), "Route wird berechnet...");
        else if (st.nextTurnDistM >= 1000) snprintf(b, sizeof(b), "%s (%.1f km)", st.nextTurnCmd, st.nextTurnDistM / 1000.0f);
        else snprintf(b, sizeof(b), "%s (%lu m)", st.nextTurnCmd, (unsigned long)st.nextTurnDistM);
        setText(sTurnCmd, b);
        snprintf(b, sizeof(b), "Ziel: %s (%s)", st.destName, mStr);
        setText(sTurnDest, b);
        if (st.routingBusy) snprintf(b, sizeof(b), "Bitte warten");
        else if (st.etaMinutes >= 60) snprintf(b, sizeof(b), "Rest %.1f km · ETA %uh %02umin · %.0f°", st.remDistKm, st.etaMinutes / 60, st.etaMinutes % 60, st.bearingDeg);
        else snprintf(b, sizeof(b), "Rest %.1f km · ETA %u min · %.0f°", st.remDistKm, st.etaMinutes, st.bearingDeg);
        setText(sTurnMeta, b);
        setButtonEnabled(sMeshSendBtn, RadioLoRa::isEnabled());
    } else {
        if (st.posSource == POS_SRC_DEFAULT) snprintf(b, sizeof(b), "Keine Position bekannt");
        else snprintf(b, sizeof(b), "%.4f° %c  %.4f° %c", fabs(st.curLat), st.curLat < 0 ? 'S' : 'N', fabs(st.curLon), st.curLon < 0 ? 'W' : 'O');
        setText(sCoordLbl, b);
        if (fix) snprintf(b, sizeof(b), "%u SAT · %.0f m · %.1f km/h", (unsigned)GpsManager::satellites(), GpsManager::altitudeM(), GpsManager::speedKmh());
        else if (en) snprintf(b, sizeof(b), "%u SAT · Suche seit %lu s", (unsigned)GpsManager::satellites(), (unsigned long)GpsManager::searchSeconds());
        else snprintf(b, sizeof(b), "GPS ist aus");
        setText(sMetaLbl, b);
        if (st.ultraLoaded) snprintf(b, sizeof(b), "Stuttgart HD · %lu POI · SD %s", (unsigned long)st.mapPois, st.sdWritable ? "R/W" : (st.sdReady ? "RO" : "--"));
        else if (st.mapLoaded && st.mapLoadPct < 100) snprintf(b, sizeof(b), "Karte lädt %u%%", (unsigned)st.mapLoadPct);
        else if (st.mapLoaded) snprintf(b, sizeof(b), "Deutschland · %lu Orte · %lu Knoten", (unsigned long)st.mapPlaces, (unsigned long)st.mapRouteNodes);
        else snprintf(b, sizeof(b), "Keine Karte (SD prüfen)");
        setText(sMapLbl, b);
    }

    if (force || sMapNeedsRedraw) redrawCanvas(st);
    refreshSkyplot();
}

// ---- map touch: pan with the finger, redraw throttled to the refresh frame ----
static lv_point_t sDragStart = {0, 0};
static double sDragStartLat = 0, sDragStartLon = 0;
static bool sMoved = false;

static void mapTouch(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (code == LV_EVENT_PRESSED) {
        sDragStart = pt;
        sDragStartLat = sViewLat;
        sDragStartLon = sViewLon;
        sMoved = false;
        sDragging = true;
        nav::setRefreshInterval(nav::PG_POSITION, 33);
    } else if (code == LV_EVENT_PRESSING) {
        int dx = pt.x - sDragStart.x, dy = pt.y - sDragStart.y;
        if (abs(dx) > 4 || abs(dy) > 4) {
            sMoved = true;
            sManualPan = true;
            sManualInteractMs = millis();
            double degPerPixX = 360.0 / (256.0 * pow(2.0, (double)sZoom));
            double degPerPixY = degPerPixX * cos(sDragStartLat * M_PI / 180.0);
            sViewLon = sDragStartLon - (double)dx * degPerPixX;
            sViewLat = sDragStartLat + (double)dy * degPerPixY;
            sMapNeedsRedraw = true;                     // rendered by the next refresh() frame
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        sDragging = false;
        nav::setRefreshInterval(nav::PG_POSITION, 1000);
        if (code == LV_EVENT_RELEASED && !sMoved) {
            int rx = pt.x - M, ry = pt.y - MAP_Y;
            if (abs(rx - MAP_W / 2) < 60 && abs(ry - MAP_H / 2) < 60) {   // tap on centre = recentre
                sManualPan = false;
                sAutoZoom = true;
                sMapNeedsRedraw = true;
                haptic(HAPTIC_TICK);
            }
        }
        sMoved = false;
        refresh(true);
    }
}

static lv_obj_t* hudButton(lv_obj_t* parent, int x, int y, int w, int h, const char* txt) {
    lv_obj_t* b = box(parent, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, cBg(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, cStroke(), 0);
    pressable(b);
    lv_obj_t* l = label(b, F.small, cText(), txt);
    lv_obj_center(l);
    return b;
}

void buildPosition(lv_obj_t* scr) {
    NavEngine::init();

    lv_obj_t* p = pageRoot(scr);
    sRoot = p;
    title(p, "POSITION");

    sSatBtn = pill(p, "SAT", cInfo());
    lv_obj_align(sSatBtn, LV_ALIGN_TOP_RIGHT, -(M_HEAD + 120), 26);
    pressable(sSatBtn);
    lv_obj_add_event_cb(sSatBtn, [](lv_event_t*) { haptic(HAPTIC_TICK); setHidden(sSkyModal, false); refreshSkyplot(); }, LV_EVENT_CLICKED, NULL);

    sGpsPill = pill(p, "GPS AUS", cText2());
    lv_obj_align(sGpsPill, LV_ALIGN_TOP_RIGHT, -M_HEAD, 26);
    pressable(sGpsPill);
    lv_obj_add_event_cb(sGpsPill, [](lv_event_t*) {
        bool en = !GpsManager::isEnabled();
        GpsManager::setEnabled(en);
        if (en) { GpsManager::setLive(true); GpsManager::requestFix(); }
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_APP_SETTINGS);
        haptic(HAPTIC_TICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    // ------------------------------------------------------------ map
    sMapCard = card(p, MAP_W, MAP_H);
    lv_obj_set_pos(sMapCard, M, MAP_Y);
    lv_obj_set_style_radius(sMapCard, 18, 0);
    lv_obj_set_style_clip_corner(sMapCard, true, 0);
    if (!sMapBuf) sMapBuf = (uint16_t*)heap_caps_malloc(MAP_W * MAP_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (sMapBuf) memset(sMapBuf, 0, MAP_W * MAP_H * sizeof(uint16_t));
    sMapDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    sMapDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    sMapDsc.header.flags = 0;
    sMapDsc.header.w = MAP_W;
    sMapDsc.header.h = MAP_H;
    sMapDsc.header.stride = MAP_W * 2;
    sMapDsc.data_size = MAP_W * MAP_H * 2;
    sMapDsc.data = (const uint8_t*)sMapBuf;
    sMapImg = lv_image_create(sMapCard);
    if (sMapBuf) lv_image_set_src(sMapImg, &sMapDsc);
    lv_obj_set_pos(sMapImg, 0, 0);

    lv_obj_add_flag(sMapCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sMapCard, mapTouch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sMapCard, mapTouch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(sMapCard, mapTouch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(sMapCard, mapTouch, LV_EVENT_PRESS_LOST, NULL);

    // HUD: source pill (top-left), route pill (top-centre), zoom (top-right), scale (bottom-left)
    sSrcPill = pill(sMapCard, "KEINE POSITION", cCrit());
    lv_obj_set_pos(sSrcPill, 8, 8);
    lv_obj_set_style_bg_opa(sSrcPill, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(sSrcPill, cBg(), 0);
    sRoutePill = pill(sMapCard, "LUFTLINIE", cWarn());
    lv_obj_set_style_bg_opa(sRoutePill, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(sRoutePill, cBg(), 0);
    lv_obj_align(sRoutePill, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(sRoutePill, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* zIn = hudButton(sMapCard, MAP_W - 44, 8, 36, 36, "+");
    lv_obj_add_event_cb(zIn, [](lv_event_t*) {
        if (sZoom < 16) sZoom++;
        sAutoZoom = false;
        sManualInteractMs = millis();
        sMapNeedsRedraw = true;
        haptic(HAPTIC_TICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* zOut = hudButton(sMapCard, MAP_W - 44, 50, 36, 36, "-");
    lv_obj_add_event_cb(zOut, [](lv_event_t*) {
        if (sZoom > 6) sZoom--;
        sAutoZoom = false;
        sManualInteractMs = millis();
        sMapNeedsRedraw = true;
        haptic(HAPTIC_TICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    sZoomBadge = hudButton(sMapCard, MAP_W - 56, 92, 48, 24, "A Z15");
    sZoomLbl = lv_obj_get_child(sZoomBadge, 0);
    lv_obj_set_style_text_font(sZoomLbl, F.cap, 0);
    lv_obj_add_event_cb(sZoomBadge, [](lv_event_t*) {
        sAutoZoom = true;
        sManualPan = false;
        sMapNeedsRedraw = true;
        haptic(HAPTIC_TICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    sScaleLbl = caption(sMapCard, "100 m", cText());
    lv_obj_align(sScaleLbl, LV_ALIGN_BOTTOM_LEFT, 10, -16);
    sScaleBar = box(sMapCard, 60, 3);
    lv_obj_set_style_bg_opa(sScaleBar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sScaleBar, cText(), 0);
    lv_obj_align(sScaleBar, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    sRecenterBtn = hudButton(sMapCard, MAP_W - 110, MAP_H - 42, 100, 32, "Zentrieren");
    lv_obj_add_event_cb(sRecenterBtn, [](lv_event_t*) {
        sManualPan = false;
        sAutoZoom = true;
        sMapNeedsRedraw = true;
        haptic(HAPTIC_TICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(sRecenterBtn, LV_OBJ_FLAG_HIDDEN);

    // honest empty state when no position is known at all
    sNoPosOverlay = box(sMapCard, MAP_W, MAP_H);
    lv_obj_set_pos(sNoPosOverlay, 0, 0);
    lv_obj_set_style_bg_opa(sNoPosOverlay, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(sNoPosOverlay, cBg(), 0);
    lv_obj_add_flag(sNoPosOverlay, LV_OBJ_FLAG_CLICKABLE);     // swallow pans
    lv_obj_t* npIc = label(sNoPosOverlay, F.icoL, cText3(), ICO_GPS_OFF);
    lv_obj_align(npIc, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_t* npT = label(sNoPosOverlay, F.body, cText2(), "Keine Position");
    lv_obj_align(npT, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_t* npH = caption(sNoPosOverlay, "GPS EINSCHALTEN FÜR DIE KARTE", cText3());
    lv_obj_align(npH, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_t* npBtn = primaryButton(sNoPosOverlay, 200, 44, ICO_SATELLITE_ALT, "GPS einschalten");
    lv_obj_align(npBtn, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_add_event_cb(npBtn, [](lv_event_t*) {
        if (!GpsManager::isEnabled()) GpsManager::setEnabled(true);
        GpsManager::setLive(true);
        GpsManager::requestFix();
        haptic(HAPTIC_CLICK);
        DisplayEngine::showToast("GPS", "Satellitensuche läuft", 2000, ICO_SATELLITE_ALT);
        nav::markDirty(nav::PG_CONTROL);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    // ------------------------------------------------------------ info card
    sFreeBox = card(p, CW, INFO_H);
    lv_obj_set_pos(sFreeBox, M, INFO_Y);
    sCoordLbl = label(sFreeBox, F.small, cText(), "");
    lv_obj_set_pos(sCoordLbl, 16, 12);
    sMetaLbl = caption(sFreeBox, "", cText2());
    lv_obj_set_pos(sMetaLbl, 16, 42);
    sMapLbl = caption(sFreeBox, "", accent());
    accReg(sMapLbl, ACC_TEXT);
    lv_label_set_long_mode(sMapLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sMapLbl, CW - 32);
    lv_obj_set_pos(sMapLbl, 16, 68);

    // ------------------------------------------------------------ guidance card
    sNavBox = card(p, CW, INFO_H);
    lv_obj_set_pos(sNavBox, M, INFO_Y);
    sTurnIcon = label(sNavBox, F.icoL, accent(), ICO_NAVIGATION);
    accReg(sTurnIcon, ACC_TEXT);
    lv_obj_set_pos(sTurnIcon, 14, 12);
    sTurnCmd = label(sNavBox, F.body, cText(), "");
    lv_label_set_long_mode(sTurnCmd, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sTurnCmd, CW - 170);
    lv_obj_set_pos(sTurnCmd, 64, 10);
    sTurnDest = caption(sNavBox, "", accent());
    accReg(sTurnDest, ACC_TEXT);
    lv_label_set_long_mode(sTurnDest, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sTurnDest, CW - 170);
    lv_obj_set_pos(sTurnDest, 64, 44);
    sTurnMeta = caption(sNavBox, "", cText2());
    lv_obj_set_pos(sTurnMeta, 16, 74);
    sMeshSendBtn = secondaryButton(sNavBox, 92, 40, ICO_PODCASTS, "Mesh");
    lv_obj_align(sMeshSendBtn, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_add_event_cb(sMeshSendBtn, [](lv_event_t*) {
        NavStatus st = NavEngine::status();
        bool ok = RadioLoRa::sendWaypoint(st.destName, st.destLat, st.destLon);
        haptic(ok ? HAPTIC_SUCCESS : HAPTIC_TICK);
        DisplayEngine::showToast("Mesh", ok ? "Ziel als Wegpunkt gesendet" : "LoRa ist aus", 2200, ICO_PODCASTS);
    }, LV_EVENT_CLICKED, NULL);
    setHidden(sNavBox, true);

    // ------------------------------------------------------------ action bar
    sDestBtn = primaryButton(p, 80, BAR_H, nullptr, "Ziel");
    lv_obj_set_pos(sDestBtn, M_BOT, BAR_Y);
    lv_obj_add_event_cb(sDestBtn, [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        if (NavEngine::isNavigating() || NavEngine::isRoutingBusy()) {
            NavEngine::stopNavigation();
            sMapNeedsRedraw = true;
            refresh(true);
        } else {
            openSearch(sActiveFilter == 1 ? 0 : sActiveFilter);
        }
    }, LV_EVENT_CLICKED, NULL);

    static const char* MODES[3] = {"AUTO", "FUSS", "MESH"};
    sModeTabs = tabBar(p, M_BOT + 86, BAR_Y + 8, 132, MODES, 3, 0, [](lv_event_t* e) {
        NavMode m = (NavMode)(int)(intptr_t)lv_event_get_user_data(e);
        NavEngine::setMode(m);
        sMapNeedsRedraw = true;
        haptic(HAPTIC_TICK);
        if (m == NAV_MODE_MESH && !NavEngine::isNavigating()) openSearch(1);
        refresh(true);
    });

    sRecBtn = secondaryButton(p, 46, BAR_H, nullptr, "GPX");
    lv_obj_set_pos(sRecBtn, M_BOT + 224, BAR_Y);
    lv_obj_add_event_cb(sRecBtn, [](lv_event_t*) {
        bool rec = !NavEngine::isTrackRecording();
        NavEngine::setTrackRecording(rec);
        haptic(HAPTIC_TICK);
        DisplayEngine::showToast("GPX-Aufzeichnung", rec ? "Gestartet (nur mit GPS-Fix)" : "Gestoppt", 2000, ICO_ROUTE);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    sWpBtn = secondaryButton(p, 44, BAR_H, ICO_FLAG, nullptr);
    lv_obj_set_pos(sWpBtn, M_BOT + 274, BAR_Y);
    lv_obj_add_event_cb(sWpBtn, [](lv_event_t*) {
        bool ok = NavEngine::markWaypoint("WP");
        haptic(ok ? HAPTIC_SUCCESS : HAPTIC_TICK);
        DisplayEngine::showToast("Wegpunkt", ok ? "Position gespeichert" : "Keine Position bekannt", 2000, ICO_FLAG);
        sMapNeedsRedraw = true;
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    // ------------------------------------------------------------ search modal
    sSearchModal = box(p, SCR_W, SCR_H);
    lv_obj_set_pos(sSearchModal, 0, 0);
    lv_obj_set_style_bg_color(sSearchModal, cBg(), 0);
    lv_obj_set_style_bg_opa(sSearchModal, LV_OPA_COVER, 0);
    lv_obj_add_flag(sSearchModal, LV_OBJ_FLAG_CLICKABLE);
    title(sSearchModal, "ZIEL");
    lv_obj_t* closeBtn = secondaryButton(sSearchModal, 84, 32, ICO_CLOSE, "Zurück");
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -M_HEAD, 26);
    lv_obj_add_event_cb(closeBtn, [](lv_event_t*) { setHidden(sSearchModal, true); haptic(HAPTIC_TICK); }, LV_EVENT_CLICKED, NULL);
    sFilterTabs = tabBar(sSearchModal, M, CONTENT_Y, CW, FILTER_LABELS, 7, 0, [](lv_event_t* e) {
        sActiveFilter = (int)(intptr_t)lv_event_get_user_data(e);
        haptic(HAPTIC_TICK);
        populateSearchList();
    });
    sSearchList = lv_obj_create(sSearchModal);
    lv_obj_set_size(sSearchList, CW, SCR_H - CONTENT_Y - 44);
    lv_obj_set_pos(sSearchList, M, CONTENT_Y + 40);
    lv_obj_set_style_bg_opa(sSearchList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sSearchList, 0, 0);
    lv_obj_set_style_pad_all(sSearchList, 0, 0);
    lv_obj_set_style_pad_bottom(sSearchList, 24, 0);
    lv_obj_set_scrollbar_mode(sSearchList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(sSearchList, LV_DIR_VER);
    lv_obj_set_flex_flow(sSearchList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sSearchList, 8, 0);
    setHidden(sSearchModal, true);

    // ------------------------------------------------------------ skyplot modal
    sSkyModal = box(p, SCR_W, SCR_H);
    lv_obj_set_pos(sSkyModal, 0, 0);
    lv_obj_set_style_bg_color(sSkyModal, cBg(), 0);
    lv_obj_set_style_bg_opa(sSkyModal, LV_OPA_COVER, 0);
    lv_obj_add_flag(sSkyModal, LV_OBJ_FLAG_CLICKABLE);
    title(sSkyModal, "SATELLITEN");
    lv_obj_t* skyClose = secondaryButton(sSkyModal, 84, 32, ICO_CLOSE, "Zurück");
    lv_obj_align(skyClose, LV_ALIGN_TOP_RIGHT, -M_HEAD, 26);
    lv_obj_add_event_cb(skyClose, [](lv_event_t*) { setHidden(sSkyModal, true); haptic(HAPTIC_TICK); }, LV_EVENT_CLICKED, NULL);
    sSkySub = caption(sSkyModal, "", accent());
    accReg(sSkySub, ACC_TEXT);
    lv_obj_set_pos(sSkySub, M, CONTENT_Y);

    lv_obj_t* radarCard = card(sSkyModal, 212, 224);
    lv_obj_set_pos(radarCard, M, CONTENT_Y + 28);
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
        lv_obj_center(caption(d, "1", cBg()));
        setHidden(d, true);
        sSatDots[i] = d;
    }
    lv_obj_t* legend = card(sSkyModal, CW - 212 - 8, 224);
    lv_obj_set_pos(legend, M + 220, CONTENT_Y + 28);
    lv_obj_t* lg = caption(legend, "GRÜN > 25 dB\nAKZENT > 15 dB\nORANGE schwach\n\nAUSSEN = HORIZONT\nMITTE = ZENIT", cText2());
    lv_obj_set_pos(lg, 12, 12);

    lv_obj_t* snrCard = card(sSkyModal, CW, 150);
    lv_obj_set_pos(snrCard, M, CONTENT_Y + 260);
    lv_obj_t* snrHdr = caption(snrCard, "SIGNALSTÄRKE (C/N0 dB-Hz)", cText2());
    lv_obj_set_pos(snrHdr, 14, 8);
    for (int i = 0; i < 8; i++) {
        int bx = 18 + i * 44;
        lv_obj_t* bar = box(snrCard, 26, 10);
        lv_obj_set_pos(bar, bx, 96);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_bg_color(bar, accent(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        sSatSnrBars[i] = bar;
        sSatSnrLbls[i] = caption(snrCard, "--", cText2());
        lv_obj_set_pos(sSatSnrLbls[i], bx - 2, 112);
    }
    setHidden(sSkyModal, true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 0;
    d.allowHorizontalGesture = allowHorizontal;
    d.onShow = [] {
        GpsManager::setLive(true);                 // PowerManager stops it on sleep; we re-arm on show
        sManualPan = false;
        sAutoZoom = true;
        sMapNeedsRedraw = true;
        sLastRefreshMs = millis();
    };
    d.onHide = [] {
        GpsManager::setLive(false);
        if (sSearchModal) setHidden(sSearchModal, true);
        if (sSkyModal) setHidden(sSkyModal, true);
        nav::setRefreshInterval(nav::PG_POSITION, 1000);
        sDragging = false;
    };
    nav::registerPage(nav::PG_POSITION, d);
}

}  // namespace pages
