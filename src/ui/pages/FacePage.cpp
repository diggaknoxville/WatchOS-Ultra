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
    "Chrono Master GT", "Casio Vintage", "Bauhaus", "Cyberpunk HUD",
    "Wortuhr", "Solar Orbit", "Pilot Flieger", "Diver Pro",
    "Radar Tactical", "Lifetime 52"
};
static const char* FACE_DESC[FACE_COUNT] = {
    "Digitale Zeit mit Schritten, Akku und Tag",
    "Analoges Chronometer mit Sekundenzeiger",
    "Grosse Ziffern, sonst nichts",
    "Mitdrehender Kompass, Koordinaten & Kurs",
    "Sport-Chronograph mit 3 Hilfszifferblättern",
    "Retro-LCD mit 7-Segment, Schritten & Kompass",
    "Reduziertes Bauhaus-Zifferblatt",
    "Sci-Fi HUD mit Telemetrie & Systemdaten",
    "Die Zeit in Worten, auf Deutsch",
    "Astronomie mit Sonnen- und Erdbogen",
    "Fliegeruhr nach B-Uhr-Vorbild",
    "Taucheruhr mit Leuchtmarken",
    "Sonar-Scope, Punkte sind LoRa-Knoten",
    "52-Wochen-Jahresraster & Lebensfortschritt"
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
static lv_obj_t *mStatusTag, *mWifi, *mBle, *mLora, *mGps, *mEco, *mDnd;
static lv_obj_t *mHH, *mMM, *mSecBar, *mDate, *mEvent, *mAmPm;
static lv_obj_t *mRing[3], *mRingVal[3], *mDetail[3];

// 3 Separate Ring Detail Modals
static lv_obj_t* sStepsDetailModal = nullptr;
static lv_obj_t* sBatteryDetailModal = nullptr;
static lv_obj_t* sDayDetailModal = nullptr;

// Steps Detail widgets
static lv_obj_t *sStArc = nullptr, *sStPct = nullptr, *sStSteps = nullptr, *sStSub = nullptr;
static lv_obj_t *sStBars[24];
static lv_obj_t *sStPeakLbl = nullptr, *sStCadenceLbl = nullptr, *sStActiveLbl = nullptr, *sStStandLbl = nullptr, *sStMetabLbl = nullptr;

// Battery Detail widgets
static lv_obj_t *sBtArc = nullptr, *sBtPct = nullptr, *sBtVolt = nullptr, *sBtEst = nullptr;
static lv_obj_t *sBtBars[24];
static lv_obj_t *sBtRateLbl = nullptr, *sBtTempLbl = nullptr, *sBtStatusLbl = nullptr, *sBtAuditLbl = nullptr;

// Day Detail widgets
static lv_obj_t *sDyArc = nullptr, *sDyPct = nullptr, *sDyTitle = nullptr, *sDySub = nullptr;
static lv_obj_t *sDySunriseLbl = nullptr, *sDySunsetLbl = nullptr, *sDyDaylightLbl = nullptr;
static lv_obj_t *sDyMoonLbl = nullptr, *sDySeasonLbl = nullptr, *sDyTimeFmtVal = nullptr;

static inline int formatHour(int hour24, bool* isPm = nullptr) {
    if (isPm) *isPm = (hour24 >= 12);
    if (!SettingsStorage::get().timeFormat12h) return hour24;
    int h = hour24 % 12;
    return (h == 0) ? 12 : h;
}

bool closeRingsDetailIfOpen() {
    bool closed = false;
    if (sStepsDetailModal && !lv_obj_has_flag(sStepsDetailModal, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(sStepsDetailModal, LV_OBJ_FLAG_HIDDEN);
        closed = true;
    }
    if (sBatteryDetailModal && !lv_obj_has_flag(sBatteryDetailModal, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(sBatteryDetailModal, LV_OBJ_FLAG_HIDDEN);
        closed = true;
    }
    if (sDayDetailModal && !lv_obj_has_flag(sDayDetailModal, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(sDayDetailModal, LV_OBJ_FLAG_HIDDEN);
        closed = true;
    }
    if (closed) nav::markDirty(nav::PG_FACE);
    return closed;
}

bool isRingsDetailOpen() {
    return (sStepsDetailModal && !lv_obj_has_flag(sStepsDetailModal, LV_OBJ_FLAG_HIDDEN)) ||
           (sBatteryDetailModal && !lv_obj_has_flag(sBatteryDetailModal, LV_OBJ_FLAG_HIDDEN)) ||
           (sDayDetailModal && !lv_obj_has_flag(sDayDetailModal, LV_OBJ_FLAG_HIDDEN));
}

static void openStepsDetail() {
    closeRingsDetailIfOpen();
    if (sStepsDetailModal) {
        lv_obj_remove_flag(sStepsDetailModal, LV_OBJ_FLAG_HIDDEN);
        nav::markDirty(nav::PG_FACE);
    }
}

static void openBatteryDetail() {
    closeRingsDetailIfOpen();
    if (sBatteryDetailModal) {
        lv_obj_remove_flag(sBatteryDetailModal, LV_OBJ_FLAG_HIDDEN);
        nav::markDirty(nav::PG_FACE);
    }
}

static void openDayDetail() {
    closeRingsDetailIfOpen();
    if (sDayDetailModal) {
        lv_obj_remove_flag(sDayDetailModal, LV_OBJ_FLAG_HIDDEN);
        nav::markDirty(nav::PG_FACE);
    }
}

// 1: Chrono
static lv_obj_t *cDial, *cDate, *cDigital;
static AnalogHands cHands;
static const int C_D = 372, C_R = C_D / 2;

// 2: Minimal
static lv_obj_t *nDate, *nHH, *nMM, *nBat;

// 3: Ultra Wayfinder
static lv_obj_t *wHH, *wMM, *wSec, *wDate, *wCoord, *wAlt, *wHeading;
static lv_obj_t *wRingSteps, *wStepsTxt, *wRingBat, *wBatTxt;
static lv_obj_t *wLblN, *wLblO, *wLblS, *wLblW, *wCompassPointer;

// 4: Chrono Master GT
static lv_obj_t *tDial, *tDate, *tPowerTxt;
static lv_obj_t *tTourbArm1, *tTourbArm2;
static AnalogHands tHands;
static const int T_SZ = 376;

// 5: Casio
static lv_obj_t *kDayBoxes[7], *kDayLabels[7], *kDate, *kBarDots[10];
static lv_obj_t *kHH, *kMM, *kSS, *kBat, *kSteps, *kHeading;

// 6: Bauhaus
static lv_obj_t *bDial, *bDate;
static lv_point_precise_t bBatonPts[12][2];     // static: lv_line keeps the pointer
static AnalogHands bHands;
static const int B_SZ = 380;

// 7: Cyberpunk
static lv_obj_t *yHeader, *yHero, *ySec, *yStatus, *yDiag, *yBat, *yNav, *yBio;

// 8: WordClock
enum WcWordId : uint8_t {
    WC_ES = 0, WC_IST, WC_FUENF_M,
    WC_ZEHN_M, WC_ZWANZIG,
    WC_DREI_M, WC_VIERTEL,
    WC_VOR, WC_NACH,
    WC_HALB, WC_ELF, WC_FUENF_H,
    WC_EINS, WC_ZWEI,
    WC_DREI_H, WC_VIER,
    WC_SECHS, WC_ACHT,
    WC_SIEBEN, WC_NEUN,
    WC_ZEHN_H, WC_ZWOELF, WC_UHR,
    WC_COUNT
};
static lv_obj_t *wcWords[WC_COUNT];
static lv_obj_t *wcDots[4];

// 9: Solar Orbit
static lv_obj_t *nhHH, *nhMM, *nhSec, *nhDate, *nhBat, *soSunArc;

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
static lv_obj_t *ltDots[52], *ltStats;

static void longPress(lv_event_t*) {
    faceNext();
    haptic(HAPTIC_DOUBLE_CLICK);
}

static lv_obj_t* faceRoot() {
    lv_obj_t* f = box(sRoot, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(f, cBg(), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(f, longPress, LV_EVENT_LONG_PRESSED, NULL);
    return f;
}

// ============================================================================
// 0: MODULAR
// ============================================================================
static void buildModular(lv_obj_t* f) {
    mStatusTag = pill(f, "ULTRA", accent());
    accReg(mStatusTag, ACC_TEXT);
    lv_obj_set_pos(mStatusTag, M_HEAD, 24);

    lv_obj_t* st = box(f, LV_SIZE_CONTENT, 28);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -M_HEAD, 24);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 8, 0);

    mEco  = label(st, F.icoS, lv_color_hex(0xFFD60A), ICO_BOLT);
    mDnd  = label(st, F.icoS, lv_color_hex(0xAF52DE), ICO_BEDTIME);
    mWifi = label(st, F.icoS, lv_color_hex(0xFF9F0A), ICO_WIFI);
    mBle  = label(st, F.icoS, lv_color_hex(0x0A84FF), ICO_SMARTPHONE);
    mGps  = label(st, F.icoS, cOk(), ICO_SATELLITE_ALT);
    mLora = label(st, F.icoS, lv_color_hex(0xBF5AF2), ICO_PODCASTS);

    lv_obj_t* hero = box(f, SCR_W, 150);
    lv_obj_set_pos(hero, 0, 56);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 4, 0);
    mHH = label(hero, F.hero, cText(), "00");
    lv_obj_t* colon = label(hero, F.hero, accent(), ":");
    accReg(colon, ACC_TEXT);
    mMM = label(hero, F.hero, cText(), "00");
    mAmPm = caption(hero, "AM", accent());
    accReg(mAmPm, ACC_TEXT);
    lv_obj_set_style_text_font(mAmPm, F.small, 0);
    lv_obj_set_style_pad_left(mAmPm, 6, 0);
    lv_obj_add_flag(mAmPm, LV_OBJ_FLAG_HIDDEN);

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
        pressable(c);
        lv_obj_add_event_cb(c, [](lv_event_t* e) {
            int ringIdx = (int)(intptr_t)lv_event_get_user_data(e);
            if (ringIdx == 0) openStepsDetail();
            else if (ringIdx == 1) openBatteryDetail();
            else if (ringIdx == 2) openDayDetail();
            haptic(HAPTIC_CLICK);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
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
    lv_obj_align(nDate, LV_ALIGN_TOP_MID, 0, 26);
    nHH = label(f, F.giant, cText(), "00");
    lv_obj_align(nHH, LV_ALIGN_TOP_MID, 0, 46);
    nMM = label(f, F.giant, accent(), "00");
    accReg(nMM, ACC_TEXT);
    lv_obj_align(nMM, LV_ALIGN_TOP_MID, 0, 236);
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

    wCompassPointer = label(ringBox, F.title, COL_ULTRA, "^");
    lv_obj_align(wCompassPointer, LV_ALIGN_TOP_MID, 0, 4);

    wLblN = label(ringBox, F.small, COL_ULTRA, "N");
    lv_obj_set_pos(wLblN, 187, 10);
    wLblO = label(ringBox, F.small, cText2(), "O");
    lv_obj_set_pos(wLblO, 360, 187);
    wLblS = label(ringBox, F.small, cText2(), "S");
    lv_obj_set_pos(wLblS, 187, 360);
    wLblW = label(ringBox, F.small, cText2(), "W");
    lv_obj_set_pos(wLblW, 10, 187);

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
// 4: CHRONO MASTER GT (replacing Tourbillon)
// ============================================================================
static void buildSkeletonTourbillon(lv_obj_t* f) {
    const lv_color_t RED_C  = lv_color_hex(0xFF3B30);
    const lv_color_t BEZEL  = lv_color_hex(0x1B2433);
    const lv_color_t DIAL_C = lv_color_hex(0x0A0E17);

    tDial = box(f, T_SZ, T_SZ);
    lv_obj_align(tDial, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(tDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tDial, DIAL_C, 0);
    lv_obj_set_style_bg_opa(tDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tDial, 3, 0);
    lv_obj_set_style_border_color(tDial, BEZEL, 0);

    for (int i = 0; i < 60; i++) {
        if (i % 5 == 0) {
            float a = (i * 6.0f - 90.0f) * (float)M_PI / 180.0f;
            float c = cosf(a), s = sinf(a);
            lv_obj_t* pip = box(tDial, 4, 8);
            lv_obj_set_style_bg_color(pip, (i % 15 == 0) ? RED_C : cText2(), 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_pos(pip, (int)(T_SZ / 2 + c * 168.0f - 2), (int)(T_SZ / 2 + s * 168.0f - 4));
        }
    }

    lv_obj_t* brand = caption(tDial, "CHRONO MASTER", cText());
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_t* sub = caption(tDial, "AUTOMATIC • 100M", RED_C);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 64);

    // Left Sub-Dial (9h): BATTERY
    lv_obj_t* subL = box(tDial, 84, 84);
    lv_obj_set_pos(subL, 26, T_SZ / 2 - 42);
    lv_obj_set_style_radius(subL, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(subL, lv_color_hex(0x060910), 0);
    lv_obj_set_style_bg_opa(subL, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(subL, 1, 0);
    lv_obj_set_style_border_color(subL, BEZEL, 0);
    lv_obj_t* lblL = caption(subL, "AKKU", cText3());
    lv_obj_align(lblL, LV_ALIGN_TOP_MID, 0, 10);
    tPowerTxt = label(subL, F.numS, cOk(), "0%");
    lv_obj_align(tPowerTxt, LV_ALIGN_CENTER, 0, 10);

    // Right Sub-Dial (3h): STEPS
    lv_obj_t* subR = box(tDial, 84, 84);
    lv_obj_set_pos(subR, T_SZ - 26 - 84, T_SZ / 2 - 42);
    lv_obj_set_style_radius(subR, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(subR, lv_color_hex(0x060910), 0);
    lv_obj_set_style_bg_opa(subR, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(subR, 1, 0);
    lv_obj_set_style_border_color(subR, BEZEL, 0);
    lv_obj_t* lblR = caption(subR, "SCHRITTE", cText3());
    lv_obj_align(lblR, LV_ALIGN_TOP_MID, 0, 10);
    tTourbArm1 = label(subR, F.numS, accent(), "0");
    lv_obj_align(tTourbArm1, LV_ALIGN_CENTER, 0, 10);

    // Bottom Sub-Dial (6h): SECONDS & DATE
    lv_obj_t* subB = box(tDial, 96, 96);
    lv_obj_align(subB, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_radius(subB, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(subB, lv_color_hex(0x060910), 0);
    lv_obj_set_style_bg_opa(subB, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(subB, 1, 0);
    lv_obj_set_style_border_color(subB, BEZEL, 0);
    dateWindow(subB, 24, 14, 48, 26, &tDate);
    tTourbArm2 = label(subB, F.title, RED_C, "00");
    lv_obj_align(tTourbArm2, LV_ALIGN_BOTTOM_MID, 0, -12);

    buildHands(tHands, tDial, T_SZ / 2, T_SZ / 2, 8, 5, 2, cText(), cText(), RED_C);
    centreCap(tDial, 16, BEZEL);
}

// ============================================================================
// 5: CASIO VINTAGE
// ============================================================================
static void buildCasio(lv_obj_t* f) {
    const lv_color_t LCD_BG  = lv_color_hex(0x9EAC8D);
    const lv_color_t LCD_INK = lv_color_hex(0x111611);
    const lv_color_t LCD_DIM = lv_color_hex(0x829171);

    lv_obj_t* cCase = card(f, CW, 430);
    lv_obj_align(cCase, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(cCase, lv_color_hex(0x14181F), 0);
    lv_obj_set_style_border_width(cCase, 2, 0);
    lv_obj_set_style_border_color(cCase, lv_color_hex(0x235594), 0);

    lv_obj_t* casio = label(cCase, F.title, cText(), "CASIO");
    lv_obj_set_pos(casio, 20, 10);
    lv_obj_t* illum = label(cCase, F.small, lv_color_hex(0xF5A623), "ILLUMINATOR");
    lv_obj_align(illum, LV_ALIGN_TOP_RIGHT, -20, 12);

    lv_obj_t* lcd = box(cCase, CW - 32, 330);
    lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_radius(lcd, 12, 0);
    lv_obj_set_style_bg_color(lcd, LCD_BG, 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, LCD_INK, 0);

    static const char* dNames[7] = {"MO", "DI", "MI", "DO", "FR", "SA", "SO"};
    for (int i = 0; i < 7; i++) {
        kDayBoxes[i] = box(lcd, 36, 22);
        lv_obj_set_pos(kDayBoxes[i], 12 + i * 46, 12);
        lv_obj_set_style_radius(kDayBoxes[i], 4, 0);
        lv_obj_set_style_bg_opa(kDayBoxes[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(kDayBoxes[i], LCD_BG, 0);
        kDayLabels[i] = label(kDayBoxes[i], F.cap, LCD_DIM, dNames[i]);
        lv_obj_center(kDayLabels[i]);
    }

    lv_obj_t* k24h = card(lcd, 36, 20);
    lv_obj_set_pos(k24h, 16, 46);
    lv_obj_set_style_bg_color(k24h, LCD_INK, 0);
    lv_obj_t* l24 = label(k24h, F.cap, LCD_BG, "24H");
    lv_obj_center(l24);

    lv_obj_t* kAlm = label(lcd, F.cap, LCD_INK, "ALM • SIG");
    lv_obj_set_pos(kAlm, 62, 48);

    kDate = label(lcd, F.title, LCD_INK, "--.--");
    lv_obj_align(kDate, LV_ALIGN_TOP_RIGHT, -16, 44);

    for (int g = 0; g < 10; g++) {
        kBarDots[g] = box(lcd, 8, 5);
        lv_obj_set_pos(kBarDots[g], 16 + g * 12, 74);
        lv_obj_set_style_radius(kBarDots[g], 1, 0);
        lv_obj_set_style_bg_opa(kBarDots[g], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(kBarDots[g], LCD_DIM, 0);
    }

    lv_obj_t* tRow = box(lcd, CW - 32, 96);
    lv_obj_align(tRow, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_flex_flow(tRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    kHH = label(tRow, F.numL, LCD_INK, "00");
    label(tRow, F.numL, LCD_INK, ":");
    kMM = label(tRow, F.numL, LCD_INK, "00");
    kSS = label(tRow, F.numS, LCD_INK, "00");
    lv_obj_set_style_pad_left(kSS, 8, 0);

    lv_obj_t* teleRow = box(lcd, CW - 50, 44);
    lv_obj_align(teleRow, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_flex_flow(teleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(teleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    kSteps = label(teleRow, F.small, LCD_INK, "STP 0");
    kHeading = label(teleRow, F.small, LCD_INK, "DIR N 000°");
    kBat = label(teleRow, F.small, LCD_INK, "BAT 00%");

    lv_obj_t* wr = label(cCase, F.small, lv_color_hex(0xE53935), "WATER 20BAR RESIST");
    lv_obj_set_pos(wr, 20, 390);
    lv_obj_t* chro = label(cCase, F.small, lv_color_hex(0xF5A623), "ALARM CHRONO");
    lv_obj_align(chro, LV_ALIGN_TOP_RIGHT, -20, 390);
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
// 7: CYBERPUNK HUD
// ============================================================================
static void buildCyberpunk(lv_obj_t* f) {
    const lv_color_t CYBER_GREEN = lv_color_hex(0x00FF66);
    const lv_color_t CYBER_CYAN  = lv_color_hex(0x00F0FF);
    const lv_color_t CYBER_YEL   = lv_color_hex(0xFCEE0A);

    lv_obj_t* pan = card(f, CW, 450);
    lv_obj_align(pan, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(pan, lv_color_hex(0x0A0F17), 0);
    lv_obj_set_style_border_width(pan, 1, 0);
    lv_obj_set_style_border_color(pan, CYBER_CYAN, 0);

    lv_obj_t* bTL = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_set_pos(bTL, 16, 10);
    lv_obj_t* bTR = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_align(bTR, LV_ALIGN_TOP_RIGHT, -16, 10);
    lv_obj_t* bBL = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_align(bBL, LV_ALIGN_BOTTOM_LEFT, 16, -10);
    lv_obj_t* bBR = label(pan, F.title, CYBER_CYAN, "+"); lv_obj_align(bBR, LV_ALIGN_BOTTOM_RIGHT, -16, -10);

    yHeader = label(pan, F.small, CYBER_GREEN, "// SYSTEM HUD • V2.4 ONLINE");
    lv_obj_set_pos(yHeader, 36, 14);

    lv_obj_t* timeRow = box(pan, CW - 40, 84);
    lv_obj_align(timeRow, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_flex_flow(timeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timeRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    yHero = label(timeRow, F.hero, CYBER_CYAN, "00:00");
    ySec = label(timeRow, F.title, CYBER_YEL, ":00");
    lv_obj_set_style_pad_left(ySec, 6, 0);

    lv_obj_t* telBox = box(pan, CW - 32, 230);
    lv_obj_align(telBox, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_flex_flow(telBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(telBox, 8, 0);

    yStatus = label(telBox, F.small, CYBER_GREEN, "[CPU] ESP32-S3 • 240MHz • 8MB PSRAM");
    yBat = label(telBox, F.small, CYBER_YEL, "[PWR] BAT 00% • 4.12V • NORMAL");
    yDiag = label(telBox, F.small, CYBER_CYAN, "[BIO] 0 STEPS • 0 KCAL");
    yNav = label(telBox, F.small, cText(), "[NAV] KURS: 000° N • IMU OK");
    yBio = label(telBox, F.small, lv_color_hex(0xFF0055), "[NET] LORA MESH: CH868 • ONLINE");

    lv_obj_t* termBot = caption(pan, "> ALL SYSTEMS OPERATIONAL _", CYBER_GREEN);
    lv_obj_align(termBot, LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ============================================================================
// 8: WORDCLOCK
// ============================================================================
static void buildWordClock(lv_obj_t* f) {
    lv_obj_t* pan = card(f, CW, 450);
    lv_obj_align(pan, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(pan, lv_color_hex(0x0C1019), 0);

    const int Y0 = 22, DY = 38;
    auto addW = [&](int idx, const char* txt, int x, int y) {
        wcWords[idx] = label(pan, F.title, lv_color_hex(0x222A38), txt);
        lv_obj_set_pos(wcWords[idx], x, y);
    };

    // Row 0: ES IST FÜNF
    addW(WC_ES, "ES", 28, Y0 + 0 * DY);
    addW(WC_IST, "IST", 88, Y0 + 0 * DY);
    addW(WC_FUENF_M, "FÜNF", 240, Y0 + 0 * DY);

    // Row 1: ZEHN ZWANZIG
    addW(WC_ZEHN_M, "ZEHN", 28, Y0 + 1 * DY);
    addW(WC_ZWANZIG, "ZWANZIG", 176, Y0 + 1 * DY);

    // Row 2: DREI VIERTEL
    addW(WC_DREI_M, "DREI", 28, Y0 + 2 * DY);
    addW(WC_VIERTEL, "VIERTEL", 176, Y0 + 2 * DY);

    // Row 3: VOR NACH
    addW(WC_VOR, "VOR", 54, Y0 + 3 * DY);
    addW(WC_NACH, "NACH", 210, Y0 + 3 * DY);

    // Row 4: HALB ELF FÜNF
    addW(WC_HALB, "HALB", 28, Y0 + 4 * DY);
    addW(WC_ELF, "ELF", 154, Y0 + 4 * DY);
    addW(WC_FUENF_H, "FÜNF", 240, Y0 + 4 * DY);

    // Row 5: EINS ZWEI
    addW(WC_EINS, "EINS", 28, Y0 + 5 * DY);
    addW(WC_ZWEI, "ZWEI", 230, Y0 + 5 * DY);

    // Row 6: DREI VIER
    addW(WC_DREI_H, "DREI", 28, Y0 + 6 * DY);
    addW(WC_VIER, "VIER", 236, Y0 + 6 * DY);

    // Row 7: SECHS ACHT
    addW(WC_SECHS, "SECHS", 28, Y0 + 7 * DY);
    addW(WC_ACHT, "ACHT", 226, Y0 + 7 * DY);

    // Row 8: SIEBEN NEUN
    addW(WC_SIEBEN, "SIEBEN", 28, Y0 + 8 * DY);
    addW(WC_NEUN, "NEUN", 232, Y0 + 8 * DY);

    // Row 9: ZEHN ZWÖLF UHR
    addW(WC_ZEHN_H, "ZEHN", 28, Y0 + 9 * DY);
    addW(WC_ZWOELF, "ZWÖLF", 146, Y0 + 9 * DY);
    addW(WC_UHR, "UHR", 262, Y0 + 9 * DY);

    // 4 Minute Dots in corners
    wcDots[0] = box(pan, 8, 8); lv_obj_set_pos(wcDots[0], 14, 12);
    wcDots[1] = box(pan, 8, 8); lv_obj_align(wcDots[1], LV_ALIGN_TOP_RIGHT, -14, 12);
    wcDots[2] = box(pan, 8, 8); lv_obj_align(wcDots[2], LV_ALIGN_BOTTOM_RIGHT, -14, -12);
    wcDots[3] = box(pan, 8, 8); lv_obj_align(wcDots[3], LV_ALIGN_BOTTOM_LEFT, 14, -12);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_radius(wcDots[i], 4, 0);
        lv_obj_set_style_bg_opa(wcDots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(wcDots[i], lv_color_hex(0x222A38), 0);
    }
}

// ============================================================================
// 9: SOLAR ORBIT (replacing Neon Horizon)
// ============================================================================
static void buildNeonHorizon(lv_obj_t* f) {
    const lv_color_t DEEP_SKY = lv_color_hex(0x060B19);
    const lv_color_t HORIZON_GLOW = lv_color_hex(0x1B3566);
    const lv_color_t SUN_C = lv_color_hex(0xFFD60A);

    lv_obj_t* sky = box(f, CW, 450);
    lv_obj_align(sky, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(sky, 20, 0);
    lv_obj_set_style_bg_color(sky, DEEP_SKY, 0);
    lv_obj_set_style_bg_opa(sky, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sky, 1, 0);
    lv_obj_set_style_border_color(sky, HORIZON_GLOW, 0);

    static const int STARS_X[12] = {24, 60, 110, 160, 210, 280, 320, 45, 90, 260, 310, 180};
    static const int STARS_Y[12] = {30, 65, 40, 85, 35, 70, 45, 120, 150, 140, 115, 160};
    for (int i = 0; i < 12; i++) {
        lv_obj_t* star = box(sky, 2, 2);
        lv_obj_set_pos(star, STARS_X[i], STARS_Y[i]);
        lv_obj_set_style_bg_color(star, (i % 3 == 0) ? SUN_C : cText2(), 0);
        lv_obj_set_style_bg_opa(star, LV_OPA_COVER, 0);
    }

    soSunArc = lv_arc_create(sky);
    lv_obj_set_size(soSunArc, 310, 310);
    lv_obj_align(soSunArc, LV_ALIGN_TOP_MID, 0, 24);
    lv_arc_set_rotation(soSunArc, 180);
    lv_arc_set_bg_angles(soSunArc, 0, 180);
    lv_arc_set_range(soSunArc, 0, 100);
    lv_obj_set_style_arc_width(soSunArc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(soSunArc, HORIZON_GLOW, LV_PART_MAIN);
    lv_obj_set_style_arc_width(soSunArc, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(soSunArc, SUN_C, LV_PART_INDICATOR);
    lv_obj_remove_style(soSunArc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(soSunArc, LV_OBJ_FLAG_CLICKABLE);

    nhBat = box(sky, 16, 16);
    lv_obj_set_style_radius(nhBat, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(nhBat, SUN_C, 0);
    lv_obj_set_style_bg_opa(nhBat, LV_OPA_COVER, 0);
    lv_obj_set_pos(nhBat, 24, 180);

    nhDate = caption(sky, "", SUN_C);
    lv_obj_align(nhDate, LV_ALIGN_TOP_MID, 0, 188);

    lv_obj_t* tBox = box(sky, CW - 40, 80);
    lv_obj_align(tBox, LV_ALIGN_TOP_MID, 0, 218);
    lv_obj_set_flex_flow(tBox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    nhHH = label(tBox, F.hero, cText(), "00");
    label(tBox, F.hero, SUN_C, ":");
    nhMM = label(tBox, F.hero, cText(), "00");
    nhSec = label(tBox, F.title, SUN_C, "00");
    lv_obj_set_style_pad_left(nhSec, 6, 0);

    lv_obj_t* horizon = box(sky, 360, 180);
    lv_obj_align(horizon, LV_ALIGN_BOTTOM_MID, 0, 80);
    lv_obj_set_style_radius(horizon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(horizon, lv_color_hex(0x0A1B3B), 0);
    lv_obj_set_style_bg_opa(horizon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(horizon, 2, 0);
    lv_obj_set_style_border_color(horizon, lv_color_hex(0x00E5FF), 0);

    lv_obj_t* btmLine = caption(sky, "SOLAR ORBIT • ERDBOGEN", cText2());
    lv_obj_align(btmLine, LV_ALIGN_BOTTOM_MID, 0, -16);
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
    lv_obj_align(ltTime, LV_ALIGN_TOP_MID, 0, 22);
    ltDate = caption(f, "", accent());
    accReg(ltDate, ACC_TEXT);
    lv_obj_align(ltDate, LV_ALIGN_TOP_MID, 0, 82);

    // 52-Week Visual Dot Matrix (4 rows of 13 dots)
    lv_obj_t* dotBox = box(f, CW, 78);
    lv_obj_align(dotBox, LV_ALIGN_TOP_MID, 0, 110);
    const int DW = 18, DH = 12, DGX = 8, DGY = 6;
    for (int i = 0; i < 52; i++) {
        int row = i / 13;
        int col = i % 13;
        ltDots[i] = box(dotBox, DW, DH);
        lv_obj_set_pos(ltDots[i], 24 + col * (DW + DGX), row * (DH + DGY));
        lv_obj_set_style_radius(ltDots[i], 3, 0);
        lv_obj_set_style_bg_color(ltDots[i], lv_color_hex(0x222A38), 0);
        lv_obj_set_style_bg_opa(ltDots[i], LV_OPA_COVER, 0);
    }

    ltStats = caption(f, "WOCHE -- / 52  •  VERBLIEBEN: --", cText2());
    lv_obj_align(ltStats, LV_ALIGN_TOP_MID, 0, 198);

    static const char* labels[3] = {"JAHR", "MONAT", "TAG"};
    static const lv_color_t colors[3] = {
        lv_color_hex(0xF5A623), lv_color_hex(0x00E5FF), lv_color_hex(0x76FF03)
    };
    const int Y_START = 228, STEP = 70;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = card(f, CW, 60);
        lv_obj_set_pos(c, M, Y_START + i * STEP);
        lv_obj_t* cap = caption(c, labels[i], cText2());
        lv_obj_set_pos(cap, 16, 8);
        ltPct[i] = label(c, F.small, colors[i], "0%");
        lv_obj_align(ltPct[i], LV_ALIGN_TOP_RIGHT, -16, 8);
        ltBars[i] = lv_bar_create(c);
        lv_obj_set_size(ltBars[i], CW - 32, 6);
        lv_obj_align(ltBars[i], LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_bar_set_range(ltBars[i], 0, 100);
        lv_obj_set_style_bg_color(ltBars[i], cStroke(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ltBars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ltBars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ltBars[i], colors[i], LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ltBars[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ltBars[i], 3, LV_PART_INDICATOR);
    }
}

// ============================================================================
//  3-RINGS DETAILANSICHT (SCHRITTE, AKKU, TAG)
// ============================================================================
static lv_obj_t* createDetailModalFrame(lv_obj_t* modal, const char* titleText) {
    lv_obj_set_size(modal, SCR_W, SCR_H);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, cBg(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);

    // Top Bar (Header)
    lv_obj_t* hdr = box(modal, SCR_W, 68);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_style_pad_top(hdr, 18, 0);

    lv_obj_t* backBtn = lv_obj_create(hdr);
    lv_obj_set_size(backBtn, 106, 36);
    lv_obj_align(backBtn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(backBtn, cSurf(), 0);
    lv_obj_set_style_bg_opa(backBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(backBtn, 18, 0);
    lv_obj_set_style_border_width(backBtn, 1, 0);
    lv_obj_set_style_border_color(backBtn, cStroke(), 0);
    pressable(backBtn);
    lv_obj_t* bLbl = label(backBtn, F.small, accent(), "< ZURÜCK");
    accReg(bLbl, ACC_TEXT);
    lv_obj_center(bLbl);
    lv_obj_add_event_cb(backBtn, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* t = label(hdr, F.title, cText(), titleText);
    lv_obj_align(t, LV_ALIGN_CENTER, 22, 0);

    // Scrollable content
    lv_obj_t* list = lv_obj_create(modal);
    lv_obj_set_size(list, SCR_W, SCR_H - 70);
    lv_obj_set_pos(list, 0, 70);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 18, 0);
    lv_obj_set_style_pad_top(list, 4, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    return list;
}

static void buildStepsDetail(lv_obj_t* parent) {
    sStepsDetailModal = lv_obj_create(parent);
    lv_obj_t* list = createDetailModalFrame(sStepsDetailModal, "SCHRITTE");

    // Card 1: Hero Steps Ring & Progress
    lv_obj_t* c1 = card(list, CW, 116);
    lv_obj_set_style_radius(c1, R_TILE, 0);
    sStArc = ring(c1, 76, 7, accent());
    accReg(sStArc, ACC_ARC, LV_PART_INDICATOR);
    lv_obj_set_pos(sStArc, 14, 20);
    sStPct = label(sStArc, F.small, cText(), "0%");
    lv_obj_center(sStPct);

    lv_obj_t* l1 = caption(c1, "SCHRITTE HEUTE", accent());
    accReg(l1, ACC_TEXT);
    lv_obj_set_pos(l1, 106, 16);
    sStSteps = label(c1, F.title, cText(), "0 / 10.000");
    lv_obj_set_pos(sStSteps, 106, 40);
    sStSub = caption(c1, "0.00 km · 0 kcal · 0 SPM", cText2());
    lv_obj_set_pos(sStSub, 106, 78);

    // Card 2: 24h Stündlicher Aktivitätsverlauf (Balkendiagramm)
    lv_obj_t* c2 = card(list, CW, 144);
    lv_obj_set_style_radius(c2, R_TILE, 0);
    lv_obj_t* l2 = caption(c2, "24H STÜNDLICHER VERLAUF", cText3());
    lv_obj_set_pos(l2, 16, 12);
    sStPeakLbl = caption(c2, "Spitzenstunde: 14:00 (1.820)", accent());
    accReg(sStPeakLbl, ACC_TEXT);
    lv_obj_set_pos(sStPeakLbl, 16, 32);

    lv_obj_t* barWrap = box(c2, CW - 32, 60);
    lv_obj_set_pos(barWrap, 16, 56);
    lv_obj_set_flex_flow(barWrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barWrap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int h = 0; h < 24; h++) {
        sStBars[h] = box(barWrap, 10, 8);
        lv_obj_set_style_radius(sStBars[h], 3, 0);
        lv_obj_set_style_bg_opa(sStBars[h], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sStBars[h], cStroke(), 0);
    }
    lv_obj_t* axisLbl = caption(c2, "00h          06h          12h          18h        23h", cText3());
    lv_obj_set_pos(axisLbl, 16, 120);

    // Card 3: Dynamik & Kadenz
    lv_obj_t* c3 = card(list, CW, 100);
    lv_obj_set_style_radius(c3, R_TILE, 0);
    lv_obj_t* l3 = caption(c3, "LEISTUNG & BEWEGUNG", cText3());
    lv_obj_set_pos(l3, 16, 12);
    sStCadenceLbl = label(c3, F.body, cText(), "Kadenz: -- SPM · Schnitt 112 SPM");
    lv_obj_set_pos(sStCadenceLbl, 16, 36);
    sStActiveLbl = caption(c3, "Aktivzeit: 1h 48m · Ruhe: 8h 12m", cText2());
    lv_obj_set_pos(sStActiveLbl, 16, 68);

    // Card 4: Stoffwechsel & Kalorien
    lv_obj_t* c4 = card(list, CW, 90);
    lv_obj_set_style_radius(c4, R_TILE, 0);
    lv_obj_t* l4 = caption(c4, "ENERGIEUMSATZ", cText3());
    lv_obj_set_pos(l4, 16, 12);
    sStMetabLbl = label(c4, F.body, cText(), "Aktiv: 412 kcal · Grundumsatz: 1.480 kcal");
    lv_obj_set_pos(sStMetabLbl, 16, 36);
    sStStandLbl = caption(c4, "Stehziel: 9 / 12 Stunden erreicht", cOk());
    lv_obj_set_pos(sStStandLbl, 16, 62);

    // Buttons
    lv_obj_t* btnApp = primaryButton(list, CW, 48, ICO_DIRECTIONS_RUN, "ZUR FITNESS-APP >");
    lv_obj_add_event_cb(btnApp, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        nav::openApp(nav::PG_ACTIVITY);
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* retBtn = secondaryButton(list, CW, 46, nullptr, "STARTSEITE ZURÜCK");
    lv_obj_add_event_cb(retBtn, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
}

static void buildBatteryDetail(lv_obj_t* parent) {
    sBatteryDetailModal = lv_obj_create(parent);
    lv_obj_t* list = createDetailModalFrame(sBatteryDetailModal, "ENERGIE");

    // Card 1: Hero Battery Ring & Status
    lv_obj_t* c1 = card(list, CW, 116);
    lv_obj_set_style_radius(c1, R_TILE, 0);
    sBtArc = ring(c1, 76, 7, cOk());
    lv_obj_set_pos(sBtArc, 14, 20);
    sBtPct = label(sBtArc, F.small, cText(), "100%");
    lv_obj_center(sBtPct);

    lv_obj_t* l1 = caption(c1, "BATTERIE & PROGNOSE", cOk());
    lv_obj_set_pos(l1, 106, 16);
    sBtVolt = label(c1, F.title, cText(), "100% · 4.15 V");
    lv_obj_set_pos(sBtVolt, 106, 40);
    sBtEst = caption(c1, "Verbleibend: ca. 1d 16h", cText2());
    lv_obj_set_pos(sBtEst, 106, 78);

    // Card 2: 24h Entladekurve
    lv_obj_t* c2 = card(list, CW, 144);
    lv_obj_set_style_radius(c2, R_TILE, 0);
    lv_obj_t* l2 = caption(c2, "24H ENTLADE- & SPANNUNGSVERLAUF", cText3());
    lv_obj_set_pos(l2, 16, 12);
    sBtRateLbl = caption(c2, "Entladerate: -2.1% / Std. · Optimal", cOk());
    lv_obj_set_pos(sBtRateLbl, 16, 32);

    lv_obj_t* barWrap = box(c2, CW - 32, 60);
    lv_obj_set_pos(barWrap, 16, 56);
    lv_obj_set_flex_flow(barWrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barWrap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int h = 0; h < 24; h++) {
        sBtBars[h] = box(barWrap, 10, 8);
        lv_obj_set_style_radius(sBtBars[h], 3, 0);
        lv_obj_set_style_bg_opa(sBtBars[h], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sBtBars[h], cOk(), 0);
    }
    lv_obj_t* axisLbl = caption(c2, "-24h         -18h         -12h         -6h        jetzt", cText3());
    lv_obj_set_pos(axisLbl, 16, 120);

    // Card 3: Energy Audit / Subsysteme
    lv_obj_t* c3 = card(list, CW, 114);
    lv_obj_set_style_radius(c3, R_TILE, 0);
    lv_obj_t* l3 = caption(c3, "VERBRAUCHER-AUDIT", cText3());
    lv_obj_set_pos(l3, 16, 12);
    sBtAuditLbl = label(c3, F.cap, cText2(), "AMOLED Display: ~45%\nGNSS / GPS: ~22%\nLoRa & BLE: ~18%\nCPU & Sensoren: ~15%");
    lv_obj_set_pos(sBtAuditLbl, 16, 34);

    // Card 4: Hardware-Zustand & Sparmodus
    lv_obj_t* c4 = card(list, CW, 78);
    lv_obj_set_style_radius(c4, R_TILE, 0);
    pressable(c4);
    sBtTempLbl = caption(c4, "PMU AXP2101: 34 °C · Status Normal", cText3());
    lv_obj_set_pos(sBtTempLbl, 16, 12);
    sBtStatusLbl = label(c4, F.body, accent(), "Energiesparmodus umschalten >");
    accReg(sBtStatusLbl, ACC_TEXT);
    lv_obj_set_pos(sBtStatusLbl, 16, 38);
    lv_obj_add_event_cb(c4, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.powerSaver = !s.powerSaver;
        SettingsStorage::save();
        haptic(HAPTIC_TICK);
        DisplayEngine::showToast("Energiesparmodus", s.powerSaver ? "Aktiviert" : "Deaktiviert", 2000, ICO_BOLT);
        nav::markDirty(nav::PG_FACE);
    }, LV_EVENT_CLICKED, NULL);

    // Buttons
    lv_obj_t* btnApp = primaryButton(list, CW, 48, ICO_POWER_SETTINGS_NEW, "ZUR ENERGIE-APP >");
    lv_obj_add_event_cb(btnApp, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        nav::openApp(nav::PG_APP_POWER);
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* retBtn = secondaryButton(list, CW, 46, nullptr, "STARTSEITE ZURÜCK");
    lv_obj_add_event_cb(retBtn, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
}

static void buildDayDetail(lv_obj_t* parent) {
    sDayDetailModal = lv_obj_create(parent);
    lv_obj_t* list = createDetailModalFrame(sDayDetailModal, "TAG & SONNE");

    // Card 1: Hero Datum & Jahresfortschritt
    lv_obj_t* c1 = card(list, CW, 116);
    lv_obj_set_style_radius(c1, R_TILE, 0);
    sDyArc = ring(c1, 76, 7, cInfo());
    lv_obj_set_pos(sDyArc, 14, 20);
    sDyPct = label(sDyArc, F.small, cText(), "82%");
    lv_obj_center(sDyPct);

    sDyTitle = caption(c1, "DATUM & KALENDER", cInfo());
    lv_obj_set_pos(sDyTitle, 106, 16);
    sDySunriseLbl = label(c1, F.title, cText(), "Dienstag, 27. Okt.");
    lv_obj_set_pos(sDySunriseLbl, 106, 40);
    sDySub = caption(c1, "Tag 301 von 365 · KW 44", cText2());
    lv_obj_set_pos(sDySub, 106, 78);

    // Card 2: 24h Sonnenlauf & Lichtkurve
    lv_obj_t* c2 = card(list, CW, 126);
    lv_obj_set_style_radius(c2, R_TILE, 0);
    lv_obj_t* l2 = caption(c2, "24H SONNENLAUF & TAGESLICHT", cText3());
    lv_obj_set_pos(l2, 16, 12);

    sDyDaylightLbl = label(c2, F.body, accent(), "Tageslicht: 10h 52m (Verbleibend: 6h 18m)");
    accReg(sDyDaylightLbl, ACC_TEXT);
    lv_obj_set_pos(sDyDaylightLbl, 16, 36);

    sDySunsetLbl = caption(c2, "Aufgang: 07:12 Uhr   ·   Untergang: 18:04 Uhr", cText2());
    lv_obj_set_pos(sDySunsetLbl, 16, 68);
    lv_obj_t* sDyGold = caption(c2, "Goldene Stunde: 17:28 - 18:04 Uhr", lv_color_hex(0xFF9F0A));
    lv_obj_set_pos(sDyGold, 16, 94);

    // Card 3: Astronomie & Jahreszeit
    lv_obj_t* c3 = card(list, CW, 90);
    lv_obj_set_style_radius(c3, R_TILE, 0);
    lv_obj_t* l3 = caption(c3, "ASTRONOMIE & JAHRESZEIT", cText3());
    lv_obj_set_pos(l3, 16, 12);
    sDyMoonLbl = label(c3, F.body, cText(), "Mondphase: Zunehmend (68%)");
    lv_obj_set_pos(sDyMoonLbl, 16, 36);
    sDySeasonLbl = caption(c3, "Jahreszeit: Herbst (Tag 36 von 90)", cText2());
    lv_obj_set_pos(sDySeasonLbl, 16, 62);

    // Card 4: Zeitformat Umschaltung
    lv_obj_t* c4 = card(list, CW, 74);
    lv_obj_set_style_radius(c4, R_TILE, 0);
    pressable(c4);
    lv_obj_t* l4 = caption(c4, "ZEITFORMAT", cText3());
    lv_obj_set_pos(l4, 18, 12);
    sDyTimeFmtVal = label(c4, F.body, accent(), "24h");
    accReg(sDyTimeFmtVal, ACC_TEXT);
    lv_obj_set_pos(sDyTimeFmtVal, 18, 36);
    lv_obj_t* btnFmt = secondaryButton(c4, 124, 38, nullptr, "12h / 24h");
    lv_obj_align(btnFmt, LV_ALIGN_RIGHT_MID, -14, 0);
    auto onToggleTime = [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.timeFormat12h = !s.timeFormat12h;
        SettingsStorage::save();
        haptic(HAPTIC_TICK);
        nav::markDirty(nav::PG_FACE);
    };
    lv_obj_add_event_cb(c4, onToggleTime, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btnFmt, onToggleTime, LV_EVENT_CLICKED, NULL);

    // Buttons
    lv_obj_t* btnApp = primaryButton(list, CW, 48, ICO_CALENDAR_MONTH, "ZUM KALENDER >");
    lv_obj_add_event_cb(btnApp, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        nav::openApp(nav::PG_AGENDA);
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* retBtn = secondaryButton(list, CW, 46, nullptr, "STARTSEITE ZURÜCK");
    lv_obj_add_event_cb(retBtn, [](lv_event_t*) {
        closeRingsDetailIfOpen();
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
}

// ============================================================================
//  SHOW / REFRESH LOOP
// ============================================================================
static void showFace(int idx) {
    idx = ((idx % FACE_COUNT) + FACE_COUNT) % FACE_COUNT;
    if (idx == sShown) return;
    for (int i = 0; i < FACE_COUNT; i++) setHidden(sFaces[i], i != idx);
    sShown = idx;
    if (sRoot) lv_obj_invalidate(sRoot);
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
            bool dnd = DisplayEngine::doNotDisturb();
            bool saver = SettingsStorage::get().powerSaver;
            if (dnd) setPill(mStatusTag, "DND", lv_color_hex(0xAF52DE));
            else if (saver) setPill(mStatusTag, "ECO", lv_color_hex(0xFFD60A));
            else setPill(mStatusTag, "ULTRA", accent());

            setHidden(mDnd, !dnd);
            setHidden(mEco, !saver);

            bool ap = CompanionServer::isApActive();
            setHidden(mWifi, !ap);

            bool bleConn = BleCompanion::isConnected();
            bool bleEn = BleCompanion::isEnabled();
            if (!bleEn) setHidden(mBle, true);
            else {
                setHidden(mBle, false);
                setTextColorIf(mBle, bleConn ? lv_color_hex(0x0A84FF) : cText3());
            }

            bool gpsFix = GpsManager::hasFix();
            bool gpsPwr = GpsManager::isPowered();
            if (!gpsPwr) setHidden(mGps, true);
            else {
                setHidden(mGps, false);
                setTextColorIf(mGps, gpsFix ? cOk() : lv_color_hex(0xFFD60A));
            }

            bool loraEn = RadioLoRa::isEnabled();
            setHidden(mLora, !loraEn);
            if (loraEn) setTextColorIf(mLora, RadioLoRa::isReady() ? lv_color_hex(0xBF5AF2) : cText3());

            bool isPm = false;
            int hDisp = formatHour(ti.tm_hour, &isPm);
            if (SettingsStorage::get().timeFormat12h) {
                setHidden(mAmPm, false);
                setText(mAmPm, isPm ? "PM" : "AM");
                snprintf(b, sizeof(b), "%02d", hDisp);
            } else {
                setHidden(mAmPm, true);
                snprintf(b, sizeof(b), "%02d", ti.tm_hour);
            }
            setText(mHH, b);
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
            snprintf(b, sizeof(b), "%02d:%02d:%02d", formatHour(ti.tm_hour), ti.tm_min, ti.tm_sec); setText(cDigital, b);
            break;
        }

        case FACE_MINIMAL: {
            snprintf(b, sizeof(b), "%s %d. %s", WD_DE_L[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12]);
            setText(nDate, b);
            snprintf(b, sizeof(b), "%02d", formatHour(ti.tm_hour)); setText(nHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nMM, b);
            snprintf(b, sizeof(b), "%d%%%s", bat, chg ? "  LÄDT" : ""); setText(nBat, b);
            break;
        }

        case FACE_ULTRA_WAYFINDER: {
            snprintf(b, sizeof(b), "%02d", formatHour(ti.tm_hour)); setText(wHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(wMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(wSec, b);
            snprintf(b, sizeof(b), "%s %02d. %s", WD_DE_S[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12]);
            setText(wDate, b);

            float hd = MotionHaptic::getHeadingDeg();
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
                if (MotionHaptic::isSensorOnline()) {
                    snprintf(b, sizeof(b), "KURS %03d° %s", ((int)lroundf(hd)) % 360, cardinalDe(hd));
                } else {
                    snprintf(b, sizeof(b), "KURS --");
                }
                setText(wHeading, b);
            }

            // Dynamic live rotating compass ring (N, O, S, W) on outer bezel perimeter
            const float CR = 180.0f, CCX = 193.0f, CCY = 193.0f;
            float aN = (0.0f - hd) * (float)M_PI / 180.0f;
            float aO = (90.0f - hd) * (float)M_PI / 180.0f;
            float aS = (180.0f - hd) * (float)M_PI / 180.0f;
            float aW = (270.0f - hd) * (float)M_PI / 180.0f;
            lv_obj_set_pos(wLblN, (int)lroundf(CCX + CR * sinf(aN) - 6), (int)lroundf(CCY - CR * cosf(aN) - 8));
            lv_obj_set_pos(wLblO, (int)lroundf(CCX + CR * sinf(aO) - 6), (int)lroundf(CCY - CR * cosf(aO) - 8));
            lv_obj_set_pos(wLblS, (int)lroundf(CCX + CR * sinf(aS) - 6), (int)lroundf(CCY - CR * cosf(aS) - 8));
            lv_obj_set_pos(wLblW, (int)lroundf(CCX + CR * sinf(aW) - 6), (int)lroundf(CCY - CR * cosf(aW) - 8));

            uint32_t steps = MotionHaptic::getStepsToday();
            uint32_t goal = SettingsStorage::get().dailyStepGoal ? SettingsStorage::get().dailyStepGoal : 1;
            setArc(wRingSteps, (float)steps / goal);
            snprintf(b, sizeof(b), "%lu", (unsigned long)steps); setText(wStepsTxt, b);
            setArc(wRingBat, bat / 100.0f);
            snprintf(b, sizeof(b), "%d%%", bat); setText(wBatTxt, b);
            break;
        }

        case FACE_SKELETON_TOURBILLON: {
            updateHands(tHands, hr, min, sec, 96, 16, 144, 18, 158, 26);
            snprintf(b, sizeof(b), "%d%%", bat); setText(tPowerTxt, b);
            uint32_t st = MotionHaptic::getStepsToday();
            if (st >= 1000) snprintf(b, sizeof(b), "%.1fk", st / 1000.0f);
            else snprintf(b, sizeof(b), "%lu", (unsigned long)st);
            setText(tTourbArm1, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec); setText(tTourbArm2, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(tDate, b);
            break;
        }

        case FACE_CASIO: {
            int dIdx = (ti.tm_wday == 0) ? 6 : (ti.tm_wday - 1);
            for (int i = 0; i < 7; i++) {
                bool active = (i == dIdx);
                setBgColorIf(kDayBoxes[i], active ? lv_color_hex(0x111611) : lv_color_hex(0x9EAC8D));
                setTextColorIf(kDayLabels[i], active ? lv_color_hex(0x9EAC8D) : lv_color_hex(0x829171));
            }
            snprintf(b, sizeof(b), "%02d.%02d.", ti.tm_mday, ti.tm_mon + 1); setText(kDate, b);
            for (int g = 0; g < 10; g++) {
                setBgColorIf(kBarDots[g], (g <= (ti.tm_sec % 10)) ? lv_color_hex(0x111611) : lv_color_hex(0x829171));
            }
            snprintf(b, sizeof(b), "%02d", formatHour(ti.tm_hour)); setText(kHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(kMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(kSS, b);

            uint32_t st = MotionHaptic::getStepsToday();
            snprintf(b, sizeof(b), "STP %lu", (unsigned long)st); setText(kSteps, b);

            float hd = MotionHaptic::getHeadingDeg();
            if (MotionHaptic::isSensorOnline()) snprintf(b, sizeof(b), "%s %03d°", cardinalDe(hd), ((int)lroundf(hd)) % 360);
            else snprintf(b, sizeof(b), "DIR --");
            setText(kHeading, b);

            snprintf(b, sizeof(b), "BAT %d%%%s", bat, chg ? "+" : ""); setText(kBat, b);
            break;
        }

        case FACE_BAUHAUS: {
            updateHands(bHands, hr, min, sec, 92, 14, 144, 16, 156, 28);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(bDate, b);
            break;
        }

        case FACE_CYBERPUNK: {
            snprintf(b, sizeof(b), "// CORP_NET: ONLINE • SEC: MAX %s", (ti.tm_sec % 2) ? "_" : " ");
            setText(yHeader, b);
            snprintf(b, sizeof(b), "%02d:%02d", formatHour(ti.tm_hour), ti.tm_min); setText(yHero, b);
            snprintf(b, sizeof(b), ":%02d", ti.tm_sec); setText(ySec, b);
            if (secTick) {
                snprintf(b, sizeof(b), "[CPU] ESP32-S3 • 240M • FREE %uK", (unsigned)(ESP.getFreeHeap() / 1024));
                setText(yStatus, b);
                snprintf(b, sizeof(b), "[PWR] BAT %d%%%s • %.2fV • NORMAL", bat, chg ? " [LADEN]" : "", PowerManager::getBatteryVoltage());
                setText(yBat, b);
                uint32_t steps = MotionHaptic::getStepsToday();
                snprintf(b, sizeof(b), "[BIO] %lu SCHRITTE • %.0f KCAL", (unsigned long)steps, steps * 0.04f);
                setText(yDiag, b);
                float hd = MotionHaptic::getHeadingDeg();
                if (MotionHaptic::isSensorOnline()) snprintf(b, sizeof(b), "[NAV] KURS %03d° %s • IMU OK", ((int)lroundf(hd)) % 360, cardinalDe(hd));
                else snprintf(b, sizeof(b), "[NAV] KURS -- • IMU WAIT");
                setText(yNav, b);
                snprintf(b, sizeof(b), "[NET] %s • BLE %s", RadioLoRa::isEnabled() ? "LORA ON" : "LORA OFF", BleCompanion::isConnected() ? "VERBUNDEN" : "BEREIT");
                setText(yBio, b);
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
                setBgColorIf(wcDots[i], (minMod >= (i + 1)) ? lv_color_hex(0x00E5FF) : lv_color_hex(0x222A38));
            }

            auto setW = [](int idx, bool on) {
                setTextColorIf(wcWords[idx], on ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x222A38));
            };

            for (int i = 0; i < WC_COUNT; i++) setW(i, false);

            setW(WC_ES, true);
            setW(WC_IST, true);

            switch (mBlock) {
                case 0: setW(WC_UHR, true); break;                                   // :00
                case 1: setW(WC_FUENF_M, true); setW(WC_NACH, true); break;         // :05
                case 2: setW(WC_ZEHN_M, true); setW(WC_NACH, true); break;          // :10
                case 3: setW(WC_VIERTEL, true); setW(WC_NACH, true); break;         // :15
                case 4: setW(WC_ZEHN_M, true); setW(WC_VOR, true); setW(WC_HALB, true); break; // :20
                case 5: setW(WC_FUENF_M, true); setW(WC_VOR, true); setW(WC_HALB, true); break; // :25
                case 6: setW(WC_HALB, true); break;                                 // :30
                case 7: setW(WC_FUENF_M, true); setW(WC_NACH, true); setW(WC_HALB, true); break;// :35
                case 8: setW(WC_ZEHN_M, true); setW(WC_NACH, true); setW(WC_HALB, true); break; // :40
                case 9: setW(WC_VIERTEL, true); setW(WC_VOR, true); break;          // :45
                case 10: setW(WC_ZEHN_M, true); setW(WC_VOR, true); break;         // :50
                case 11: setW(WC_FUENF_M, true); setW(WC_VOR, true); break;         // :55
            }

            switch (dispH) {
                case 1:
                    setText(wcWords[WC_EINS], (mBlock == 0) ? "EIN" : "EINS");
                    setW(WC_EINS, true);
                    break;
                case 2: setW(WC_ZWEI, true); break;
                case 3: setW(WC_DREI_H, true); break;
                case 4: setW(WC_VIER, true); break;
                case 5: setW(WC_FUENF_H, true); break;
                case 6: setW(WC_SECHS, true); break;
                case 7: setW(WC_SIEBEN, true); break;
                case 8: setW(WC_ACHT, true); break;
                case 9: setW(WC_NEUN, true); break;
                case 10: setW(WC_ZEHN_H, true); break;
                case 11: setW(WC_ELF, true); break;
                case 12: setW(WC_ZWOELF, true); break;
            }
            break;
        }

        case FACE_NEON_HORIZON: {
            snprintf(b, sizeof(b), "%02d", formatHour(ti.tm_hour)); setText(nhHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nhMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(nhSec, b);
            snprintf(b, sizeof(b), "%s %02d. %s", WD_DE_S[ti.tm_wday % 7], ti.tm_mday, MO_DE_S[ti.tm_mon % 12]);
            setText(nhDate, b);

            // Sun trajectory arc: 6:00 (0%) to 18:00 (100%)
            float daySec = ti.tm_hour * 3600.0f + ti.tm_min * 60.0f + ti.tm_sec;
            float sunPct = (daySec - 21600.0f) / 43200.0f;
            if (sunPct < 0.0f) sunPct = 0.0f;
            if (sunPct > 1.0f) sunPct = 1.0f;
            lv_arc_set_value(soSunArc, (int)(sunPct * 100));
            float sunAngle = (1.0f - sunPct) * (float)M_PI;
            float sunX = (CW / 2.0f) + 155.0f * cosf(sunAngle);
            float sunY = 179.0f - 155.0f * sinf(sunAngle);
            lv_obj_set_pos(nhBat, (int)sunX - 8, (int)sunY - 8);
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
                        double brg = atan2(dx, dy);
                        float r = 30.0f + 45.0f * log10f(1.0f + (float)km);
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
            snprintf(b, sizeof(b), "%02d:%02d", formatHour(ti.tm_hour), ti.tm_min); setText(ltTime, b);
            snprintf(b, sizeof(b), "%s, %02d. %s", WD_DE_L[ti.tm_wday % 7], ti.tm_mday, MO_DE_L[ti.tm_mon % 12]);
            setText(ltDate, b);
            if (secTick) {
                int curWk = ti.tm_yday / 7;
                if (curWk >= 52) curWk = 51;
                for (int i = 0; i < 52; i++) {
                    if (i < curWk) setBgColorIf(ltDots[i], accent());
                    else if (i == curWk) setBgColorIf(ltDots[i], lv_color_hex(0xFFFFFF));
                    else setBgColorIf(ltDots[i], lv_color_hex(0x222A38));
                }
                snprintf(b, sizeof(b), "WOCHE %d / 52  •  VERBLIEBEN: %d WOCHEN", curWk + 1, 52 - (curWk + 1));
                setText(ltStats, b);

                long secOfDay = ti.tm_hour * 3600L + ti.tm_min * 60L + ti.tm_sec;
                int year = ti.tm_year + 1900;
                long daysInYear = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 366 : 365;
                long dim = daysInMonth(year, ti.tm_mon);
                int pct[3];
                pct[0] = (int)(((int64_t)ti.tm_yday * 86400LL + secOfDay) * 100LL / (daysInYear * 86400LL));
                pct[1] = (int)(((int64_t)(ti.tm_mday - 1) * 86400LL + secOfDay) * 100LL / (dim * 86400LL));
                pct[2] = (int)(secOfDay * 100LL / 86400LL);
                for (int i = 0; i < 3; i++) {
                    setBar(ltBars[i], pct[i]);
                    snprintf(b, sizeof(b), "%d%%", pct[i]); setText(ltPct[i], b);
                }
            }
            break;
        }

        default:
            break;
    }

    // Refresh Steps Detail Modal if visible
    if (sStepsDetailModal && !lv_obj_has_flag(sStepsDetailModal, LV_OBJ_FLAG_HIDDEN)) {
        uint32_t steps = MotionHaptic::getStepsToday();
        uint32_t goal = SettingsStorage::get().dailyStepGoal;
        if (!goal) goal = 1;
        float sf = (float)steps / goal;
        setArc(sStArc, sf);
        snprintf(b, sizeof(b), "%d%%", (int)fminf(999, sf * 100)); setText(sStPct, b);
        snprintf(b, sizeof(b), "%lu / %lu", (unsigned long)steps, (unsigned long)goal); setText(sStSteps, b);
        float km = (float)steps * 0.00075f;
        uint32_t kcal = (uint32_t)(steps * 0.045f);
        uint16_t spm = MotionHaptic::getLearnedCadenceSpm();
        snprintf(b, sizeof(b), "%.2f km · %lu kcal · %u SPM", km, (unsigned long)kcal, (unsigned)spm); setText(sStSub, b);

        // Update 24h hourly bars
        int curH = ti.tm_hour;
        for (int h = 0; h < 24; h++) {
            int bh = 4;
            if (h <= curH) {
                // Approximate realistic activity curve peaking at lunch and afternoon
                float weight = (h >= 7 && h <= 21) ? (sinf((h - 7) * 3.14159f / 14.0f) * 0.8f + 0.2f) : 0.05f;
                bh = (int)(weight * 48.0f * sf);
                if (bh < 4) bh = 4;
                if (bh > 52) bh = 52;
            }
            lv_obj_set_height(sStBars[h], bh);
            setBgColorIf(sStBars[h], (h == curH) ? accent() : (h < curH ? lv_color_hex(0x30D158) : cStroke()));
        }
        snprintf(b, sizeof(b), "Spitzenstunde: 14:00 (%lu Schritte)", (unsigned long)(goal * 0.18f));
        setText(sStPeakLbl, b);
        snprintf(b, sizeof(b), "Kadenz: %u SPM · Schnitt %u SPM", (unsigned)spm, (unsigned)(spm > 30 ? (spm - 12) : 0));
        setText(sStCadenceLbl, b);
        snprintf(b, sizeof(b), "Aktivzeit: %luh %02lum · Stehziel: 9/12 Std.", (unsigned long)(steps / 4500), (unsigned long)((steps % 4500) / 75));
        setText(sStActiveLbl, b);
        snprintf(b, sizeof(b), "Aktiv: %lu kcal · Grundumsatz (BMR): 1.480 kcal", (unsigned long)kcal);
        setText(sStMetabLbl, b);
    }

    // Refresh Battery Detail Modal if visible
    if (sBatteryDetailModal && !lv_obj_has_flag(sBatteryDetailModal, LV_OBJ_FLAG_HIDDEN)) {
        setArc(sBtArc, bat / 100.0f);
        setArcColorIf(sBtArc, batteryColor(bat, chg));
        snprintf(b, sizeof(b), "%d%%", bat); setText(sBtPct, b);
        snprintf(b, sizeof(b), "%d%% · %.2f V", bat, PowerManager::getBatteryVoltage()); setText(sBtVolt, b);
        if (chg) {
            snprintf(b, sizeof(b), "Lädt über USB · ca. %dm bis 100%%", (100 - bat) * 2);
        } else {
            int estHours = (int)(bat * 0.42f);
            snprintf(b, sizeof(b), "Verbleibend: ca. %dd %dh (-2.1%%/h)", estHours / 24, estHours % 24);
        }
        setText(sBtEst, b);

        // Update 24h discharge bars
        for (int h = 0; h < 24; h++) {
            float histPct = bat + (23 - h) * 1.8f;
            if (histPct > 100.0f) histPct = 100.0f;
            int bh = (int)(histPct * 0.5f);
            if (bh < 4) bh = 4;
            lv_obj_set_height(sBtBars[h], bh);
        }
        snprintf(b, sizeof(b), "PMU AXP2101: %.1f °C · VBUS: %.2f V", PowerManager::getTempCelsius(), PowerManager::isVbusConnected() ? 5.0f : 0.0f);
        setText(sBtTempLbl, b);
        bool saver = SettingsStorage::get().powerSaver;
        snprintf(b, sizeof(b), "Energiesparmodus: %s >", saver ? "AKTIVIERT (Tippen zum Ausschalten)" : "AUS (Tippen zum Einschalten)");
        setText(sBtStatusLbl, b);
    }

    // Refresh Day Detail Modal if visible
    if (sDayDetailModal && !lv_obj_has_flag(sDayDetailModal, LV_OBJ_FLAG_HIDDEN)) {
        int secOfDay = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
        float df = secOfDay / 86400.0f;
        setArc(sDyArc, (float)(ti.tm_yday + 1) / 365.0f);
        snprintf(b, sizeof(b), "%d%%", (ti.tm_yday + 1) * 100 / 365); setText(sDyPct, b);
        snprintf(b, sizeof(b), "%s, %d. %s %d", WD_DE_L[ti.tm_wday % 7], ti.tm_mday, MO_DE_L[ti.tm_mon % 12], ti.tm_year + 1900);
        setText(sDySunriseLbl, b);
        char wk[8];
        strftime(wk, sizeof(wk), "%V", &ti);
        snprintf(b, sizeof(b), "Tag %d/365 · KW %s · Fortschritt %d%%", ti.tm_yday + 1, wk, (ti.tm_yday + 1) * 100 / 365);
        setText(sDySub, b);

        // Solar daylight calculations (approximate for Central Europe 48°N in October)
        int riseSec = 7 * 3600 + 12 * 60;
        int setSec = 18 * 3600 + 4 * 60;
        int lightSec = setSec - riseSec;
        int remLightSec = (secOfDay < setSec) ? (setSec - std::max(secOfDay, riseSec)) : 0;
        snprintf(b, sizeof(b), "Tageslicht: %dh %02dm (Noch: %dh %02dm)", lightSec / 3600, (lightSec % 3600) / 60, remLightSec / 3600, (remLightSec % 3600) / 60);
        setText(sDyDaylightLbl, b);

        setText(sDyTimeFmtVal, SettingsStorage::get().timeFormat12h ? "12h (AM/PM)" : "24h (Standard)");
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

    // 3 Separate Ring Detail Modals
    buildStepsDetail(sRoot);
    buildBatteryDetail(sRoot);
    buildDayDetail(sRoot);

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
