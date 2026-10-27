// ============================================================================
//  Watch faces:
//   0: MODULAR (digital hero time + 3 complications)
//   1: CHRONO (analog chronometer dial + sweeping hands)
//   2: MINIMAL (stacked oversized numerals)
//   3: ULTRA WAYFINDER (compass & adventure dial, real GNSS / heading data)
//   4: SKELETON TOURBILLON (skeleton dial + animated balance)
//   5: CASIO (retro LCD)
//   6: BAUHAUS (minimalist analog dial)
//   7: CYBERPUNK (HUD terminal with real system data)
//   8: WORDCLOCK (German typographic word clock)
//   9: NEON HORIZON (synthwave sunset grid)
//  10: PILOT FLIEGER (B-Uhr aviator dial)
//  11: DIVER PRO (diver dial)
//  12: RADAR TACTICAL (sonar scope, blips = real LoRa peers)
//  13: LIFETIME (year / month / week / day progress bars)
//
//  All faces live in one page; only the selected face container is visible.
//  A shared notification badge (unread count) sits above every face.
// ============================================================================
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "MotionHaptic.h"
#include "RadioLoRa.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
#include "NotificationStore.h"
#include "GpsManager.h"
#include "NavEngine.h"
#include "DisplayEngine.h"
#include "Config.h"
#include <sys/time.h>
#include <math.h>

using namespace ui;

namespace pages {

static lv_obj_t* sRoot;
static lv_obj_t* sFaces[FACE_COUNT];
static lv_obj_t *sBadge, *sBadgeTxt;
static int sShown = -1;

static const char* FACE_NAMES[FACE_COUNT] = {
    "Modular", "Chronometer", "Minimal", "Wayfinder Ultra",
    "Tourbillon", "Casio Vintage", "Bauhaus", "Cyberpunk",
    "Wortuhr", "Neon Horizon", "Pilot Flieger", "Diver Pro",
    "Radar Tactical", "Lifetime"
};
static const char* FACE_DESC[FACE_COUNT] = {
    "Digitale Zeit mit Schritten, Akku und Tag",
    "Analoges Chronometer mit Sekundenzeiger",
    "Grosse Ziffern, sonst nichts",
    "Kompass, Koordinaten und Kurs vom Sensor",
    "Skelettiertes Werk mit laufender Unruh",
    "Retro-LCD im Stil der 80er",
    "Reduziertes Bauhaus-Zifferblatt",
    "Terminal-HUD mit echten Systemdaten",
    "Die Zeit in Worten, auf Deutsch",
    "Synthwave-Sonnenuntergang mit Datum",
    "Fliegeruhr nach B-Uhr-Vorbild",
    "Taucheruhr mit Leuchtmarken",
    "Sonar-Scope, Punkte sind LoRa-Knoten",
    "Fortschritt von Jahr, Monat, Woche, Tag"
};

const char* faceName(int idx) {
    idx = ((idx % FACE_COUNT) + FACE_COUNT) % FACE_COUNT;
    return FACE_NAMES[idx];
}
const char* faceDescription(int idx) {
    idx = ((idx % FACE_COUNT) + FACE_COUNT) % FACE_COUNT;
    return FACE_DESC[idx];
}

// ---------------------------------------------------------------- hands
// Update a 2-point line hand. pts must have static storage (LVGL keeps the pointer).
static void updateLineHand(lv_obj_t* line, lv_point_precise_t pts[2], float cx, float cy, float deg, float len,
                           float tail, float pad = 6.0f) {
    float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    float c = cosf(a), s = sinf(a);
    float x1 = cx + c * len, y1 = cy + s * len;
    float x0 = cx - c * tail, y0 = cy - s * tail;
    int bx = (int)floorf(fminf(x0, x1) - pad), by = (int)floorf(fminf(y0, y1) - pad);
    pts[0] = {(lv_value_precise_t)(x0 - bx), (lv_value_precise_t)(y0 - by)};
    pts[1] = {(lv_value_precise_t)(x1 - bx), (lv_value_precise_t)(y1 - by)};
    lv_obj_set_pos(line, bx, by);
    lv_obj_set_size(line, (int)ceilf(fabsf(x1 - x0) + 2 * pad), (int)ceilf(fabsf(y1 - y0) + 2 * pad));
    lv_line_set_points(line, pts, 2);
}

// Hour / minute / second hand set of an analog dial (static point storage inside)
struct AnalogHands {
    lv_obj_t *hour = nullptr, *min = nullptr, *sec = nullptr;
    lv_point_precise_t pts[3][2];
    float cx = 0, cy = 0;
};

static lv_obj_t* makeHandLine(lv_obj_t* dial, int w, lv_color_t col, lv_point_precise_t pts[2], float cx, float cy) {
    lv_obj_t* l = lv_line_create(dial);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_color(l, col, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    pts[0] = {(lv_value_precise_t)cx, (lv_value_precise_t)cy};
    pts[1] = {(lv_value_precise_t)cx, (lv_value_precise_t)cy};
    lv_line_set_points(l, pts, 2);
    return l;
}

static void buildHands(AnalogHands& h, lv_obj_t* dial, float cx, float cy, int wH, int wM, int wS,
                       lv_color_t cH, lv_color_t cM, lv_color_t cS) {
    h.cx = cx;
    h.cy = cy;
    h.hour = makeHandLine(dial, wH, cH, h.pts[0], cx, cy);
    h.min  = makeHandLine(dial, wM, cM, h.pts[1], cx, cy);
    h.sec  = makeHandLine(dial, wS, cS, h.pts[2], cx, cy);
}

static void updateHands(AnalogHands& h, float hr, float mn, float sc, int lenH, int tailH, int lenM, int tailM,
                        int lenS, int tailS) {
    updateLineHand(h.hour, h.pts[0], h.cx, h.cy, hr * 30.0f, lenH, tailH);
    updateLineHand(h.min,  h.pts[1], h.cx, h.cy, mn * 6.0f, lenM, tailM);
    updateLineHand(h.sec,  h.pts[2], h.cx, h.cy, sc * 6.0f, lenS, tailS);
}

static lv_obj_t* centreCap(lv_obj_t* dial, int size, lv_color_t col) {
    lv_obj_t* cap = box(dial, size, size);
    lv_obj_center(cap);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cap, col, 0);
    return cap;
}

static lv_obj_t* dateWindow(lv_obj_t* dial, int x, int y, int w, int h, lv_obj_t** lbl) {
    lv_obj_t* db = card(dial, w, h);
    lv_obj_set_pos(db, x, y);
    lv_obj_set_style_radius(db, 6, 0);
    *lbl = label(db, F.small, cText(), "--");
    lv_obj_center(*lbl);
    return db;
}

static const char* cardinalDe(float deg) {
    static const char* D[8] = {"N", "NO", "O", "SO", "S", "SW", "W", "NW"};
    int i = ((int)lroundf(deg / 45.0f)) & 7;
    return D[i];
}

// ---------------------------------------------------------------- WIDGET DEFS
// 0: Modular
static lv_obj_t *mStatus, *mWifi, *mBle, *mLora, *mGps, *mBatIcon, *mBatTxt;
static lv_obj_t *mHH, *mMM, *mSecBar, *mDate, *mEvent;
static lv_obj_t *mRing[3], *mRingVal[3], *mDetail[3];

// 1: Chrono
static lv_obj_t *cDial, *cDate, *cDigital;
static AnalogHands cHands;
static const int C_D = 372, C_R = C_D / 2;

// 2: Minimal
static lv_obj_t *nDate, *nHH, *nMM, *nBat;

// 3: Ultra Wayfinder
static lv_obj_t *wHH, *wMM, *wSec, *wDate, *wCoord, *wAlt, *wHeading;
static lv_obj_t *wRingSteps, *wStepsTxt, *wRingBat, *wBatTxt;

// 4: Skeleton Tourbillon
static lv_obj_t *tDial, *tDate, *tPowerArc, *tPowerTxt;
static lv_obj_t *tTourbArm1, *tTourbArm2;
static lv_point_precise_t tArmPts[2][2];
static AnalogHands tHands;
static const int T_SZ = 376, TB_SZ = 110;

// 5: Casio
static lv_obj_t *kDayBoxes[7], *kDayLabels[7], *kDate, *kBarDots[10];
static lv_obj_t *kHH, *kMM, *kSS, *kBat;

// 6: Bauhaus
static lv_obj_t *bDial, *bDate;
static lv_point_precise_t bBatonPts[12][2];     // static: lv_line keeps the pointer
static AnalogHands bHands;
static const int B_SZ = 380;

// 7: Cyberpunk
static lv_obj_t *yHeader, *yHero, *ySec, *yStatus, *yDiag, *yBat;

// 8: WordClock
static lv_obj_t *wcWords[21];
static lv_obj_t *wcDots[4];

// 9: Neon Horizon
static lv_obj_t *nhHH, *nhMM, *nhSec, *nhDate, *nhBat;

// 10: Pilot Flieger
static lv_obj_t *pfDial, *pfDate;
static AnalogHands pfHands;
static const int PF_SZ = 380;

// 11: Diver Pro
static lv_obj_t *dpDial, *dpDate;
static AnalogHands dpHands;
static const int DP_SZ = 380;

// 12: Radar Tactical
static lv_obj_t *rtScope, *rtSweep, *rtBlip[3], *rtHeading, *rtTime;
static lv_point_precise_t rtPts[2];
static const int RT_SZ = 370;

// 13: Lifetime
static lv_obj_t *ltBars[4], *ltPct[4], *ltTime, *ltDate;

static void longPress(lv_event_t*) {
    faceNext();
    haptic(HAPTIC_DOUBLE_CLICK);
}

static lv_obj_t* faceRoot() {
    lv_obj_t* f = box(sRoot, SCR_W, SCR_H);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(f, longPress, LV_EVENT_LONG_PRESSED, NULL);
    return f;
}

// ============================================================================
// 0: MODULAR
// ============================================================================
static void buildModular(lv_obj_t* f) {
    mStatus = caption(f, "", accent());
    accReg(mStatus, ACC_TEXT);
    lv_obj_set_pos(mStatus, M_HEAD, 26);

    lv_obj_t* st = box(f, LV_SIZE_CONTENT, 28);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -M_HEAD, 24);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 8, 0);
    mWifi = label(st, F.icoS, cText3(), ICO_WIFI);
    mBle  = label(st, F.icoS, cText3(), ICO_SMARTPHONE);
    mGps  = label(st, F.icoS, cText3(), ICO_SATELLITE_ALT);
    mLora = label(st, F.icoS, cText3(), ICO_PODCASTS);
    mBatIcon = label(st, F.icoS, cText2(), ICO_BATTERY_FULL);
    mBatTxt = label(st, F.small, cText2(), "--%");

    lv_obj_t* hero = box(f, SCR_W, 150);
    lv_obj_set_pos(hero, 0, 56);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 4, 0);
    mHH = label(hero, F.hero, cText(), "00");
    lv_obj_t* colon = label(hero, F.hero, accent(), ":");
    accReg(colon, ACC_TEXT);
    mMM = label(hero, F.hero, cText(), "00");

    mSecBar = lv_bar_create(f);
    lv_obj_set_size(mSecBar, 300, 4);
    lv_obj_align(mSecBar, LV_ALIGN_TOP_MID, 0, 214);
    lv_bar_set_range(mSecBar, 0, 59999);
    lv_obj_set_style_bg_color(mSecBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mSecBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(mSecBar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(mSecBar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(mSecBar, accent(), LV_PART_INDICATOR);
    accReg(mSecBar, ACC_BG, LV_PART_INDICATOR);

    mDate = label(f, F.body, cText2(), "");
    lv_obj_set_style_text_letter_space(mDate, 1, 0);
    lv_obj_align(mDate, LV_ALIGN_TOP_MID, 0, 230);

    mEvent = caption(f, "", cText3());              // next calendar event (if synced)
    lv_obj_align(mEvent, LV_ALIGN_TOP_MID, 0, 264);

    static const char* caps[3] = {"SCHRITTE", "AKKU", "TAG"};
    const int startX = 36;
    const int spanW = SCR_W - 2 * startX; // 338 px
    const int cw = (spanW - 16) / 3;      // 107 px
    const int ch = 156, cy = 296;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = card(f, cw, ch);
        lv_obj_set_style_radius(c, R_TILE, 0);
        lv_obj_set_pos(c, startX + i * (cw + 8), cy);
        lv_color_t col = (i == 1) ? cOk() : (i == 2 ? cInfo() : accent());
        mRing[i] = ring(c, 74, 7, col);
        if (i == 0) accReg(mRing[i], ACC_ARC, LV_PART_INDICATOR);
        lv_obj_align(mRing[i], LV_ALIGN_TOP_MID, 0, 12);
        mRingVal[i] = label(mRing[i], F.numS, cText(), "0");
        lv_obj_align(mRingVal[i], LV_ALIGN_CENTER, 0, 0);
        mDetail[i] = label(c, F.small, cText(), "");
        lv_obj_align(mDetail[i], LV_ALIGN_TOP_MID, 0, 94);
        lv_obj_t* cp = caption(c, caps[i], cText3());
        lv_obj_align(cp, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
}

// ============================================================================
// 1: CHRONO
// ============================================================================
static void buildChrono(lv_obj_t* f) {
    lv_obj_t* bezel = box(f, C_D, C_D);
    lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bezel, cSurf(), 0);
    lv_obj_set_style_bg_opa(bezel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bezel, 1, 0);
    lv_obj_set_style_border_color(bezel, cStroke(), 0);

    lv_obj_t* sc = lv_scale_create(bezel);
    lv_obj_set_size(sc, C_D - 24, C_D - 24);
    lv_obj_center(sc);
    lv_scale_set_mode(sc, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(sc, 0, 60);
    lv_scale_set_angle_range(sc, 360);
    lv_scale_set_rotation(sc, 270);
    lv_scale_set_total_tick_count(sc, 61);
    lv_scale_set_major_tick_every(sc, 5);
    lv_scale_set_label_show(sc, true);
    static const char* hours[] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "", NULL};
    lv_scale_set_text_src(sc, hours);
    lv_obj_set_style_arc_width(sc, 0, LV_PART_MAIN);
    lv_obj_set_style_length(sc, 8, LV_PART_ITEMS);
    lv_obj_set_style_line_width(sc, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(sc, cText3(), LV_PART_ITEMS);
    lv_obj_set_style_length(sc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(sc, cText(), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(sc, F.small, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(sc, cText2(), LV_PART_INDICATOR);
    lv_obj_t* brand = caption(bezel, "WATCHOS", cText3());
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -70);

    lv_obj_update_layout(bezel);
    lv_draw_buf_t* baked = lv_snapshot_take(bezel, LV_COLOR_FORMAT_RGB565);
    lv_obj_delete(bezel);
    cDial = lv_image_create(f);
    if (baked) lv_image_set_src(cDial, baked);
    lv_obj_set_size(cDial, C_D, C_D);
    lv_obj_align(cDial, LV_ALIGN_TOP_MID, 0, 44);

    cDate = label(cDial, F.small, accent(), "");
    accReg(cDate, ACC_TEXT);
    lv_obj_set_style_text_letter_space(cDate, 2, 0);
    lv_obj_align(cDate, LV_ALIGN_CENTER, 0, 72);

    buildHands(cHands, cDial, C_R, C_R, 10, 6, 3, cText(), cText(), accent());
    accReg(cHands.sec, ACC_LINE);

    lv_obj_t* cap = centreCap(cDial, 22, accent());
    accReg(cap, ACC_BG);
    centreCap(cap, 8, cBg());

    cDigital = label(f, F.numS, cText2(), "");
    lv_obj_align(cDigital, LV_ALIGN_BOTTOM_MID, 0, -30);
}

// ============================================================================
// 2: MINIMAL
// ============================================================================
static void buildMinimal(lv_obj_t* f) {
    nDate = caption(f, "", cText2());
    lv_obj_set_style_text_letter_space(nDate, 4, 0);
    lv_obj_align(nDate, LV_ALIGN_TOP_MID, 0, 30);
    nHH = label(f, F.giant, cText(), "00");
    lv_obj_align(nHH, LV_ALIGN_TOP_MID, 0, 52);
    nMM = label(f, F.giant, accent(), "00");
    accReg(nMM, ACC_TEXT);
    lv_obj_align(nMM, LV_ALIGN_TOP_MID, 0, 254);
    nBat = caption(f, "", cText3());
    lv_obj_align(nBat, LV_ALIGN_BOTTOM_MID, 0, -22);
}

// ============================================================================
// 3: ULTRA WAYFINDER
// ============================================================================
static void buildUltraWayfinder(lv_obj_t* f) {
    const lv_color_t COL_ULTRA = lv_color_hex(0xFF5500);

    lv_obj_t* ringBox = box(f, 386, 386);
    lv_obj_align(ringBox, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_radius(ringBox, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ringBox, 2, 0);
    lv_obj_set_style_border_color(ringBox, cStroke(), 0);

    lv_obj_t* lblN = label(ringBox, F.title, COL_ULTRA, "N");
    lv_obj_align(lblN, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t* lblE = label(ringBox, F.title, cText2(), "O");
    lv_obj_align(lblE, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t* lblS = label(ringBox, F.title, cText2(), "S");
    lv_obj_align(lblS, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t* lblW = label(ringBox, F.title, cText2(), "W");
    lv_obj_align(lblW, LV_ALIGN_LEFT_MID, 8, 0);

    wCoord = caption(f, "KEIN FIX", cText2());
    lv_obj_align(wCoord, LV_ALIGN_TOP_MID, 0, 52);
    wAlt = caption(f, "GPS AUS", cText3());
    lv_obj_align(wAlt, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t* hero = box(f, SCR_W, 110);
    lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 4, 0);
    wHH = label(hero, F.hero, cText(), "00");
    label(hero, F.hero, COL_ULTRA, ":");
    wMM = label(hero, F.hero, cText(), "00");
    wSec = label(hero, F.numS, COL_ULTRA, "00");
    lv_obj_set_style_pad_left(wSec, 8, 0);

    wDate = label(f, F.title, COL_ULTRA, "");
    lv_obj_align(wDate, LV_ALIGN_TOP_MID, 0, 230);
    wHeading = caption(f, "KURS --", cText2());
    lv_obj_align(wHeading, LV_ALIGN_TOP_MID, 0, 265);

    const int startX = 32;
    const int spanW = SCR_W - 2 * startX;
    const int cw = (spanW - 14) / 2, ch = 136, cy = 310;
    lv_obj_t* cAct = card(f, cw, ch);
    lv_obj_set_style_radius(cAct, R_TILE, 0);
    lv_obj_set_pos(cAct, startX, cy);
    wRingSteps = ring(cAct, 72, 7, COL_ULTRA);
    lv_obj_align(wRingSteps, LV_ALIGN_TOP_MID, 0, 12);
    wStepsTxt = label(wRingSteps, F.numS, cText(), "0");
    lv_obj_align(wStepsTxt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t* capAct = caption(cAct, "SCHRITTE", cText3());
    lv_obj_align(capAct, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t* cBat = card(f, cw, ch);
    lv_obj_set_style_radius(cBat, R_TILE, 0);
    lv_obj_set_pos(cBat, startX + cw + 14, cy);
    wRingBat = ring(cBat, 72, 7, cOk());
    lv_obj_align(wRingBat, LV_ALIGN_TOP_MID, 0, 12);
    wBatTxt = label(wRingBat, F.numS, cText(), "0%");
    lv_obj_align(wBatTxt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t* capBat = caption(cBat, "AKKU", cText3());
    lv_obj_align(capBat, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
// 4: SKELETON TOURBILLON
// ============================================================================
static void buildSkeletonTourbillon(lv_obj_t* f) {
    const lv_color_t GOLD = lv_color_hex(0xDAA520);
    tDial = box(f, T_SZ, T_SZ);
    lv_obj_align(tDial, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(tDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tDial, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_bg_opa(tDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tDial, 2, 0);
    lv_obj_set_style_border_color(tDial, lv_color_hex(0x2A3245), 0);

    lv_obj_t* brand = caption(tDial, "WATCHOS ULTRA", GOLD);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_t* sub = caption(tDial, "TOURBILLON", cText3());
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 72);

    // battery as "power reserve" at 9 o'clock (honest: percent, not hours)
    tPowerArc = lv_arc_create(tDial);
    lv_obj_set_size(tPowerArc, 82, 82);
    lv_obj_set_pos(tPowerArc, 26, 140);
    lv_arc_set_rotation(tPowerArc, 135);
    lv_arc_set_bg_angles(tPowerArc, 0, 180);
    lv_arc_set_range(tPowerArc, 0, 100);
    lv_obj_set_style_arc_width(tPowerArc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(tPowerArc, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(tPowerArc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(tPowerArc, GOLD, LV_PART_INDICATOR);
    lv_obj_remove_style(tPowerArc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(tPowerArc, LV_OBJ_FLAG_CLICKABLE);
    tPowerTxt = label(tDial, F.cap, GOLD, "--%");
    lv_obj_set_pos(tPowerTxt, 50, 172);

    dateWindow(tDial, T_SZ - 76, 166, 48, 30, &tDate);

    const int TB_X = (T_SZ - TB_SZ) / 2, TB_Y = 230;
    lv_obj_t* tbWell = box(tDial, TB_SZ, TB_SZ);
    lv_obj_set_pos(tbWell, TB_X, TB_Y);
    lv_obj_set_style_radius(tbWell, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tbWell, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(tbWell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tbWell, 2, 0);
    lv_obj_set_style_border_color(tbWell, GOLD, 0);

    lv_obj_t* bridge = box(tbWell, TB_SZ - 16, 4);
    lv_obj_center(bridge);
    lv_obj_set_style_bg_color(bridge, lv_color_hex(0x7A8599), 0);
    lv_obj_set_style_bg_opa(bridge, LV_OPA_COVER, 0);

    tTourbArm1 = makeHandLine(tbWell, 3, GOLD, tArmPts[0], TB_SZ / 2, TB_SZ / 2);
    tTourbArm2 = makeHandLine(tbWell, 3, GOLD, tArmPts[1], TB_SZ / 2, TB_SZ / 2);

    buildHands(tHands, tDial, T_SZ / 2, T_SZ / 2, 8, 5, 2, cText(), cText(), GOLD);
    centreCap(tDial, 18, GOLD);

    lv_obj_t* swiss = caption(f, "AUTOMATIK • TOURBILLON", cText3());
    lv_obj_align(swiss, LV_ALIGN_BOTTOM_MID, 0, -26);
}

// ============================================================================
// 5: CASIO
// ============================================================================
static void buildCasio(lv_obj_t* f) {
    const lv_color_t LCD_BG  = lv_color_hex(0x9EAC8D);
    const lv_color_t LCD_INK = lv_color_hex(0x111611);
    const lv_color_t LCD_DIM = lv_color_hex(0x8D9B7C);

    lv_obj_t* cCase = card(f, CW, 420);
    lv_obj_align(cCase, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(cCase, lv_color_hex(0x15181C), 0);
    lv_obj_set_style_border_width(cCase, 2, 0);
    lv_obj_set_style_border_color(cCase, lv_color_hex(0x1E56A0), 0);

    lv_obj_t* casio = label(cCase, F.title, cText(), "CASIO");
    lv_obj_set_pos(casio, 20, 12);
    lv_obj_t* illum = label(cCase, F.small, lv_color_hex(0xF5A623), "ILLUMINATOR");
    lv_obj_align(illum, LV_ALIGN_TOP_RIGHT, -20, 14);

    lv_obj_t* lcd = box(cCase, CW - 36, 300);
    lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_radius(lcd, 16, 0);
    lv_obj_set_style_bg_color(lcd, LCD_BG, 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, LCD_INK, 0);

    static const char* dNames[7] = {"MO", "DI", "MI", "DO", "FR", "SA", "SO"};
    for (int i = 0; i < 7; i++) {
        kDayBoxes[i] = box(lcd, 28, 20);
        lv_obj_set_pos(kDayBoxes[i], 16 + i * 38, 14);
        lv_obj_set_style_radius(kDayBoxes[i], 4, 0);
        lv_obj_set_style_bg_opa(kDayBoxes[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(kDayBoxes[i], LCD_BG, 0);
        kDayLabels[i] = label(kDayBoxes[i], F.cap, LCD_DIM, dNames[i]);
        lv_obj_center(kDayLabels[i]);
    }

    lv_obj_t* k24h = card(lcd, 36, 22);                 // every face is 24 h: static badge
    lv_obj_set_pos(k24h, 20, 48);
    lv_obj_set_style_bg_color(k24h, LCD_INK, 0);
    lv_obj_t* l24 = label(k24h, F.cap, LCD_BG, "24H");
    lv_obj_center(l24);

    kDate = label(lcd, F.title, LCD_INK, "--.--");
    lv_obj_align(kDate, LV_ALIGN_TOP_RIGHT, -24, 48);

    for (int g = 0; g < 10; g++) {
        kBarDots[g] = box(lcd, 6, 6);
        lv_obj_set_pos(kBarDots[g], 16 + g * 10, 80);
        lv_obj_set_style_radius(kBarDots[g], 2, 0);
        lv_obj_set_style_bg_opa(kBarDots[g], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(kBarDots[g], LCD_DIM, 0);
    }

    lv_obj_t* tRow = box(lcd, CW - 60, 100);
    lv_obj_align(tRow, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_flex_flow(tRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    kHH = label(tRow, F.hero, LCD_INK, "00");
    label(tRow, F.hero, LCD_INK, ":");
    kMM = label(tRow, F.hero, LCD_INK, "00");
    kSS = label(tRow, F.numS, LCD_INK, "00");
    lv_obj_set_style_pad_left(kSS, 8, 0);

    lv_obj_t* wr = label(cCase, F.small, lv_color_hex(0xE53935), "WATER RESIST");
    lv_obj_set_pos(wr, 20, 375);
    kBat = label(cCase, F.small, cText2(), "AKKU --%");
    lv_obj_align(kBat, LV_ALIGN_TOP_RIGHT, -20, 375);
}

// ============================================================================
// 6: BAUHAUS
// ============================================================================
static void buildBauhaus(lv_obj_t* f) {
    const lv_color_t RED = lv_color_hex(0xE53935);
    bDial = box(f, B_SZ, B_SZ);
    lv_obj_align(bDial, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_radius(bDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bDial, lv_color_hex(0x12151B), 0);
    lv_obj_set_style_bg_opa(bDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bDial, 1, 0);
    lv_obj_set_style_border_color(bDial, cStroke(), 0);

    lv_obj_t* logo = caption(bDial, "BAUHAUS", cText3());
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 68);

    // hour batons: points live in bBatonPts (static), LVGL only stores the pointer
    for (int i = 0; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        float c = cosf(a), s = sinf(a);
        const float r1 = 155.0f, r2 = 172.0f;
        bBatonPts[i][0] = {(lv_value_precise_t)(B_SZ / 2 + c * r1), (lv_value_precise_t)(B_SZ / 2 + s * r1)};
        bBatonPts[i][1] = {(lv_value_precise_t)(B_SZ / 2 + c * r2), (lv_value_precise_t)(B_SZ / 2 + s * r2)};
        lv_obj_t* baton = lv_line_create(bDial);
        lv_obj_set_style_line_width(baton, (i % 3 == 0) ? 3 : 2, 0);
        lv_obj_set_style_line_color(baton, (i % 3 == 0) ? cText() : cText3(), 0);
        lv_line_set_points(baton, bBatonPts[i], 2);
        if (i % 3 == 0) {
            lv_obj_t* pip = box(bDial, 6, 6);
            lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(pip, RED, 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_pos(pip, (int)(B_SZ / 2 + c * 142.0f - 3), (int)(B_SZ / 2 + s * 142.0f - 3));
        }
    }

    dateWindow(bDial, B_SZ - 76, B_SZ / 2 - 14, 44, 28, &bDate);
    buildHands(bHands, bDial, B_SZ / 2, B_SZ / 2, 6, 4, 2, cText(), cText(), RED);
    centreCap(bDial, 12, RED);
}

// ============================================================================
// 7: CYBERPUNK
// ============================================================================
static void buildCyberpunk(lv_obj_t* f) {
    const lv_color_t CYBER_GREEN = lv_color_hex(0x00FF66);
    const lv_color_t CYBER_CYAN  = lv_color_hex(0x00E5FF);

    lv_obj_t* pan = card(f, CW, 450);
    lv_obj_align(pan, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t* bTL = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_set_pos(bTL, 24, 14);
    lv_obj_t* bTR = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_align(bTR, LV_ALIGN_TOP_RIGHT, -24, 14);
    lv_obj_t* bBL = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_align(bBL, LV_ALIGN_BOTTOM_LEFT, 24, -14);
    lv_obj_t* bBR = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_align(bBR, LV_ALIGN_BOTTOM_RIGHT, -24, -14);

    yHeader = label(pan, F.small, CYBER_GREEN, "// SYSTEM HUD • UHRZEIT");
    lv_obj_set_pos(yHeader, 24, 22);

    lv_obj_t* timeRow = box(pan, CW - 32, 90);
    lv_obj_align(timeRow, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_flex_flow(timeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timeRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(timeRow, F.hero, CYBER_CYAN, "[ ");
    yHero = label(timeRow, F.hero, cText(), "00:00");
    ySec = label(timeRow, F.title, CYBER_GREEN, ":00");
    label(timeRow, F.hero, CYBER_CYAN, " ]");

    yStatus = label(pan, F.body, CYBER_GREEN, "HEAP: -- KB • PSRAM: -- KB");
    lv_obj_set_pos(yStatus, 24, 210);
    yDiag = label(pan, F.body, CYBER_CYAN, "STEPS: 0 • MESH: AUS");
    lv_obj_set_pos(yDiag, 24, 250);
    yBat = label(pan, F.body, cText(), "PWR: --%");
    lv_obj_set_pos(yBat, 24, 290);

    lv_obj_t* termBot = caption(pan, "// ESP32-S3 • 2 CORES • TERMINAL READY", cText3());
    lv_obj_align(termBot, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// ============================================================================
// 8: WORDCLOCK
// ============================================================================
static void buildWordClock(lv_obj_t* f) {
    lv_obj_t* pan = card(f, CW, 450);
    lv_obj_align(pan, LV_ALIGN_TOP_MID, 0, 24);

    const int Y_BASE = 24, Y_STEP = 54;
    auto addW = [&](int idx, const char* txt, int x, int y) {
        wcWords[idx] = label(pan, F.title, cStroke(), txt);
        lv_obj_set_pos(wcWords[idx], x, y);
    };
    addW(0, "ES", 20, Y_BASE);
    addW(1, "IST", 78, Y_BASE);
    addW(2, "FÜNF", 240, Y_BASE);
    addW(3, "ZEHN", 20, Y_BASE + Y_STEP);
    addW(4, "ZWANZIG", 160, Y_BASE + Y_STEP);
    addW(5, "VIERTEL", 20, Y_BASE + Y_STEP * 2);
    addW(6, "VOR", 190, Y_BASE + Y_STEP * 2);
    addW(7, "NACH", 258, Y_BASE + Y_STEP * 2);
    addW(8, "HALB", 20, Y_BASE + Y_STEP * 3);
    addW(9, "ZWÖLF", 130, Y_BASE + Y_STEP * 3);
    addW(10, "ZWEI", 248, Y_BASE + Y_STEP * 3);
    addW(11, "EINS", 20, Y_BASE + Y_STEP * 4);       // becomes "EIN" at one o'clock sharp
    addW(12, "DREI", 134, Y_BASE + Y_STEP * 4);
    addW(13, "VIER", 248, Y_BASE + Y_STEP * 4);
    addW(14, "SECHS", 20, Y_BASE + Y_STEP * 5);
    addW(15, "ACHT", 140, Y_BASE + Y_STEP * 5);
    addW(16, "SIEBEN", 232, Y_BASE + Y_STEP * 5);
    addW(17, "NEUN", 20, Y_BASE + Y_STEP * 6);
    addW(18, "ZEHN", 120, Y_BASE + Y_STEP * 6);
    addW(19, "ELF", 200, Y_BASE + Y_STEP * 6);
    addW(20, "UHR", 268, Y_BASE + Y_STEP * 6);

    wcDots[0] = box(pan, 8, 8); lv_obj_set_pos(wcDots[0], 28, 18);
    wcDots[1] = box(pan, 8, 8); lv_obj_align(wcDots[1], LV_ALIGN_TOP_RIGHT, -28, 18);
    wcDots[2] = box(pan, 8, 8); lv_obj_align(wcDots[2], LV_ALIGN_BOTTOM_RIGHT, -28, -18);
    wcDots[3] = box(pan, 8, 8); lv_obj_align(wcDots[3], LV_ALIGN_BOTTOM_LEFT, 28, -18);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_radius(wcDots[i], 4, 0);
        lv_obj_set_style_bg_opa(wcDots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(wcDots[i], cStroke(), 0);
    }
}

// ============================================================================
// 9: NEON HORIZON
// ============================================================================
static void buildNeonHorizon(lv_obj_t* f) {
    const lv_color_t SYNTH_MAG = lv_color_hex(0xFF2BD6);
    const lv_color_t SYNTH_CYAN = lv_color_hex(0x00E5FF);
    const lv_color_t SYNTH_YEL = lv_color_hex(0xFFDD00);

    lv_obj_t* sun = box(f, 160, 160);
    lv_obj_align(sun, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun, SYNTH_MAG, 0);
    lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sun, 3, 0);
    lv_obj_set_style_border_color(sun, SYNTH_YEL, 0);

    lv_obj_t* hLine = box(f, SCR_W, 3);
    lv_obj_set_pos(hLine, 0, 230);
    lv_obj_set_style_bg_color(hLine, SYNTH_CYAN, 0);
    lv_obj_set_style_bg_opa(hLine, LV_OPA_COVER, 0);

    lv_obj_t* tRow = box(f, SCR_W, 90);
    lv_obj_align(tRow, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_flex_flow(tRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    nhHH = label(tRow, F.hero, cText(), "00");
    label(tRow, F.hero, SYNTH_CYAN, ":");
    nhMM = label(tRow, F.hero, cText(), "00");
    nhSec = label(tRow, F.title, SYNTH_YEL, "00");
    lv_obj_set_style_pad_left(nhSec, 8, 0);

    for (int y = 250; y < 460; y += 32) {
        lv_obj_t* gl = box(f, SCR_W, 1);
        lv_obj_set_pos(gl, 0, y);
        lv_obj_set_style_bg_color(gl, SYNTH_MAG, 0);
        lv_obj_set_style_bg_opa(gl, LV_OPA_COVER, 0);
    }

    nhDate = label(f, F.title, SYNTH_CYAN, "");
    lv_obj_align(nhDate, LV_ALIGN_BOTTOM_MID, 0, -40);
    nhBat = caption(f, "", cText2());
    lv_obj_align(nhBat, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// ============================================================================
// 10: PILOT FLIEGER
// ============================================================================
static void buildPilotFlieger(lv_obj_t* f) {
    const lv_color_t LUME = lv_color_hex(0x76FF03);
    pfDial = box(f, PF_SZ, PF_SZ);
    lv_obj_align(pfDial, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(pfDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pfDial, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(pfDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pfDial, 2, 0);
    lv_obj_set_style_border_color(pfDial, cStroke(), 0);

    lv_obj_t* m12A = box(pfDial, 18, 12);
    lv_obj_align(m12A, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_radius(m12A, 4, 0);
    lv_obj_set_style_bg_color(m12A, LUME, 0);
    lv_obj_set_style_bg_opa(m12A, LV_OPA_COVER, 0);
    for (int k = -1; k <= 1; k += 2) {
        lv_obj_t* dot = box(pfDial, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, LUME, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, 16 * k, 21);
    }

    static const char* mins[12] = {"", "05", "10", "15", "20", "25", "30", "35", "40", "45", "50", "55"};
    for (int i = 1; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* m = label(pfDial, F.small, cText(), mins[i]);
        lv_obj_set_pos(m, (int)(PF_SZ / 2 + cosf(a) * 148.0f - 10), (int)(PF_SZ / 2 + sinf(a) * 148.0f - 8));
    }

    lv_obj_t* inRing = box(pfDial, 180, 180);
    lv_obj_center(inRing);
    lv_obj_set_style_radius(inRing, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(inRing, 1, 0);
    lv_obj_set_style_border_color(inRing, cStroke(), 0);

    static const char* hrs[12] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
    for (int i = 0; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* h = label(pfDial, F.small, LUME, hrs[i]);
        lv_obj_set_pos(h, (int)(PF_SZ / 2 + cosf(a) * 70.0f - 8), (int)(PF_SZ / 2 + sinf(a) * 70.0f - 8));
    }

    dateWindow(pfDial, PF_SZ - 68, PF_SZ / 2 - 13, 44, 26, &pfDate);
    buildHands(pfHands, pfDial, PF_SZ / 2, PF_SZ / 2, 10, 6, 2, LUME, LUME, cText());
    centreCap(pfDial, 16, cText());
}

// ============================================================================
// 11: DIVER PRO
// ============================================================================
static void buildDiverPro(lv_obj_t* f) {
    const lv_color_t LUME = lv_color_hex(0x00E5FF);
    dpDial = box(f, DP_SZ, DP_SZ);
    lv_obj_align(dpDial, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(dpDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dpDial, lv_color_hex(0x060C14), 0);
    lv_obj_set_style_bg_opa(dpDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dpDial, 4, 0);
    lv_obj_set_style_border_color(dpDial, lv_color_hex(0x1F2937), 0);

    lv_obj_t* pip12 = box(dpDial, 22, 14);
    lv_obj_align(pip12, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_radius(pip12, 4, 0);
    lv_obj_set_style_bg_color(pip12, LUME, 0);
    lv_obj_set_style_bg_opa(pip12, LV_OPA_COVER, 0);

    for (int i = 0; i < 12; i++) {
        if (i == 0 || i == 3 || i == 6 || i == 9) continue;
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* dot = box(dpDial, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, LUME, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, (int)(DP_SZ / 2 + cosf(a) * 152.0f - 6), (int)(DP_SZ / 2 + sinf(a) * 152.0f - 6));
    }
    for (int k = 0; k < 2; k++) {                        // batons at 6 and 9
        float a = ((k ? 270.0f : 180.0f) - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* b = box(dpDial, 20, 8);
        lv_obj_set_style_bg_color(b, LUME, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_pos(b, (int)(DP_SZ / 2 + cosf(a) * 150.0f - 10), (int)(DP_SZ / 2 + sinf(a) * 150.0f - 4));
    }

    dateWindow(dpDial, DP_SZ - 70, DP_SZ / 2 - 14, 44, 28, &dpDate);

    lv_obj_t* brand = caption(dpDial, "WATCHOS DIVER", cText());
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_t* depth = caption(dpDial, "300 M", LUME);
    lv_obj_align(depth, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_t* cert = caption(dpDial, "PROFESSIONAL", cText3());
    lv_obj_align(cert, LV_ALIGN_TOP_MID, 0, 260);

    buildHands(dpHands, dpDial, DP_SZ / 2, DP_SZ / 2, 9, 5, 2, cText(), cText(), LUME);
    centreCap(dpDial, 14, cText());
}

// ============================================================================
// 12: RADAR TACTICAL
// ============================================================================
static void buildRadarTactical(lv_obj_t* f) {
    const lv_color_t RADAR_GRN = lv_color_hex(0x00FF44);
    const lv_color_t RADAR_DIM = lv_color_hex(0x0A3A1A);

    rtScope = box(f, RT_SZ, RT_SZ);
    lv_obj_align(rtScope, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(rtScope, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rtScope, lv_color_hex(0x020E06), 0);
    lv_obj_set_style_bg_opa(rtScope, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rtScope, 2, 0);
    lv_obj_set_style_border_color(rtScope, RADAR_GRN, 0);

    static const int rRad[3] = {100, 200, 300};        // ~1 km / ~10 km / ~100 km (log scale)
    for (int i = 0; i < 3; i++) {
        lv_obj_t* ringObj = box(rtScope, rRad[i], rRad[i]);
        lv_obj_center(ringObj);
        lv_obj_set_style_radius(ringObj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ringObj, 1, 0);
        lv_obj_set_style_border_color(ringObj, RADAR_DIM, 0);
    }
    lv_obj_t* chH = box(rtScope, RT_SZ - 16, 1); lv_obj_center(chH);
    lv_obj_set_style_bg_color(chH, RADAR_DIM, 0); lv_obj_set_style_bg_opa(chH, LV_OPA_COVER, 0);
    lv_obj_t* chV = box(rtScope, 1, RT_SZ - 16); lv_obj_center(chV);
    lv_obj_set_style_bg_color(chV, RADAR_DIM, 0); lv_obj_set_style_bg_opa(chV, LV_OPA_COVER, 0);

    rtSweep = makeHandLine(rtScope, 3, RADAR_GRN, rtPts, RT_SZ / 2, RT_SZ / 2);

    for (int i = 0; i < 3; i++) {
        rtBlip[i] = box(rtScope, 8, 8);
        lv_obj_set_style_radius(rtBlip[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(rtBlip[i], RADAR_GRN, 0);
        lv_obj_set_style_bg_opa(rtBlip[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(rtBlip[i], LV_OBJ_FLAG_HIDDEN);
    }

    rtHeading = label(f, F.title, RADAR_GRN, "SCAN // AZIMUT 000°");
    lv_obj_align(rtHeading, LV_ALIGN_TOP_MID, 0, 404);
    rtTime = label(f, F.small, cText(), "00:00:00 • KEINE KNOTEN");
    lv_obj_align(rtTime, LV_ALIGN_TOP_MID, 0, 439);
}

// ============================================================================
// 13: LIFETIME
// ============================================================================
static void buildLifetime(lv_obj_t* f) {
    ltTime = label(f, F.numL, cText(), "00:00");
    lv_obj_align(ltTime, LV_ALIGN_TOP_MID, 0, 28);
    ltDate = caption(f, "", accent());
    accReg(ltDate, ACC_TEXT);
    lv_obj_align(ltDate, LV_ALIGN_TOP_MID, 0, 92);

    static const char* labels[4] = {"JAHR", "MONAT", "WOCHE", "TAG"};
    static const lv_color_t colors[4] = {
        lv_color_hex(0xF5A623), lv_color_hex(0x00E5FF), lv_color_hex(0xBF5AF2), lv_color_hex(0x76FF03)
    };
    const int Y_START = 128, STEP = 86;
    for (int i = 0; i < 4; i++) {
        lv_obj_t* c = card(f, CW, 74);
        lv_obj_set_pos(c, M, Y_START + i * STEP);
        lv_obj_t* cap = caption(c, labels[i], cText2());
        lv_obj_set_pos(cap, 16, 12);
        ltPct[i] = label(c, F.small, colors[i], "0%");
        lv_obj_align(ltPct[i], LV_ALIGN_TOP_RIGHT, -16, 10);
        ltBars[i] = lv_bar_create(c);
        lv_obj_set_size(ltBars[i], CW - 32, 8);
        lv_obj_align(ltBars[i], LV_ALIGN_BOTTOM_MID, 0, -14);
        lv_bar_set_range(ltBars[i], 0, 100);
        lv_obj_set_style_bg_color(ltBars[i], cStroke(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ltBars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ltBars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ltBars[i], colors[i], LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ltBars[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ltBars[i], 4, LV_PART_INDICATOR);
    }
}

// ============================================================================
//  SHOW / REFRESH LOOP
// ============================================================================
static void showFace(int idx) {
    idx = ((idx % FACE_COUNT) + FACE_COUNT) % FACE_COUNT;
    if (idx == sShown) return;
    for (int i = 0; i < FACE_COUNT; i++) setHidden(sFaces[i], i != idx);
    sShown = idx;
    nav::markDirty(nav::PG_FACE);
}

static void refreshBadge() {
    uint8_t n = NotificationStore::unread();
    setHidden(sBadge, n == 0);
    if (n) {
        char b[16];
        snprintf(b, sizeof(b), "%u NEU", (unsigned)n);
        setText(sBadgeTxt, b);
    }
}

static void refresh(bool force) {
    showFace(SettingsStorage::get().watchfaceIndex);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t secs = tv.tv_sec;
    struct tm ti;
    localtime_r(&secs, &ti);
    int ms = (int)(tv.tv_usec / 1000);
    static int lastSec = -1;
    // once-per-second work; a forced refresh (boot warm-up, wake, page show)
    // fills every complication at once instead of over the next minute
    bool secTick = force || (ti.tm_sec != lastSec);
    lastSec = ti.tm_sec;

    char b[80];
    int bat = PowerManager::getBatteryPercent();
    bool chg = PowerManager::isCharging();

    float sec = ti.tm_sec + ms / 1000.0f;
    float min = ti.tm_min + sec / 60.0f;
    float hr  = (ti.tm_hour % 12) + min / 60.0f;

    if (secTick) refreshBadge();

    switch (sShown) {
        case FACE_MODULAR: {
            snprintf(b, sizeof(b), "%s %02d. %s", WD_DE_S[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12]);
            setText(mStatus, b);
            setTextColorIf(mWifi, CompanionServer::isApActive() ? cText2() : cText3());
            setTextColorIf(mBle, BleCompanion::isConnected() ? cText2() : cText3());
            setTextColorIf(mGps, GpsManager::hasFix() ? cText2() : cText3());
            setTextColorIf(mLora, RadioLoRa::isEnabled() && RadioLoRa::isReady() ? cText2() : cText3());
            setText(mBatIcon, batterySymbol(bat, chg));
            setTextColorIf(mBatIcon, batteryColor(bat, chg));
            snprintf(b, sizeof(b), "%d%%", bat);
            setText(mBatTxt, b);

            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(mHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(mMM, b);
            setBar(mSecBar, ti.tm_sec * 1000 + ms);
            snprintf(b, sizeof(b), "%s, %d. %s", WD_DE_L[ti.tm_wday % 7], ti.tm_mday, MO_DE_L[ti.tm_mon % 12]);
            setText(mDate, b);

            if (force || (secTick && ti.tm_sec % 30 == 0)) {  // next calendar event, twice a minute
                CalendarEvent ev;
                if (BleCompanion::getUpcomingEvents(&ev, 1) > 0) {
                    time_t st = (time_t)ev.startEpoch;
                    struct tm et;
                    localtime_r(&st, &et);
                    if (ev.allDay) snprintf(b, sizeof(b), "%02d.%02d. %s", et.tm_mday, et.tm_mon + 1, ev.title);
                    else snprintf(b, sizeof(b), "%02d:%02d %s", et.tm_hour, et.tm_min, ev.title);
                    setText(mEvent, b);
                } else {
                    setText(mEvent, "");
                }
            }

            uint32_t steps = MotionHaptic::getStepsToday();
            uint32_t goal = SettingsStorage::get().dailyStepGoal;
            if (!goal) goal = 1;
            float sf = (float)steps / goal;
            setArc(mRing[0], sf);
            snprintf(b, sizeof(b), "%d%%", (int)fminf(999, sf * 100)); setText(mRingVal[0], b);
            snprintf(b, sizeof(b), "%lu", (unsigned long)steps);       setText(mDetail[0], b);

            setArc(mRing[1], bat / 100.0f);
            setArcColorIf(mRing[1], batteryColor(bat, chg));
            snprintf(b, sizeof(b), "%d%%", bat); setText(mRingVal[1], b);
            if (chg) setText(mDetail[1], "LÄDT");
            else { snprintf(b, sizeof(b), "%.2f V", PowerManager::getBatteryVoltage()); setText(mDetail[1], b); }

            int secOfDay = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
            setArc(mRing[2], secOfDay / 86400.0f);
            snprintf(b, sizeof(b), "%d%%", secOfDay * 100 / 86400); setText(mRingVal[2], b);
            char wk[8];
            strftime(wk, sizeof(wk), "%V", &ti);
            snprintf(b, sizeof(b), "KW %s", wk); setText(mDetail[2], b);
            break;
        }

        case FACE_CHRONO: {
            updateHands(cHands, hr, min, sec, 92, 14, 140, 16, 152, 26);
            snprintf(b, sizeof(b), "%s %02d", WD_DE_S[ti.tm_wday % 7], ti.tm_mday); setText(cDate, b);
            snprintf(b, sizeof(b), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec); setText(cDigital, b);
            break;
        }

        case FACE_MINIMAL: {
            snprintf(b, sizeof(b), "%s %d. %s", WD_DE_L[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12]);
            setText(nDate, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(nHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nMM, b);
            snprintf(b, sizeof(b), "%d%%%s", bat, chg ? "  LÄDT" : ""); setText(nBat, b);
            break;
        }

        case FACE_ULTRA_WAYFINDER: {
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(wHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(wMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(wSec, b);
            snprintf(b, sizeof(b), "%s %02d. %s", WD_DE_S[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12]);
            setText(wDate, b);

            if (secTick) {
                bool fix = GpsManager::hasFix();
                bool last = GpsManager::hasLastFix();
                if (fix || last) {
                    double la = GpsManager::latitude(), lo = GpsManager::longitude();
                    snprintf(b, sizeof(b), "%.4f° %c  %.4f° %c", fabs(la), la < 0 ? 'S' : 'N', fabs(lo), lo < 0 ? 'W' : 'O');
                    setText(wCoord, b);
                    setTextColorIf(wCoord, fix ? cText2() : cText3());
                } else {
                    setText(wCoord, GpsManager::isEnabled() ? "KEIN FIX" : "GPS AUS");
                    setTextColorIf(wCoord, cText3());
                }
                if (fix) {
                    snprintf(b, sizeof(b), "HÖHE %.0f M • HDOP %.1f • %u SAT", GpsManager::altitudeM(), GpsManager::hdop(),
                             (unsigned)GpsManager::satellites());
                } else if (last) {
                    char a[24];
                    ago(a, sizeof(a), GpsManager::lastFixEpoch());
                    snprintf(b, sizeof(b), "LETZTER FIX %s", a);
                } else {
                    snprintf(b, sizeof(b), "HÖHE --");
                }
                setText(wAlt, b);
                float hd = MotionHaptic::getHeadingDeg();
                if (MotionHaptic::isSensorOnline()) {
                    snprintf(b, sizeof(b), "KURS %03d° %s", ((int)lroundf(hd)) % 360, cardinalDe(hd));
                } else {
                    snprintf(b, sizeof(b), "KURS --");
                }
                setText(wHeading, b);
            }

            uint32_t steps = MotionHaptic::getStepsToday();
            uint32_t goal = SettingsStorage::get().dailyStepGoal ? SettingsStorage::get().dailyStepGoal : 1;
            setArc(wRingSteps, (float)steps / goal);
            snprintf(b, sizeof(b), "%lu", (unsigned long)steps); setText(wStepsTxt, b);
            setArc(wRingBat, bat / 100.0f);
            snprintf(b, sizeof(b), "%d%%", bat); setText(wBatTxt, b);
            break;
        }

        case FACE_SKELETON_TOURBILLON: {
            updateHands(tHands, hr, min, sec, 96, 16, 142, 18, 154, 26);
            // balance wheel: continuous oscillation driven by millis(), no phase jump per second
            float oscDeg = sinf((float)(millis() % 100000UL) * 0.008f) * 140.0f;
            updateLineHand(tTourbArm1, tArmPts[0], TB_SZ / 2, TB_SZ / 2, oscDeg, 38, 38);
            updateLineHand(tTourbArm2, tArmPts[1], TB_SZ / 2, TB_SZ / 2, oscDeg + 90.0f, 38, 38);
            if (lv_arc_get_value(tPowerArc) != bat) lv_arc_set_value(tPowerArc, bat);
            snprintf(b, sizeof(b), "%d%%", bat); setText(tPowerTxt, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(tDate, b);
            break;
        }

        case FACE_CASIO: {
            int dIdx = (ti.tm_wday == 0) ? 6 : (ti.tm_wday - 1);
            for (int i = 0; i < 7; i++) {
                bool active = (i == dIdx);
                setBgColorIf(kDayBoxes[i], active ? lv_color_hex(0x111611) : lv_color_hex(0x9EAC8D));
                setTextColorIf(kDayLabels[i], active ? lv_color_hex(0x9EAC8D) : lv_color_hex(0x8D9B7C));
            }
            snprintf(b, sizeof(b), "%02d.%02d.", ti.tm_mday, ti.tm_mon + 1); setText(kDate, b);
            for (int g = 0; g < 10; g++) {
                setBgColorIf(kBarDots[g], (g <= (ti.tm_sec % 10)) ? lv_color_hex(0x111611) : lv_color_hex(0x8D9B7C));
            }
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(kHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(kMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(kSS, b);
            snprintf(b, sizeof(b), "AKKU %d%%%s", bat, chg ? " LÄDT" : ""); setText(kBat, b);
            break;
        }

        case FACE_BAUHAUS: {
            updateHands(bHands, hr, min, sec, 92, 14, 144, 16, 156, 28);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(bDate, b);
            break;
        }

        case FACE_CYBERPUNK: {
            snprintf(b, sizeof(b), "// WATCHOS %s %s", OS_VERSION, (ti.tm_sec % 2) ? "_" : " ");
            setText(yHeader, b);
            snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); setText(yHero, b);
            snprintf(b, sizeof(b), ":%02d", ti.tm_sec); setText(ySec, b);
            if (secTick) {
                snprintf(b, sizeof(b), "HEAP: %u KB • PSRAM: %u KB",
                         (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
                setText(yStatus, b);
                uint32_t steps = MotionHaptic::getStepsToday();
                if (RadioLoRa::isEnabled()) {
                    snprintf(b, sizeof(b), "STEPS: %lu • MESH: %u PEERS", (unsigned long)steps,
                             (unsigned)RadioLoRa::getPeerCount());
                } else {
                    snprintf(b, sizeof(b), "STEPS: %lu • MESH: AUS", (unsigned long)steps);
                }
                setText(yDiag, b);
                snprintf(b, sizeof(b), "PWR: %d%% %s • %.2f V", bat, chg ? "LÄDT" : "AKKU", PowerManager::getBatteryVoltage());
                setText(yBat, b);
            }
            break;
        }

        case FACE_WORDCLOCK: {
            int h = ti.tm_hour, m = ti.tm_min;
            int dispH = (m >= 25) ? ((h + 1) % 24) : h;
            dispH = dispH % 12;
            if (dispH == 0) dispH = 12;
            int mBlock = m / 5;
            int minMod = m % 5;
            for (int i = 0; i < 4; i++) {
                setBgColorIf(wcDots[i], (minMod >= (i + 1)) ? lv_color_hex(0x00E5FF) : cStroke());
            }
            auto setW = [](int idx, bool on) { setTextColorIf(wcWords[idx], on ? cText() : cStroke()); };
            setW(0, true);  // ES
            setW(1, true);  // IST
            setW(2, mBlock == 1 || mBlock == 5 || mBlock == 7 || mBlock == 11); // FÜNF
            setW(3, mBlock == 2 || mBlock == 10);                               // ZEHN (min)
            setW(4, mBlock == 4 || mBlock == 8);                                // ZWANZIG
            setW(5, mBlock == 3 || mBlock == 9);                                // VIERTEL
            setW(6, mBlock == 5 || mBlock == 8 || mBlock == 9 || mBlock == 10 || mBlock == 11); // VOR
            setW(7, mBlock == 1 || mBlock == 2 || mBlock == 3 || mBlock == 4 || mBlock == 7);  // NACH
            setW(8, mBlock >= 5 && mBlock <= 7);                               // HALB
            setW(9, dispH == 12);
            setW(10, dispH == 2);
            // "ES IST EIN UHR" (sharp), otherwise "EINS" ("FÜNF NACH EINS", "HALB EINS")
            setText(wcWords[11], (dispH == 1 && mBlock == 0) ? "EIN" : "EINS");
            setW(11, dispH == 1);
            setW(12, dispH == 3);
            setW(13, dispH == 4);
            setW(14, dispH == 6);
            setW(15, dispH == 8);
            setW(16, dispH == 7);
            setW(17, dispH == 9);
            setW(18, dispH == 10);
            setW(19, dispH == 11);
            setW(20, mBlock == 0);                                             // UHR
            break;
        }

        case FACE_NEON_HORIZON: {
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(nhHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nhMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(nhSec, b);
            snprintf(b, sizeof(b), "%s %02d. %s %d", WD_DE_S[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12],
                     ti.tm_year + 1900);
            setText(nhDate, b);
            snprintf(b, sizeof(b), "AKKU %d%%%s", bat, chg ? " • LÄDT" : "");
            setText(nhBat, b);
            break;
        }

        case FACE_PILOT_FLIEGER: {
            updateHands(pfHands, hr, min, sec, 92, 16, 148, 18, 158, 28);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(pfDate, b);
            break;
        }

        case FACE_DIVER_PRO: {
            updateHands(dpHands, hr, min, sec, 96, 16, 146, 18, 156, 26);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(dpDate, b);
            break;
        }

        case FACE_RADAR_TACTICAL: {
            updateLineHand(rtSweep, rtPts, RT_SZ / 2, RT_SZ / 2, sec * 6.0f, 168, 0);
            snprintf(b, sizeof(b), "SCAN // AZIMUT %03d°", (int)(sec * 6.0f) % 360);
            setText(rtHeading, b);
            if (secTick) {
                // blips: real LoRa peers with a position, relative to our own position
                static LoraPeerNode peers[3];
                uint8_t np = RadioLoRa::isEnabled() ? RadioLoRa::getPeers(peers, 3) : 0;
                bool haveRef = false;
                double myLat = 0, myLon = 0;
                if (GpsManager::hasFix() || GpsManager::hasLastFix()) {
                    myLat = GpsManager::latitude(); myLon = GpsManager::longitude(); haveRef = true;
                } else {
                    NavStatus st = NavEngine::status();
                    if (st.posSource != POS_SRC_DEFAULT) { myLat = st.curLat; myLon = st.curLon; haveRef = true; }
                }
                int shown = 0;
                for (int i = 0; i < 3; i++) {
                    bool ok = haveRef && i < np && peers[i].hasPos;
                    if (ok) {
                        double dy = (peers[i].lat - myLat) * 111.32;
                        double dx = (peers[i].lon - myLon) * 111.32 * cos(myLat * M_PI / 180.0);
                        double km = sqrt(dx * dx + dy * dy);
                        double brg = atan2(dx, dy);                            // 0 = north, clockwise
                        float r = 30.0f + 45.0f * log10f(1.0f + (float)km);    // log scale, 1 km ~ 44 px
                        if (r > 170.0f) r = 170.0f;
                        int px = RT_SZ / 2 + (int)lroundf(sinf((float)brg) * r) - 4;
                        int py = RT_SZ / 2 - (int)lroundf(cosf((float)brg) * r) - 4;
                        lv_obj_set_pos(rtBlip[i], px, py);
                        shown++;
                    }
                    setHidden(rtBlip[i], !ok);
                }
                if (!RadioLoRa::isEnabled()) snprintf(b, sizeof(b), "%02d:%02d:%02d • LORA AUS", ti.tm_hour, ti.tm_min, ti.tm_sec);
                else if (!haveRef) snprintf(b, sizeof(b), "%02d:%02d:%02d • KEINE POSITION", ti.tm_hour, ti.tm_min, ti.tm_sec);
                else snprintf(b, sizeof(b), "%02d:%02d:%02d • %d KNOTEN", ti.tm_hour, ti.tm_min, ti.tm_sec, shown);
                setText(rtTime, b);
            }
            break;
        }

        case FACE_LIFETIME: {
            snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); setText(ltTime, b);
            snprintf(b, sizeof(b), "%s, %02d. %s", WD_DE_L[ti.tm_wday % 7], ti.tm_mday, MO_DE_L[ti.tm_mon % 12]);
            setText(ltDate, b);
            if (secTick) {
                long secOfDay = ti.tm_hour * 3600L + ti.tm_min * 60L + ti.tm_sec;
                int year = ti.tm_year + 1900;
                long daysInYear = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 366 : 365;
                long dim = daysInMonth(year, ti.tm_mon);
                long wdayMon = (ti.tm_wday + 6) % 7;
                int pct[4];
                pct[0] = (int)((ti.tm_yday * 86400L + secOfDay) * 100 / (daysInYear * 86400L));
                pct[1] = (int)(((ti.tm_mday - 1) * 86400L + secOfDay) * 100 / (dim * 86400L));
                pct[2] = (int)((wdayMon * 86400L + secOfDay) * 100 / (7L * 86400L));
                pct[3] = (int)(secOfDay * 100 / 86400L);
                for (int i = 0; i < 4; i++) {
                    setBar(ltBars[i], pct[i]);
                    snprintf(b, sizeof(b), "%d%%", pct[i]); setText(ltPct[i], b);
                }
            }
            break;
        }

        default:
            break;
    }
}

void faceSelect(int idx) {
    idx = ((idx % FACE_COUNT) + FACE_COUNT) % FACE_COUNT;
    SystemSettings& s = SettingsStorage::get();
    s.watchfaceIndex = (uint8_t)idx;
    SettingsStorage::save();
    showFace(idx);
    nav::markDirty(nav::PG_FACE);
    nav::markDirty(nav::PG_APP_FACES);
    DisplayEngine::showToast("Zifferblatt", FACE_NAMES[idx], 1800, ICO_WATCH);
}

void faceNext() {
    faceSelect(SettingsStorage::get().watchfaceIndex + 1);
}

bool faceShowing() { return nav::current() == nav::PG_FACE; }

void buildFace(lv_obj_t* scr) {
    sRoot = pageRoot(scr);

    sFaces[FACE_MODULAR]             = faceRoot(); buildModular(sFaces[FACE_MODULAR]);
    sFaces[FACE_CHRONO]              = faceRoot(); buildChrono(sFaces[FACE_CHRONO]);
    sFaces[FACE_MINIMAL]             = faceRoot(); buildMinimal(sFaces[FACE_MINIMAL]);
    sFaces[FACE_ULTRA_WAYFINDER]     = faceRoot(); buildUltraWayfinder(sFaces[FACE_ULTRA_WAYFINDER]);
    sFaces[FACE_SKELETON_TOURBILLON] = faceRoot(); buildSkeletonTourbillon(sFaces[FACE_SKELETON_TOURBILLON]);
    sFaces[FACE_CASIO]               = faceRoot(); buildCasio(sFaces[FACE_CASIO]);
    sFaces[FACE_BAUHAUS]             = faceRoot(); buildBauhaus(sFaces[FACE_BAUHAUS]);
    sFaces[FACE_CYBERPUNK]           = faceRoot(); buildCyberpunk(sFaces[FACE_CYBERPUNK]);
    sFaces[FACE_WORDCLOCK]           = faceRoot(); buildWordClock(sFaces[FACE_WORDCLOCK]);
    sFaces[FACE_NEON_HORIZON]        = faceRoot(); buildNeonHorizon(sFaces[FACE_NEON_HORIZON]);
    sFaces[FACE_PILOT_FLIEGER]       = faceRoot(); buildPilotFlieger(sFaces[FACE_PILOT_FLIEGER]);
    sFaces[FACE_DIVER_PRO]           = faceRoot(); buildDiverPro(sFaces[FACE_DIVER_PRO]);
    sFaces[FACE_RADAR_TACTICAL]      = faceRoot(); buildRadarTactical(sFaces[FACE_RADAR_TACTICAL]);
    sFaces[FACE_LIFETIME]            = faceRoot(); buildLifetime(sFaces[FACE_LIFETIME]);

    // shared unread badge above every face (swipe up opens the notification sheet)
    sBadge = box(sRoot, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_pad_hor(sBadge, 10, 0);
    lv_obj_set_style_radius(sBadge, 10, 0);
    lv_obj_set_style_bg_opa(sBadge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sBadge, accent(), 0);
    accReg(sBadge, ACC_BG);
    sBadgeTxt = label(sBadge, F.cap, cBg(), "1 NEU");
    lv_obj_set_style_text_letter_space(sBadgeTxt, 1, 0);
    lv_obj_center(sBadgeTxt);
    lv_obj_align(sBadge, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_flag(sBadge, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < FACE_COUNT; i++) lv_obj_add_flag(sFaces[i], LV_OBJ_FLAG_HIDDEN);
    showFace(SettingsStorage::get().watchfaceIndex);

    nav::PageDef d;
    d.root = sRoot;
    d.refresh = refresh;
    d.refreshMs = 0;            // live: every frame (sweeping hands & animations)
    d.ttlMs = 1000;
    nav::registerPage(nav::PG_FACE, d);
}

}  // namespace pages
