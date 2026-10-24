// Position: GNSS status, coordinates and fix quality. GNSS runs continuously
// while this page is visible (onShow/onHide).
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "GpsManager.h"
#include <math.h>

using namespace ui;

namespace pages {

static lv_obj_t *pPill, *pPillTxt, *pLat, *pLon, *pVal[6];

static void refresh(bool) {
    char b[48];
    bool online = GpsManager::isOnline(), fix = GpsManager::hasFix(), pw = GpsManager::isPowered();
    lv_color_t pc = !online ? cCrit() : fix ? cOk() : pw ? cWarn() : cText3();
    setBgColorIf(pPill, pc);
    setTextColorIf(pPillTxt, pc);
    setText(pPillTxt, !online ? "NO GNSS" : fix ? "3D FIX" : pw ? "SEARCHING" : "STANDBY");
    if (GpsManager::hasLastFix()) {
        double la = GpsManager::latitude(), lo = GpsManager::longitude();
        snprintf(b, sizeof(b), "%.5f° %c", fabs(la), la >= 0 ? 'N' : 'S'); setText(pLat, b);
        snprintf(b, sizeof(b), "%.5f° %c", fabs(lo), lo >= 0 ? 'E' : 'W'); setText(pLon, b);
        setTextColorIf(pLat, fix ? cText() : cText2());
        setTextColorIf(pLon, fix ? cText() : cText2());
    } else {
        setText(pLat, "No position yet");
        setText(pLon, pw ? "Looking for satellites..." : "");
    }
    snprintf(b, sizeof(b), "%u", GpsManager::satellites());          setText(pVal[0], b);
    snprintf(b, sizeof(b), "%.0f m", GpsManager::altitudeM());       setText(pVal[1], b);
    snprintf(b, sizeof(b), "%.1f km/h", GpsManager::speedKmh());     setText(pVal[2], b);
    if (fix) snprintf(b, sizeof(b), "~%.0f m", GpsManager::hdop() * 3.0f);
    else snprintf(b, sizeof(b), "--");
    setText(pVal[3], b);
    ago(b, sizeof(b), GpsManager::lastFixEpoch());                     setText(pVal[4], b);
    ago(b, sizeof(b), GpsManager::lastTimeSyncEpoch());                setText(pVal[5], b);
}

void buildPosition(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "POSITION");
    pPill = box(p, 124, 30);
    lv_obj_align(pPill, LV_ALIGN_TOP_RIGHT, -M, 26);
    lv_obj_set_style_radius(pPill, 15, 0);
    lv_obj_set_style_bg_opa(pPill, LV_OPA_30, 0);
    pPillTxt = caption(pPill, "", cText2());
    lv_obj_center(pPillTxt);

    pLat = label(p, F.title, cText(), "");
    lv_obj_set_pos(pLat, M, 88);
    pLon = label(p, F.title, cText(), "");
    lv_obj_set_pos(pLon, M, 126);

    lv_obj_t* c = kvCard(p, 178, 6);
    static const char* keys[6] = {"SATELLITES", "ALTITUDE", "SPEED", "ACCURACY", "LAST FIX", "TIME SYNC"};
    for (int i = 0; i < 6; i++) pVal[i] = kvRow(c, keys[i], i == 5);
    lv_obj_t* hint = caption(p, "GNSS ACTIVE WHILE THIS SCREEN IS OPEN", cText3());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    d.onShow = [] { GpsManager::setLive(true); };
    d.onHide = [] { GpsManager::setLive(false); };
    nav::registerPage(nav::PG_POSITION, d);
}

}  // namespace pages
