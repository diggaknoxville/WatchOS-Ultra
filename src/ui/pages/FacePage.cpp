// Watch faces: MODULAR (digital + complications), CHRONO (analog, pre-rendered
// dial + sweeping hands), MINIMAL (oversized numerals).
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "MotionHaptic.h"
#include "RadioLoRa.h"
#include "CompanionServer.h"
#include "NotificationStore.h"
#include "Config.h"
#include <sys/time.h>
#include <math.h>

using namespace ui;

namespace pages {

static lv_obj_t* sRoot;
static lv_obj_t* sFaces[FACE_COUNT];
static int sShown = -1;

// modular
static lv_obj_t *mStatus, *mWifi, *mLora, *mBatIcon, *mBatTxt, *mUnread;
static lv_obj_t *mHH, *mMM, *mSecBar, *mDate;
static lv_obj_t *mRing[3], *mRingVal[3], *mDetail[3];
// chrono
static lv_obj_t *cDial, *cHour, *cMin, *cSec, *cDate, *cDigital;
static lv_point_precise_t cPts[3][2];
static const int C_D = 372, C_R = C_D / 2;
// minimal
static lv_obj_t *nDate, *nHH, *nMM, *nBat;

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

// ---------------------------------------------------------------- MODULAR
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

// ---------------------------------------------------------------- CHRONO
// The dial (ticks, numerals, brand) is rendered once into an image; only the
// three hands are live objects, so a sweeping second hand costs a small blit.
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

    // bake the dial
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

// A line object spans from its origin to its furthest point, so it is placed on
// the bounding box of its stroke: moving a hand then only invalidates the small
// region around it instead of most of the dial.
static void setHand(lv_obj_t* line, int i, float deg, float len, float tail) {
    static int lastKey[3] = {-1, -1, -1};
    float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    float c = cosf(a), s = sinf(a);
    float x1 = C_R + c * len, y1 = C_R + s * len;
    float x0 = C_R - c * tail, y0 = C_R - s * tail;
    int key = (int)lroundf(x1 * 4) * 4096 + (int)lroundf(y1 * 4);       // quarter-pixel resolution
    if (key == lastKey[i]) return;
    lastKey[i] = key;
    const float pad = 8;                                                 // half stroke + AA
    int bx = (int)floorf(fminf(x0, x1) - pad), by = (int)floorf(fminf(y0, y1) - pad);
    cPts[i][0] = {(lv_value_precise_t)(x0 - bx), (lv_value_precise_t)(y0 - by)};
    cPts[i][1] = {(lv_value_precise_t)(x1 - bx), (lv_value_precise_t)(y1 - by)};
    lv_obj_set_pos(line, bx, by);
    lv_obj_set_size(line, (int)ceilf(fabsf(x1 - x0) + 2 * pad), (int)ceilf(fabsf(y1 - y0) + 2 * pad));
    lv_line_set_points(line, cPts[i], 2);
}

// ---------------------------------------------------------------- MINIMAL
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

// ---------------------------------------------------------------- refresh
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

    // sub-second phase straight from the system clock -> locked to the second
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t secs = tv.tv_sec;
    struct tm ti;
    localtime_r(&secs, &ti);
    int ms = (int)(tv.tv_usec / 1000);

    char b[64];
    int bat = PowerManager::getBatteryPercent();
    bool chg = PowerManager::isCharging();

    if (sShown == FACE_MODULAR) {
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
    } else if (sShown == FACE_CHRONO) {
        float sec = ti.tm_sec + ms / 1000.0f;
        float min = ti.tm_min + sec / 60.0f;
        float hr = (ti.tm_hour % 12) + min / 60.0f;
        setHand(cHour, 0, hr * 30.0f, 92, 14);
        setHand(cMin, 1, min * 6.0f, 140, 16);
        setHand(cSec, 2, sec * 6.0f, 152, 26);
        snprintf(b, sizeof(b), "%s %02d", WD_S[ti.tm_wday % 7], ti.tm_mday); setText(cDate, b);
        snprintf(b, sizeof(b), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec); setText(cDigital, b);
    } else {
        snprintf(b, sizeof(b), "%s %d", WD_L[ti.tm_wday % 7], ti.tm_mday); setText(nDate, b);
        snprintf(b, sizeof(b), "%02d", ti.tm_hour); setText(nHH, b);
        snprintf(b, sizeof(b), "%02d", ti.tm_min);  setText(nMM, b);
        snprintf(b, sizeof(b), "%d%%%s", bat, chg ? "  CHARGING" : ""); setText(nBat, b);
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
    sFaces[FACE_MODULAR] = faceRoot(); buildModular(sFaces[FACE_MODULAR]);
    sFaces[FACE_CHRONO]  = faceRoot(); buildChrono(sFaces[FACE_CHRONO]);
    sFaces[FACE_MINIMAL] = faceRoot(); buildMinimal(sFaces[FACE_MINIMAL]);
    for (int i = 0; i < FACE_COUNT; i++) lv_obj_add_flag(sFaces[i], LV_OBJ_FLAG_HIDDEN);
    showFace(SettingsStorage::get().watchfaceIndex);

    nav::PageDef d;
    d.root = sRoot;
    d.refresh = refresh;
    d.refreshMs = 0;            // live: every frame (seconds rail / sweeping hand)
    d.ttlMs = 1000;             // a neighbour snapshot of the face is good for 1 s
    nav::registerPage(nav::PG_FACE, d);
}

}  // namespace pages
