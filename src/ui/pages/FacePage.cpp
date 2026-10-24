// ============================================================================
//  Watch faces:
//   0: MODULAR (digital hero time + 3 complications)
//   1: CHRONO (analog chronometer dial + sweeping hands)
//   2: MINIMAL (stacked oversized numerals)
//   3: ULTRA WAYFINDER (Apple Watch Ultra compass & adventure dial)
//   4: SKELETON TOURBILLON (Haute Horlogerie skeleton dial + animated balance)
//   5: CASIO (Retro DW-5600 Illuminator LCD)
//   6: BAUHAUS (Max Bill minimalist analog dial)
//   7: CYBERPUNK (Matrix HUD terminal)
//   8: WORDCLOCK (German typographic word clock)
//   9: NEON HORIZON (Synthwave 80s sunset perspective grid)
//  10: PILOT FLIEGER (B-Uhr Aviator cockpit dial)
//  11: DIVER PRO (Submariner 300M Diver rotating bezel)
//  12: RADAR TACTICAL (Tactical military sonar radar)
//  13: LIFETIME (Lifetime / Year / Month / Day progress bars)
// ============================================================================
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "MotionHaptic.h"
#include "RadioLoRa.h"
#include "CompanionServer.h"
#include "NotificationStore.h"
#include "GpsManager.h"
#include "Config.h"
#include <sys/time.h>
#include <math.h>

using namespace ui;

namespace pages {

static lv_obj_t* sRoot;
static lv_obj_t* sFaces[FACE_COUNT];
static int sShown = -1;

// Helper to update a 2-point line hand
static void updateLineHand(lv_obj_t* line, lv_point_precise_t pts[2], float cx, float cy, float deg, float len, float tail, float pad = 6.0f) {
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

// ---------------------------------------------------------------- WIDGET DEFS
// 0: Modular
static lv_obj_t *mStatus, *mWifi, *mLora, *mBatIcon, *mBatTxt, *mUnread;
static lv_obj_t *mHH, *mMM, *mSecBar, *mDate;
static lv_obj_t *mRing[3], *mRingVal[3], *mDetail[3];

// 1: Chrono
static lv_obj_t *cDial, *cHour, *cMin, *cSec, *cDate, *cDigital;
static lv_point_precise_t cPts[3][2];
static const int C_D = 372, C_R = C_D / 2;

// 2: Minimal
static lv_obj_t *nDate, *nHH, *nMM, *nBat;

// 3: Ultra Wayfinder
static lv_obj_t *wHH, *wMM, *wSec, *wDate, *wCoord, *wAlt, *wHeading;
static lv_obj_t *wRingSteps, *wStepsTxt, *wRingBat, *wBatTxt;

// 4: Skeleton Tourbillon
static lv_obj_t *tDial, *tHour, *tMin, *tSec, *tDate, *tPowerArc, *tPowerTxt;
static lv_obj_t *tTourbArm1, *tTourbArm2;
static lv_point_precise_t tPts[5][2];
static const int T_CX = 205, T_CY = 220;

// 5: Casio
static lv_obj_t *kDayBoxes[7], *kDayLabels[7], *k24h, *kDate, *kBarDots[10];
static lv_obj_t *kHH, *kMM, *kSS, *kBat;

// 6: Bauhaus
static lv_obj_t *bDial, *bHour, *bMin, *bSec, *bDate;
static lv_point_precise_t bPts[3][2];
static const int B_CX = 205, B_CY = 230;

// 7: Cyberpunk
static lv_obj_t *yHeader, *yHero, *ySec, *yStatus, *yDiag, *yBat;

// 8: WordClock
static lv_obj_t *wcWords[26];
static lv_obj_t *wcDots[4];

// 9: Neon Horizon
static lv_obj_t *nhSun, *nhHH, *nhMM, *nhSec, *nhDate, *nhBat;

// 10: Pilot Flieger
static lv_obj_t *pfDial, *pfHour, *pfMin, *pfSec, *pfDate;
static lv_point_precise_t pfPts[3][2];
static const int PF_CX = 205, PF_CY = 230;

// 11: Diver Pro
static lv_obj_t *dpDial, *dpHour, *dpMin, *dpSec, *dpDate;
static lv_point_precise_t dpPts[3][2];
static const int DP_CX = 205, DP_CY = 230;

// 12: Radar Tactical
static lv_obj_t *rtScope, *rtSweep, *rtBlip[3], *rtHeading, *rtTime, *rtBat;
static lv_point_precise_t rtPts[2];
static const int RT_CX = 205, RT_CY = 220;

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
    lv_obj_set_pos(mStatus, M, 24);

    mUnread = box(f, 8, 8);
    lv_obj_set_style_radius(mUnread, 4, 0);
    lv_obj_set_style_bg_opa(mUnread, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mUnread, accent(), 0);
    accReg(mUnread, ACC_BG);
    lv_obj_align(mUnread, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t* st = box(f, 170, 28);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -M, 18);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 10, 0);
    mWifi = label(st, F.icoS, cText3(), ICO_WIFI);
    mLora = label(st, F.icoS, cText3(), ICO_PODCASTS);
    mBatIcon = label(st, F.icoS, cText2(), ICO_BATTERY_FULL);
    mBatTxt = label(st, F.small, cText2(), "--%");

    lv_obj_t* hero = box(f, SCR_W, 150);
    lv_obj_set_pos(hero, 0, 60);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 4, 0);
    mHH = label(hero, F.hero, cText(), "00");
    lv_obj_t* colon = label(hero, F.hero, accent(), ":");
    accReg(colon, ACC_TEXT);
    mMM = label(hero, F.hero, cText(), "00");

    mSecBar = lv_bar_create(f);
    lv_obj_set_size(mSecBar, 300, 4);
    lv_obj_align(mSecBar, LV_ALIGN_TOP_MID, 0, 218);
    lv_bar_set_range(mSecBar, 0, 59999);
    lv_obj_set_style_bg_color(mSecBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mSecBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(mSecBar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(mSecBar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(mSecBar, accent(), LV_PART_INDICATOR);
    accReg(mSecBar, ACC_BG, LV_PART_INDICATOR);

    mDate = label(f, F.body, cText2(), "");
    lv_obj_set_style_text_letter_space(mDate, 1, 0);
    lv_obj_align(mDate, LV_ALIGN_TOP_MID, 0, 240);

    static const char* caps[3] = {"ACTIVITY", "BATTERY", "DAY"};
    const int cw = (CW - 20) / 3, ch = 176, cy = 300;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = card(f, cw, ch);
        lv_obj_set_pos(c, M + i * (cw + 10), cy);
        lv_color_t col = (i == 1) ? cOk() : (i == 2 ? cInfo() : accent());
        mRing[i] = ring(c, 82, 8, col);
        if (i == 0) accReg(mRing[i], ACC_ARC, LV_PART_INDICATOR);
        lv_obj_align(mRing[i], LV_ALIGN_TOP_MID, 0, 16);
        mRingVal[i] = label(mRing[i], F.numS, cText(), "0");
        lv_obj_align(mRingVal[i], LV_ALIGN_CENTER, 0, 0);
        mDetail[i] = label(c, F.small, cText(), "");
        lv_obj_align(mDetail[i], LV_ALIGN_TOP_MID, 0, 106);
        lv_obj_t* cp = caption(c, caps[i], cText3());
        lv_obj_align(cp, LV_ALIGN_BOTTOM_MID, 0, -14);
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
    lv_obj_t* brand = caption(bezel, "KRONOS", cText3());
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

    auto hand = [&](int i, int w, lv_color_t c) {
        lv_obj_t* l = lv_line_create(cDial);
        lv_obj_set_style_line_width(l, w, 0);
        lv_obj_set_style_line_color(l, c, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        cPts[i][0] = {C_R, C_R};
        cPts[i][1] = {C_R, C_R};
        lv_line_set_points(l, cPts[i], 2);
        return l;
    };
    cHour = hand(0, 10, cText());
    cMin = hand(1, 6, cText());
    cSec = hand(2, 3, accent());
    accReg(cSec, ACC_LINE);

    lv_obj_t* cap = box(cDial, 22, 22);
    lv_obj_center(cap);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cap, accent(), 0);
    accReg(cap, ACC_BG);
    lv_obj_t* pin = box(cap, 8, 8);
    lv_obj_center(pin);
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pin, cBg(), 0);

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
    lv_obj_align(nHH, LV_ALIGN_TOP_MID, 0, 48);
    nMM = label(f, F.giant, accent(), "00");
    accReg(nMM, ACC_TEXT);
    lv_obj_align(nMM, LV_ALIGN_TOP_MID, 0, 250);
    nBat = caption(f, "", cText3());
    lv_obj_align(nBat, LV_ALIGN_BOTTOM_MID, 0, -22);
}

// ============================================================================
// 3: ULTRA WAYFINDER (Apple Watch Ultra Style)
// ============================================================================
static void buildUltraWayfinder(lv_obj_t* f) {
    const lv_color_t COL_ULTRA = lv_color_hex(0xFF5500); // Iconic Ultra Orange

    // Outer Compass Azimuth Ring Container
    lv_obj_t* ringBox = box(f, 386, 386);
    lv_obj_align(ringBox, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_radius(ringBox, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ringBox, 2, 0);
    lv_obj_set_style_border_color(ringBox, cStroke(), 0);

    // Cardinal Points
    lv_obj_t* lblN = label(ringBox, F.title, COL_ULTRA, "N");
    lv_obj_align(lblN, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t* lblE = label(ringBox, F.title, cText2(), "E");
    lv_obj_align(lblE, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t* lblS = label(ringBox, F.title, cText2(), "S");
    lv_obj_align(lblS, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t* lblW = label(ringBox, F.title, cText2(), "W");
    lv_obj_align(lblW, LV_ALIGN_LEFT_MID, 8, 0);

    // GPS & Elevation Complications at top
    wCoord = caption(f, "48°08'N 11°34'E", cText2());
    lv_obj_align(wCoord, LV_ALIGN_TOP_MID, 0, 52);

    wAlt = caption(f, "ELEV 520M • 1018 hPa", cText3());
    lv_obj_align(wAlt, LV_ALIGN_TOP_MID, 0, 78);

    // Hero Digital Time (Center)
    lv_obj_t* hero = box(f, SCR_W, 110);
    lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 4, 0);
    wHH = label(hero, F.hero, cText(), "00");
    lv_obj_t* col = label(hero, F.hero, COL_ULTRA, ":");
    wMM = label(hero, F.hero, cText(), "00");
    wSec = label(hero, F.numS, COL_ULTRA, "00");
    lv_obj_set_style_pad_left(wSec, 8, 0);

    // Date & Heading Banner
    wDate = label(f, F.title, COL_ULTRA, "FR 24 OKT");
    lv_obj_align(wDate, LV_ALIGN_TOP_MID, 0, 230);

    wHeading = caption(f, "BEARING 034° NE", cText2());
    lv_obj_align(wHeading, LV_ALIGN_TOP_MID, 0, 265);

    // Bottom Complications: Activity (Steps) & Battery
    const int cw = (CW - 16) / 2, ch = 140, cy = 310;

    // Card Left: Activity
    lv_obj_t* cAct = card(f, cw, ch);
    lv_obj_set_pos(cAct, M, cy);
    wRingSteps = ring(cAct, 72, 7, COL_ULTRA);
    lv_obj_align(wRingSteps, LV_ALIGN_TOP_MID, 0, 12);
    wStepsTxt = label(wRingSteps, F.numS, cText(), "0");
    lv_obj_align(wStepsTxt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t* capAct = caption(cAct, "DAILY STEPS", cText3());
    lv_obj_align(capAct, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Card Right: Battery
    lv_obj_t* cBat = card(f, cw, ch);
    lv_obj_set_pos(cBat, M + cw + 16, cy);
    wRingBat = ring(cBat, 72, 7, cOk());
    lv_obj_align(wRingBat, LV_ALIGN_TOP_MID, 0, 12);
    wBatTxt = label(wRingBat, F.numS, cText(), "0%");
    lv_obj_align(wBatTxt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t* capBat = caption(cBat, "BATTERY", cText3());
    lv_obj_align(capBat, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ============================================================================
// 4: SKELETON TOURBILLON (Haute Horlogerie Luxury Dial)
// ============================================================================
static void buildSkeletonTourbillon(lv_obj_t* f) {
    const int D_SZ = 376;
    tDial = box(f, D_SZ, D_SZ);
    lv_obj_align(tDial, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_radius(tDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tDial, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_bg_opa(tDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tDial, 2, 0);
    lv_obj_set_style_border_color(tDial, lv_color_hex(0x2A3245), 0);

    // Dial Branding
    lv_obj_t* brand = caption(tDial, "KRONOS • GENEVE", lv_color_hex(0xDAA520)); // Polished Gold
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_t* sub = caption(tDial, "TOURBILLON", cText3());
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 72);

    // Power Reserve Indicator at 9 o'clock
    tPowerArc = lv_arc_create(tDial);
    lv_obj_set_size(tPowerArc, 82, 82);
    lv_obj_set_pos(tPowerArc, 26, 140);
    lv_arc_set_rotation(tPowerArc, 135);
    lv_arc_set_bg_angles(tPowerArc, 0, 180);
    lv_arc_set_range(tPowerArc, 0, 100);
    lv_obj_set_style_arc_width(tPowerArc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(tPowerArc, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(tPowerArc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(tPowerArc, lv_color_hex(0xDAA520), LV_PART_INDICATOR);
    lv_obj_remove_style(tPowerArc, NULL, LV_PART_KNOB);
    tPowerTxt = label(tDial, F.cap, lv_color_hex(0xDAA520), "40H");
    lv_obj_set_pos(tPowerTxt, 56, 172);

    // Date Window at 3 o'clock
    lv_obj_t* dBox = card(tDial, 48, 30);
    lv_obj_set_pos(dBox, D_SZ - 76, 166);
    lv_obj_set_style_radius(dBox, 6, 0);
    tDate = label(dBox, F.small, cText(), "24");
    lv_obj_center(tDate);

    // Tourbillon Cutout Opening at 6 o'clock
    const int TB_SZ = 110;
    const int TB_X = (D_SZ - TB_SZ) / 2, TB_Y = 230;
    lv_obj_t* tbWell = box(tDial, TB_SZ, TB_SZ);
    lv_obj_set_pos(tbWell, TB_X, TB_Y);
    lv_obj_set_style_radius(tbWell, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tbWell, lv_color_hex(0x020305), 0);
    lv_obj_set_style_bg_opa(tbWell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tbWell, 2, 0);
    lv_obj_set_style_border_color(tbWell, lv_color_hex(0xDAA520), 0);

    // Tourbillon Bridge Bar
    lv_obj_t* bridge = box(tbWell, TB_SZ - 16, 4);
    lv_obj_center(bridge);
    lv_obj_set_style_bg_color(bridge, lv_color_hex(0x7A8599), 0);
    lv_obj_set_style_bg_opa(bridge, LV_OPA_COVER, 0);

    // Tourbillon oscillating arms (animated balance wheel)
    auto mkArm = [&](int i, lv_color_t col) {
        lv_obj_t* l = lv_line_create(tbWell);
        lv_obj_set_style_line_width(l, 3, 0);
        lv_obj_set_style_line_color(l, col, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        tPts[i][0] = {TB_SZ / 2, TB_SZ / 2};
        tPts[i][1] = {TB_SZ / 2, TB_SZ / 2};
        lv_line_set_points(l, tPts[i], 2);
        return l;
    };
    tTourbArm1 = mkArm(3, lv_color_hex(0xDAA520));
    tTourbArm2 = mkArm(4, lv_color_hex(0xDAA520));

    // Dial Hands: Hours, Minutes, Seconds
    auto mkHand = [&](int i, int w, lv_color_t col) {
        lv_obj_t* l = lv_line_create(tDial);
        lv_obj_set_style_line_width(l, w, 0);
        lv_obj_set_style_line_color(l, col, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        tPts[i][0] = {D_SZ / 2, D_SZ / 2};
        tPts[i][1] = {D_SZ / 2, D_SZ / 2};
        lv_line_set_points(l, tPts[i], 2);
        return l;
    };
    tHour = mkHand(0, 8, cText());
    tMin  = mkHand(1, 5, cText());
    tSec  = mkHand(2, 2, lv_color_hex(0xDAA520));

    // Center Cap
    lv_obj_t* cap = box(tDial, 18, 18);
    lv_obj_center(cap);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(0xDAA520), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);

    // Bottom Inscription
    lv_obj_t* swiss = caption(f, "SWISS CHRONOMETER • 28,800 VPH", cText3());
    lv_obj_align(swiss, LV_ALIGN_BOTTOM_MID, 0, -26);
}

// ============================================================================
// 5: CASIO (Retro DW-5600 Illuminator LCD)
// ============================================================================
static void buildCasio(lv_obj_t* f) {
    const lv_color_t LCD_BG   = lv_color_hex(0x9EAC8D); // Vintage Olive LCD
    const lv_color_t LCD_INK  = lv_color_hex(0x111611); // Deep LCD Black Ink
    const lv_color_t LCD_DIM  = lv_color_hex(0x8D9B7C); // Unlit segment ghost

    // Outer Resin Case
    lv_obj_t* cCase = card(f, CW, 420);
    lv_obj_align(cCase, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(cCase, lv_color_hex(0x15181C), 0);
    lv_obj_set_style_border_width(cCase, 2, 0);
    lv_obj_set_style_border_color(cCase, lv_color_hex(0x1E56A0), 0); // Blue accent

    // Bezel Text
    lv_obj_t* casio = label(cCase, F.title, cText(), "CASIO");
    lv_obj_set_pos(casio, 20, 12);
    lv_obj_t* illum = label(cCase, F.small, lv_color_hex(0xF5A623), "ILLUMINATOR");
    lv_obj_align(illum, LV_ALIGN_TOP_RIGHT, -20, 14);

    // Inner Olive LCD Panel
    lv_obj_t* lcd = box(cCase, CW - 36, 300);
    lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_radius(lcd, 16, 0);
    lv_obj_set_style_bg_color(lcd, LCD_BG, 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, LCD_INK, 0);

    // Day of Week Banner
    static const char* dNames[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    for (int i = 0; i < 7; i++) {
        kDayBoxes[i] = box(lcd, 28, 20);
        lv_obj_set_pos(kDayBoxes[i], 16 + i * 38, 14);
        lv_obj_set_style_radius(kDayBoxes[i], 4, 0);
        kDayLabels[i] = label(kDayBoxes[i], F.cap, LCD_DIM, dNames[i]);
        lv_obj_center(kDayLabels[i]);
    }

    // 24H Badge & Date
    k24h = card(lcd, 36, 22);
    lv_obj_set_pos(k24h, 20, 48);
    lv_obj_set_style_bg_color(k24h, LCD_INK, 0);
    lv_obj_t* l24 = label(k24h, F.cap, LCD_BG, "24H");
    lv_obj_center(l24);

    kDate = label(lcd, F.title, LCD_INK, "10-24");
    lv_obj_align(kDate, LV_ALIGN_TOP_RIGHT, -24, 48);

    // Dot Matrix Alarm Graph
    for (int g = 0; g < 10; g++) {
        kBarDots[g] = box(lcd, 6, 6);
        lv_obj_set_pos(kBarDots[g], 16 + g * 10, 80);
        lv_obj_set_style_radius(kBarDots[g], 2, 0);
        lv_obj_set_style_bg_opa(kBarDots[g], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(kBarDots[g], LCD_DIM, 0);
    }

    // Large Digital Time
    lv_obj_t* tRow = box(lcd, CW - 60, 100);
    lv_obj_align(tRow, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_flex_flow(tRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    kHH = label(tRow, F.hero, LCD_INK, "12");
    lv_obj_t* col = label(tRow, F.hero, LCD_INK, ":");
    kMM = label(tRow, F.hero, LCD_INK, "34");
    kSS = label(tRow, F.numS, LCD_INK, "56");
    lv_obj_set_style_pad_left(kSS, 8, 0);

    // Bottom Water Resist Inscription
    lv_obj_t* wr = label(cCase, F.small, lv_color_hex(0xE53935), "WATER 50M RESIST");
    lv_obj_set_pos(wr, 20, 375);
    kBat = label(cCase, F.small, cText2(), "LITHIUM 85%");
    lv_obj_align(kBat, LV_ALIGN_TOP_RIGHT, -20, 375);
}

// ============================================================================
// 6: BAUHAUS (Max Bill Minimalist Analog)
// ============================================================================
static void buildBauhaus(lv_obj_t* f) {
    const int D_SZ = 380;
    bDial = box(f, D_SZ, D_SZ);
    lv_obj_align(bDial, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_radius(bDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bDial, lv_color_hex(0x12151B), 0);
    lv_obj_set_style_bg_opa(bDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bDial, 1, 0);
    lv_obj_set_style_border_color(bDial, cStroke(), 0);

    // Bauhaus Logo
    lv_obj_t* logo = caption(bDial, "BAUHAUS", cText3());
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 68);

    // Bauhaus Hour Batons & Cardinal Red Pips
    for (int i = 0; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        float c = cosf(a), s = sinf(a);
        float r1 = 155.0f, r2 = 172.0f;
        lv_point_precise_t pts[2] = {
            {(lv_value_precise_t)(D_SZ / 2 + c * r1), (lv_value_precise_t)(D_SZ / 2 + s * r1)},
            {(lv_value_precise_t)(D_SZ / 2 + c * r2), (lv_value_precise_t)(D_SZ / 2 + s * r2)}
        };
        lv_obj_t* baton = lv_line_create(bDial);
        lv_obj_set_style_line_width(baton, (i % 3 == 0) ? 3 : 2, 0);
        lv_obj_set_style_line_color(baton, (i % 3 == 0) ? cText() : cText3(), 0);
        lv_line_set_points(baton, pts, 2);

        // Cardinal Red Pips at 12, 3, 6, 9
        if (i % 3 == 0) {
            lv_obj_t* pip = box(bDial, 6, 6);
            lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(pip, lv_color_hex(0xE53935), 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_pos(pip, (int)(D_SZ / 2 + c * 142.0f - 3), (int)(D_SZ / 2 + s * 142.0f - 3));
        }
    }

    // Date Window at 3 o'clock
    lv_obj_t* db = card(bDial, 44, 28);
    lv_obj_set_pos(db, D_SZ - 76, D_SZ / 2 - 14);
    bDate = label(db, F.small, cText(), "24");
    lv_obj_center(bDate);

    // Hands
    auto mkHand = [&](int i, int w, lv_color_t col) {
        lv_obj_t* l = lv_line_create(bDial);
        lv_obj_set_style_line_width(l, w, 0);
        lv_obj_set_style_line_color(l, col, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        bPts[i][0] = {D_SZ / 2, D_SZ / 2};
        bPts[i][1] = {D_SZ / 2, D_SZ / 2};
        lv_line_set_points(l, bPts[i], 2);
        return l;
    };
    bHour = mkHand(0, 6, cText());
    bMin  = mkHand(1, 4, cText());
    bSec  = mkHand(2, 2, lv_color_hex(0xE53935)); // Bauhaus Red second hand

    // Center Pin
    lv_obj_t* pin = box(bDial, 12, 12);
    lv_obj_center(pin);
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pin, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
}

// ============================================================================
// 7: CYBERPUNK (Matrix HUD Terminal)
// ============================================================================
static void buildCyberpunk(lv_obj_t* f) {
    const lv_color_t CYBER_GREEN = lv_color_hex(0x00FF66);
    const lv_color_t CYBER_CYAN  = lv_color_hex(0x00E5FF);

    // Grid Canvas Card
    lv_obj_t* pan = card(f, CW, 450);
    lv_obj_align(pan, LV_ALIGN_TOP_MID, 0, 20);

    // Corner HUD Brackets
    lv_obj_t* bTL = label(pan, F.title, CYBER_CYAN, "┌"); lv_obj_set_pos(bTL, 8, 4);
    lv_obj_t* bTR = label(pan, F.title, CYBER_CYAN, "┐"); lv_obj_align(bTR, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_t* bBL = label(pan, F.title, CYBER_CYAN, "└"); lv_obj_align(bBL, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_t* bBR = label(pan, F.title, CYBER_CYAN, "┘"); lv_obj_align(bBR, LV_ALIGN_BOTTOM_RIGHT, -8, -4);

    // Terminal Status Line
    yHeader = label(pan, F.small, CYBER_GREEN, "// SYS.KRONOS // v4.0 _");
    lv_obj_set_pos(yHeader, 24, 18);

    // Hero Bracketed Time
    lv_obj_t* timeRow = box(pan, CW - 32, 90);
    lv_obj_align(timeRow, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_flex_flow(timeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timeRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    label(timeRow, F.hero, CYBER_CYAN, "[ ");
    yHero = label(timeRow, F.hero, cText(), "14:32");
    ySec = label(timeRow, F.title, CYBER_GREEN, ":05");
    label(timeRow, F.hero, CYBER_CYAN, " ]");

    // System Diagnostics Lines
    yStatus = label(pan, F.body, CYBER_GREEN, "CORE: 240MHz • THREADS: 2");
    lv_obj_set_pos(yStatus, 24, 210);

    yDiag = label(pan, F.body, CYBER_CYAN, "CYCLE: 8420 STEPS • MESH: OK");
    lv_obj_set_pos(yDiag, 24, 250);

    yBat = label(pan, F.body, cText(), "PWR_BUS: [0x55] 85% ONLINE");
    lv_obj_set_pos(yBat, 24, 290);

    lv_obj_t* termBot = caption(pan, "SEC_LVL: MAXIMUM // HOST_READY", cText3());
    lv_obj_align(termBot, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// ============================================================================
// 8: WORDCLOCK (German Typographic Word Clock)
// ============================================================================
static void buildWordClock(lv_obj_t* f) {
    lv_obj_t* pan = card(f, CW, 450);
    lv_obj_align(pan, LV_ALIGN_TOP_MID, 0, 20);

    // Word matrix entries
    static const char* wText[26] = {
        "ES", "IST", "FÜNF",
        "ZEHN", "ZWANZIG",
        "VIERTEL", "VOR", "NACH",
        "HALB", "ZWÖLF", "ZWEI",
        "EINS", "DREI", "VIER",
        "SECHS", "ACHT", "SIEBEN",
        "NEUN", "ZEHN", "ELF", "UHR",
        "EIN"
    };

    // Row positions
    const int Y_BASE = 24, Y_STEP = 54;
    auto addW = [&](int idx, const char* txt, int x, int y) {
        wcWords[idx] = label(pan, F.title, cStroke(), txt);
        lv_obj_set_pos(wcWords[idx], x, y);
    };

    // Row 0
    addW(0, "ES", 20, Y_BASE);
    addW(1, "IST", 78, Y_BASE);
    addW(2, "FÜNF", 240, Y_BASE);

    // Row 1
    addW(3, "ZEHN", 20, Y_BASE + Y_STEP);
    addW(4, "ZWANZIG", 160, Y_BASE + Y_STEP);

    // Row 2
    addW(5, "VIERTEL", 20, Y_BASE + Y_STEP * 2);
    addW(6, "VOR", 190, Y_BASE + Y_STEP * 2);
    addW(7, "NACH", 258, Y_BASE + Y_STEP * 2);

    // Row 3
    addW(8, "HALB", 20, Y_BASE + Y_STEP * 3);
    addW(9, "ZWÖLF", 130, Y_BASE + Y_STEP * 3);
    addW(10, "ZWEI", 248, Y_BASE + Y_STEP * 3);

    // Row 4
    addW(11, "EINS", 20, Y_BASE + Y_STEP * 4);
    addW(12, "DREI", 134, Y_BASE + Y_STEP * 4);
    addW(13, "VIER", 248, Y_BASE + Y_STEP * 4);

    // Row 5
    addW(14, "SECHS", 20, Y_BASE + Y_STEP * 5);
    addW(15, "ACHT", 140, Y_BASE + Y_STEP * 5);
    addW(16, "SIEBEN", 232, Y_BASE + Y_STEP * 5);

    // Row 6
    addW(17, "NEUN", 20, Y_BASE + Y_STEP * 6);
    addW(18, "ZEHN", 120, Y_BASE + Y_STEP * 6);
    addW(19, "ELF", 200, Y_BASE + Y_STEP * 6);
    addW(20, "UHR", 268, Y_BASE + Y_STEP * 6);

    // 4 Corner Minute Dots
    wcDots[0] = box(pan, 8, 8); lv_obj_set_pos(wcDots[0], 10, 10);
    wcDots[1] = box(pan, 8, 8); lv_obj_align(wcDots[1], LV_ALIGN_TOP_RIGHT, -10, 10);
    wcDots[2] = box(pan, 8, 8); lv_obj_align(wcDots[2], LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    wcDots[3] = box(pan, 8, 8); lv_obj_align(wcDots[3], LV_ALIGN_BOTTOM_LEFT, 10, -10);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_radius(wcDots[i], 4, 0);
        lv_obj_set_style_bg_opa(wcDots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(wcDots[i], cStroke(), 0);
    }
}

// ============================================================================
// 9: NEON HORIZON (Synthwave 80s Sunset Grid)
// ============================================================================
static void buildNeonHorizon(lv_obj_t* f) {
    const lv_color_t SYNTH_MAG = lv_color_hex(0xF81F);
    const lv_color_t SYNTH_CYAN = lv_color_hex(0x00E5FF);
    const lv_color_t SYNTH_YEL = lv_color_hex(0xFFDD00);

    // Retro Neon Sun
    nhSun = box(f, 160, 160);
    lv_obj_align(nhSun, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(nhSun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(nhSun, SYNTH_MAG, 0);
    lv_obj_set_style_bg_opa(nhSun, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nhSun, 3, 0);
    lv_obj_set_style_border_color(nhSun, SYNTH_YEL, 0);

    // Horizon Separator Line
    lv_obj_t* hLine = box(f, SCR_W, 3);
    lv_obj_set_pos(hLine, 0, 230);
    lv_obj_set_style_bg_color(hLine, SYNTH_CYAN, 0);
    lv_obj_set_style_bg_opa(hLine, LV_OPA_COVER, 0);

    // Hero Digital Time
    lv_obj_t* tRow = box(f, SCR_W, 90);
    lv_obj_align(tRow, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_flex_flow(tRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    nhHH = label(tRow, F.hero, cText(), "12");
    lv_obj_t* col = label(tRow, F.hero, SYNTH_CYAN, ":");
    nhMM = label(tRow, F.hero, cText(), "34");
    nhSec = label(tRow, F.title, SYNTH_YEL, "56");
    lv_obj_set_style_pad_left(nhSec, 8, 0);

    // Bottom Synthwave Grid Lines
    for (int y = 250; y < 460; y += 32) {
        lv_obj_t* gl = box(f, SCR_W, 1);
        lv_obj_set_pos(gl, 0, y);
        lv_obj_set_style_bg_color(gl, SYNTH_MAG, 0);
        lv_obj_set_style_bg_opa(gl, LV_OPA_COVER, 0);
    }

    nhDate = label(f, F.title, SYNTH_CYAN, "SYNTHWAVE 1984");
    lv_obj_align(nhDate, LV_ALIGN_BOTTOM_MID, 0, -40);
    nhBat = caption(f, "BATTERY 85% • SYNTH_ACTIVE", cText2());
    lv_obj_align(nhBat, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// ============================================================================
// 10: PILOT FLIEGER (B-Uhr Aviator Cockpit)
// ============================================================================
static void buildPilotFlieger(lv_obj_t* f) {
    const int D_SZ = 380;
    pfDial = box(f, D_SZ, D_SZ);
    lv_obj_align(pfDial, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(pfDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pfDial, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(pfDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pfDial, 2, 0);
    lv_obj_set_style_border_color(pfDial, cStroke(), 0);

    // Aviation 12 o'clock orientation triangle & two pips
    lv_obj_t* m12A = box(pfDial, 18, 12);
    lv_obj_align(m12A, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_radius(m12A, 4, 0);
    lv_obj_set_style_bg_color(m12A, lv_color_hex(0x76FF03), 0);
    lv_obj_set_style_bg_opa(m12A, LV_OPA_COVER, 0);

    lv_obj_t* dotL = box(pfDial, 6, 6);
    lv_obj_set_style_radius(dotL, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dotL, lv_color_hex(0x76FF03), 0);
    lv_obj_set_style_bg_opa(dotL, LV_OPA_COVER, 0);
    lv_obj_align(dotL, LV_ALIGN_TOP_MID, -16, 21);

    lv_obj_t* dotR = box(pfDial, 6, 6);
    lv_obj_set_style_radius(dotR, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dotR, lv_color_hex(0x76FF03), 0);
    lv_obj_set_style_bg_opa(dotR, LV_OPA_COVER, 0);
    lv_obj_align(dotR, LV_ALIGN_TOP_MID, 16, 21);

    // Outer Minute Numbers (05..55)
    static const char* mins[12] = {"", "05", "10", "15", "20", "25", "30", "35", "40", "45", "50", "55"};
    for (int i = 1; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* m = label(pfDial, F.small, cText(), mins[i]);
        lv_obj_set_pos(m, (int)(D_SZ / 2 + cosf(a) * 148.0f - 10), (int)(D_SZ / 2 + sinf(a) * 148.0f - 8));
    }

    // Inner Hour Ring (1..12)
    lv_obj_t* inRing = box(pfDial, 180, 180);
    lv_obj_center(inRing);
    lv_obj_set_style_radius(inRing, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(inRing, 1, 0);
    lv_obj_set_style_border_color(inRing, cStroke(), 0);

    static const char* hrs[12] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
    for (int i = 0; i < 12; i++) {
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* h = label(pfDial, F.small, lv_color_hex(0x76FF03), hrs[i]);
        lv_obj_set_pos(h, (int)(D_SZ / 2 + cosf(a) * 70.0f - 8), (int)(D_SZ / 2 + sinf(a) * 70.0f - 8));
    }

    // Date
    lv_obj_t* db = card(pfDial, 44, 26);
    lv_obj_set_pos(db, D_SZ - 68, D_SZ / 2 - 13);
    pfDate = label(db, F.small, cText(), "24");
    lv_obj_center(pfDate);

    // Hands
    auto mkHand = [&](int i, int w, lv_color_t col) {
        lv_obj_t* l = lv_line_create(pfDial);
        lv_obj_set_style_line_width(l, w, 0);
        lv_obj_set_style_line_color(l, col, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        pfPts[i][0] = {D_SZ / 2, D_SZ / 2};
        pfPts[i][1] = {D_SZ / 2, D_SZ / 2};
        lv_line_set_points(l, pfPts[i], 2);
        return l;
    };
    pfHour = mkHand(0, 10, lv_color_hex(0x76FF03));
    pfMin  = mkHand(1, 6, lv_color_hex(0x76FF03));
    pfSec  = mkHand(2, 2, cText());

    lv_obj_t* cap = box(pfDial, 16, 16);
    lv_obj_center(cap);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, cText(), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
}

// ============================================================================
// 11: DIVER PRO (Submariner 300M Diver)
// ============================================================================
static void buildDiverPro(lv_obj_t* f) {
    const int D_SZ = 380;
    dpDial = box(f, D_SZ, D_SZ);
    lv_obj_align(dpDial, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(dpDial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dpDial, lv_color_hex(0x060C14), 0);
    lv_obj_set_style_bg_opa(dpDial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dpDial, 4, 0);
    lv_obj_set_style_border_color(dpDial, lv_color_hex(0x1F2937), 0);

    // Inverted Triangle / Double Pip at 12 o'clock
    lv_obj_t* pip12 = box(dpDial, 22, 14);
    lv_obj_align(pip12, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_radius(pip12, 4, 0);
    lv_obj_set_style_bg_color(pip12, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(pip12, LV_OPA_COVER, 0);

    // Circular Lume Plots at 1, 2, 4, 5, 7, 8, 10, 11
    for (int i = 0; i < 12; i++) {
        if (i == 0 || i == 3 || i == 6 || i == 9) continue;
        float a = (i * 30.0f - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* dot = box(dpDial, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, (int)(D_SZ / 2 + cosf(a) * 152.0f - 6), (int)(D_SZ / 2 + sinf(a) * 152.0f - 6));
    }

    // Baton markers at 6 and 9
    auto mkBaton = [&](float deg) {
        float a = (deg - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_t* b = box(dpDial, 20, 8);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_pos(b, (int)(D_SZ / 2 + cosf(a) * 150.0f - 10), (int)(D_SZ / 2 + sinf(a) * 150.0f - 4));
    };
    mkBaton(180.0f); // 6 o'clock
    mkBaton(270.0f); // 9 o'clock

    // Date at 3 o'clock
    lv_obj_t* dBox = card(dpDial, 44, 28);
    lv_obj_set_pos(dBox, D_SZ - 70, D_SZ / 2 - 14);
    dpDate = label(dBox, F.small, cText(), "24");
    lv_obj_center(dpDate);

    // Inscriptions
    lv_obj_t* brand = caption(dpDial, "KRONOS DIVER", cText());
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_t* depth = caption(dpDial, "1000ft = 300m", lv_color_hex(0x00E5FF));
    lv_obj_align(depth, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_t* cert = caption(dpDial, "SUPERLATIVE CHRONOMETER", cText3());
    lv_obj_align(cert, LV_ALIGN_TOP_MID, 0, 260);

    // Hands
    auto mkHand = [&](int i, int w, lv_color_t col) {
        lv_obj_t* l = lv_line_create(dpDial);
        lv_obj_set_style_line_width(l, w, 0);
        lv_obj_set_style_line_color(l, col, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        dpPts[i][0] = {D_SZ / 2, D_SZ / 2};
        dpPts[i][1] = {D_SZ / 2, D_SZ / 2};
        lv_line_set_points(l, dpPts[i], 2);
        return l;
    };
    dpHour = mkHand(0, 9, cText());
    dpMin  = mkHand(1, 5, cText());
    dpSec  = mkHand(2, 2, lv_color_hex(0x00E5FF));
}

// ============================================================================
// 12: RADAR TACTICAL (Tactical Military Sonar Radar)
// ============================================================================
static void buildRadarTactical(lv_obj_t* f) {
    const lv_color_t RADAR_GRN = lv_color_hex(0x00FF44);
    const lv_color_t RADAR_DIM = lv_color_hex(0x0A3A1A);

    const int R_SZ = 370;
    rtScope = box(f, R_SZ, R_SZ);
    lv_obj_align(rtScope, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_radius(rtScope, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rtScope, lv_color_hex(0x020E06), 0);
    lv_obj_set_style_bg_opa(rtScope, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rtScope, 2, 0);
    lv_obj_set_style_border_color(rtScope, RADAR_GRN, 0);

    // Concentric Range Rings (10km, 25km, 50km)
    int rRad[3] = {100, 200, 300};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* ringObj = box(rtScope, rRad[i], rRad[i]);
        lv_obj_center(ringObj);
        lv_obj_set_style_radius(ringObj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ringObj, 1, 0);
        lv_obj_set_style_border_color(ringObj, RADAR_DIM, 0);
    }

    // Crosshairs
    lv_obj_t* chH = box(rtScope, R_SZ - 16, 1); lv_obj_center(chH);
    lv_obj_set_style_bg_color(chH, RADAR_DIM, 0); lv_obj_set_style_bg_opa(chH, LV_OPA_COVER, 0);
    lv_obj_t* chV = box(rtScope, 1, R_SZ - 16); lv_obj_center(chV);
    lv_obj_set_style_bg_color(chV, RADAR_DIM, 0); lv_obj_set_style_bg_opa(chV, LV_OPA_COVER, 0);

    // Sweeping Radar Line
    rtSweep = lv_line_create(rtScope);
    lv_obj_set_style_line_width(rtSweep, 3, 0);
    lv_obj_set_style_line_color(rtSweep, RADAR_GRN, 0);
    lv_obj_set_style_line_rounded(rtSweep, true, 0);
    rtPts[0] = {R_SZ / 2, R_SZ / 2};
    rtPts[1] = {R_SZ / 2, R_SZ / 2};
    lv_line_set_points(rtSweep, rtPts, 2);

    // Tactical Target Blips
    for (int i = 0; i < 3; i++) {
        rtBlip[i] = box(rtScope, 8, 8);
        lv_obj_set_style_radius(rtBlip[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(rtBlip[i], RADAR_GRN, 0);
        lv_obj_set_style_bg_opa(rtBlip[i], LV_OPA_COVER, 0);
    }
    lv_obj_set_pos(rtBlip[0], 120, 100);
    lv_obj_set_pos(rtBlip[1], 240, 160);
    lv_obj_set_pos(rtBlip[2], 180, 270);

    // Status Lines
    rtHeading = label(f, F.title, RADAR_GRN, "SCAN // AZIMUTH 045°");
    lv_obj_align(rtHeading, LV_ALIGN_TOP_MID, 0, 400);

    rtTime = label(f, F.small, cText(), "14:32:05 UTC • 50KM RNG");
    lv_obj_align(rtTime, LV_ALIGN_TOP_MID, 0, 435);
}

// ============================================================================
// 13: LIFETIME (Lifetime / Year / Month / Day Progress Bars)
// ============================================================================
static void buildLifetime(lv_obj_t* f) {
    // Top Digital Time
    ltTime = label(f, F.hero, cText(), "14:32");
    lv_obj_align(ltTime, LV_ALIGN_TOP_MID, 0, 16);
    ltDate = caption(f, "FREITAG, 24. OKTOBER", accent());
    lv_obj_align(ltDate, LV_ALIGN_TOP_MID, 0, 78);

    // 4 Progress Bars: Life, Year, Month, Day
    static const char* labels[4] = {"LIFETIME (80Y)", "CURRENT YEAR", "CURRENT MONTH", "CURRENT DAY"};
    static const lv_color_t colors[4] = {
        lv_color_hex(0xE53935), // Red Life
        lv_color_hex(0xF5A623), // Amber Year
        lv_color_hex(0x00E5FF), // Cyan Month
        lv_color_hex(0x76FF03)  // Green Day
    };

    const int Y_START = 110, BAR_H = 80;
    for (int i = 0; i < 4; i++) {
        lv_obj_t* c = card(f, CW, 70);
        lv_obj_set_pos(c, M, Y_START + i * BAR_H);

        lv_obj_t* cap = caption(c, labels[i], cText2());
        lv_obj_set_pos(cap, 16, 10);

        ltPct[i] = label(c, F.small, colors[i], "0%");
        lv_obj_align(ltPct[i], LV_ALIGN_TOP_RIGHT, -16, 10);

        ltBars[i] = lv_bar_create(c);
        lv_obj_set_size(ltBars[i], CW - 32, 8);
        lv_obj_align(ltBars[i], LV_ALIGN_BOTTOM_MID, 0, -12);
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
    if (sShown >= 0) lv_obj_fade_in(sFaces[idx], 260, 0);
    sShown = idx;
    nav::markDirty(nav::PG_FACE);
}

static void refresh(bool) {
    showFace(SettingsStorage::get().watchfaceIndex);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t secs = tv.tv_sec;
    struct tm ti;
    localtime_r(&secs, &ti);
    int ms = (int)(tv.tv_usec / 1000);

    char b[64];
    int bat = PowerManager::getBatteryPercent();
    bool chg = PowerManager::isCharging();

    float sec = ti.tm_sec + ms / 1000.0f;
    float min = ti.tm_min + sec / 60.0f;
    float hr  = (ti.tm_hour % 12) + min / 60.0f;

    switch (sShown) {
        case FACE_MODULAR: {
            snprintf(b, sizeof(b), "%s %02d %s", WD_S[ti.tm_wday % 7], ti.tm_mday, MO_S[ti.tm_mon % 12]);
            setText(mStatus, b);
            setHidden(mUnread, NotificationStore::unread() == 0);
            setTextColorIf(mWifi, CompanionServer::isApActive() ? cText2() : cText3());
            setTextColorIf(mLora, RadioLoRa::isReady() ? cText2() : cText3());
            setText(mBatIcon, batterySymbol(bat, chg));
            setTextColorIf(mBatIcon, batteryColor(bat, chg));
            snprintf(b, sizeof(b), "%d%%", bat);
            setText(mBatTxt, b);

            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(mHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(mMM, b);
            int msOfMin = ti.tm_sec * 1000 + ms;
            if (lv_bar_get_value(mSecBar) != msOfMin) lv_bar_set_value(mSecBar, msOfMin, LV_ANIM_OFF);
            snprintf(b, sizeof(b), "%s, %d %s", WD_L[ti.tm_wday % 7], ti.tm_mday, MO_L[ti.tm_mon % 12]);
            setText(mDate, b);

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
            if (chg) setText(mDetail[1], "CHARGING");
            else { snprintf(b, sizeof(b), "%.2f V", PowerManager::getBatteryVoltage()); setText(mDetail[1], b); }

            int secOfDay = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
            setArc(mRing[2], secOfDay / 86400.0f);
            snprintf(b, sizeof(b), "%d%%", secOfDay * 100 / 86400); setText(mRingVal[2], b);
            char wk[8];
            strftime(wk, sizeof(wk), "%V", &ti);
            snprintf(b, sizeof(b), "WEEK %s", wk); setText(mDetail[2], b);
            break;
        }

        case FACE_CHRONO: {
            updateLineHand(cHour, cPts[0], C_R, C_R, hr * 30.0f, 92, 14);
            updateLineHand(cMin,  cPts[1], C_R, C_R, min * 6.0f, 140, 16);
            updateLineHand(cSec,  cPts[2], C_R, C_R, sec * 6.0f, 152, 26);
            snprintf(b, sizeof(b), "%s %02d", WD_S[ti.tm_wday % 7], ti.tm_mday); setText(cDate, b);
            snprintf(b, sizeof(b), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec); setText(cDigital, b);
            break;
        }

        case FACE_MINIMAL: {
            snprintf(b, sizeof(b), "%s %d", WD_L[ti.tm_wday % 7], ti.tm_mday); setText(nDate, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(nHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nMM, b);
            snprintf(b, sizeof(b), "%d%%%s", bat, chg ? "  CHARGING" : ""); setText(nBat, b);
            break;
        }

        case FACE_ULTRA_WAYFINDER: {
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(wHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(wMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(wSec, b);
            snprintf(b, sizeof(b), "%s %02d %s", WD_S[ti.tm_wday % 7], ti.tm_mday, MO_S[ti.tm_mon % 12]);
            setText(wDate, b);

            if (GpsManager::hasFix()) {
                snprintf(b, sizeof(b), "%.2f°N  %.2f°E", GpsManager::latitude(), GpsManager::longitude());
                setText(wCoord, b);
                snprintf(b, sizeof(b), "ELEV %.0fM • HDOP %.1f", GpsManager::altitudeM(), GpsManager::hdop());
                setText(wAlt, b);
            } else {
                setText(wCoord, "48°08'N  11°34'E");
                setText(wAlt, "ELEV 520M • 1018 hPa");
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
            const int D_SZ = 376;
            updateLineHand(tHour, tPts[0], D_SZ / 2, D_SZ / 2, hr * 30.0f, 96, 16);
            updateLineHand(tMin,  tPts[1], D_SZ / 2, D_SZ / 2, min * 6.0f, 142, 18);
            updateLineHand(tSec,  tPts[2], D_SZ / 2, D_SZ / 2, sec * 6.0f, 154, 26);

            // Tourbillon balance wheel oscillation
            float oscDeg = sinf((float)ms * 0.008f + ti.tm_sec * 5.0f) * 140.0f;
            const int TB_SZ = 110;
            updateLineHand(tTourbArm1, tPts[3], TB_SZ / 2, TB_SZ / 2, oscDeg, 38, 38);
            updateLineHand(tTourbArm2, tPts[4], TB_SZ / 2, TB_SZ / 2, oscDeg + 90.0f, 38, 38);

            lv_arc_set_value(tPowerArc, bat);
            snprintf(b, sizeof(b), "%dH", (int)(bat * 40 / 100)); setText(tPowerTxt, b);
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
            snprintf(b, sizeof(b), "%02d-%02d", ti.tm_mon + 1, ti.tm_mday); setText(kDate, b);

            for (int g = 0; g < 10; g++) {
                setBgColorIf(kBarDots[g], (g <= (ti.tm_sec % 10)) ? lv_color_hex(0x111611) : lv_color_hex(0x8D9B7C));
            }

            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(kHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(kMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(kSS, b);
            snprintf(b, sizeof(b), "LITHIUM %d%%%s", bat, chg ? " CHG" : ""); setText(kBat, b);
            break;
        }

        case FACE_BAUHAUS: {
            const int D_SZ = 380;
            updateLineHand(bHour, bPts[0], D_SZ / 2, D_SZ / 2, hr * 30.0f, 92, 14);
            updateLineHand(bMin,  bPts[1], D_SZ / 2, D_SZ / 2, min * 6.0f, 144, 16);
            updateLineHand(bSec,  bPts[2], D_SZ / 2, D_SZ / 2, sec * 6.0f, 156, 28);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(bDate, b);
            break;
        }

        case FACE_CYBERPUNK: {
            snprintf(b, sizeof(b), "// SYS.KRONOS // v4.0 %s", (ti.tm_sec % 2) ? "_" : " ");
            setText(yHeader, b);
            snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); setText(yHero, b);
            snprintf(b, sizeof(b), ":%02d", ti.tm_sec); setText(ySec, b);

            uint32_t steps = MotionHaptic::getStepsToday();
            snprintf(b, sizeof(b), "CYCLE: %lu STEPS • MESH: OK", (unsigned long)steps);
            setText(yDiag, b);

            snprintf(b, sizeof(b), "PWR_BUS: [0x%02X] %d%% %s", bat, bat, chg ? "CHG" : "ONLINE");
            setText(yBat, b);
            break;
        }

        case FACE_WORDCLOCK: {
            int h = ti.tm_hour, m = ti.tm_min;
            int dispH = (m >= 25) ? ((h + 1) % 24) : h;
            dispH = dispH % 12;
            if (dispH == 0) dispH = 12;

            int mBlock = m / 5;
            int minMod = m % 5;

            // Update corner single-minute dots
            for (int i = 0; i < 4; i++) {
                setBgColorIf(wcDots[i], (minMod >= (i + 1)) ? lv_color_hex(0x00E5FF) : cStroke());
            }

            auto setW = [](int idx, bool on) {
                setTextColorIf(wcWords[idx], on ? cText() : cStroke());
            };

            setW(0, true);  // ES
            setW(1, true);  // IST
            setW(2, mBlock == 1 || mBlock == 5 || mBlock == 7 || mBlock == 11); // FÜNF (M)
            setW(3, mBlock == 2 || mBlock == 10);                               // ZEHN (M)
            setW(4, mBlock == 4 || mBlock == 8);                                // ZWANZIG
            setW(5, mBlock == 3 || mBlock == 9);                                // VIERTEL
            setW(6, mBlock == 5 || mBlock == 8 || mBlock == 9 || mBlock == 10 || mBlock == 11); // VOR
            setW(7, mBlock == 1 || mBlock == 2 || mBlock == 3 || mBlock == 4 || mBlock == 7);  // NACH
            setW(8, mBlock >= 5 && mBlock <= 7);                               // HALB
            setW(9, dispH == 12);                                              // ZWÖLF
            setW(10, dispH == 2);                                              // ZWEI
            setW(11, dispH == 1);                                              // EINS
            setW(12, dispH == 3);                                              // DREI
            setW(13, dispH == 4);                                              // VIER
            setW(14, dispH == 6);                                              // SECHS
            setW(15, dispH == 8);                                              // ACHT
            setW(16, dispH == 7);                                              // SIEBEN
            setW(17, dispH == 9);                                              // NEUN
            setW(18, dispH == 10);                                             // ZEHN (H)
            setW(19, dispH == 11);                                             // ELF
            setW(20, mBlock == 0);                                             // UHR
            break;
        }

        case FACE_NEON_HORIZON: {
            snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(nhHH, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nhMM, b);
            snprintf(b, sizeof(b), "%02d", ti.tm_sec);  setText(nhSec, b);
            snprintf(b, sizeof(b), "BATTERY %d%%%s", bat, chg ? " • CHARGING" : "");
            setText(nhBat, b);
            break;
        }

        case FACE_PILOT_FLIEGER: {
            const int D_SZ = 380;
            updateLineHand(pfHour, pfPts[0], D_SZ / 2, D_SZ / 2, hr * 30.0f, 92, 16);
            updateLineHand(pfMin,  pfPts[1], D_SZ / 2, D_SZ / 2, min * 6.0f, 148, 18);
            updateLineHand(pfSec,  pfPts[2], D_SZ / 2, D_SZ / 2, sec * 6.0f, 158, 28);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(pfDate, b);
            break;
        }

        case FACE_DIVER_PRO: {
            const int D_SZ = 380;
            updateLineHand(dpHour, dpPts[0], D_SZ / 2, D_SZ / 2, hr * 30.0f, 96, 16);
            updateLineHand(dpMin,  dpPts[1], D_SZ / 2, D_SZ / 2, min * 6.0f, 146, 18);
            updateLineHand(dpSec,  dpPts[2], D_SZ / 2, D_SZ / 2, sec * 6.0f, 156, 26);
            snprintf(b, sizeof(b), "%02d", ti.tm_mday); setText(dpDate, b);
            break;
        }

        case FACE_RADAR_TACTICAL: {
            const int R_SZ = 370;
            updateLineHand(rtSweep, rtPts, R_SZ / 2, R_SZ / 2, sec * 6.0f, 168, 0);
            snprintf(b, sizeof(b), "SCAN // AZIMUTH %03d°", (int)(sec * 6.0f) % 360);
            setText(rtHeading, b);
            snprintf(b, sizeof(b), "%02d:%02d:%02d UTC • BAT %d%%", ti.tm_hour, ti.tm_min, ti.tm_sec, bat);
            setText(rtTime, b);
            break;
        }

        case FACE_LIFETIME: {
            snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); setText(ltTime, b);
            snprintf(b, sizeof(b), "%s, %02d. %s", WD_L[ti.tm_wday % 7], ti.tm_mday, MO_L[ti.tm_mon % 12]);
            setText(ltDate, b);

            // 1. Life progress (e.g. 35yo / 80y = 43%)
            int lifePct = 43;
            lv_bar_set_value(ltBars[0], lifePct, LV_ANIM_OFF);
            snprintf(b, sizeof(b), "%d%%", lifePct); setText(ltPct[0], b);

            // 2. Year progress
            int yday = ti.tm_yday;
            int yrPct = (yday * 100) / 365;
            lv_bar_set_value(ltBars[1], yrPct, LV_ANIM_OFF);
            snprintf(b, sizeof(b), "%d%%", yrPct); setText(ltPct[1], b);

            // 3. Month progress
            int mday = ti.tm_mday;
            int mPct = (mday * 100) / 31;
            lv_bar_set_value(ltBars[2], mPct, LV_ANIM_OFF);
            snprintf(b, sizeof(b), "%d%%", mPct); setText(ltPct[2], b);

            // 4. Day progress
            int secOfDay = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
            int dayPct = (secOfDay * 100) / 86400;
            lv_bar_set_value(ltBars[3], dayPct, LV_ANIM_OFF);
            snprintf(b, sizeof(b), "%d%%", dayPct); setText(ltPct[3], b);
            break;
        }

        default:
            break;
    }
}

void faceNext() {
    SystemSettings& s = SettingsStorage::get();
    s.watchfaceIndex = (s.watchfaceIndex + 1) % FACE_COUNT;
    SettingsStorage::save();
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
