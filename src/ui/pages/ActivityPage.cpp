// Activity & BHI260AP Sensor Suite:
//  - View 0: Daily steps ring, adaptive cadence learning, hand-waving filter counter,
//            Smart Sleep status, distance / energy / goal and 7-day history.
//  - View 1: Digital Bullseye Bubble Level (Wasserwaage) & 6DoF Gyro-Compass with zero calibration.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "MotionHaptic.h"
#include "RtcManager.h"
#include "DisplayEngine.h"
#include <math.h>

using namespace ui;

namespace pages {

static const int BAR_W = 30, BAR_MAX = 82, BAR_BASE = 132;
static lv_obj_t *aModePill, *aModeTxt;
static lv_obj_t *aActWrap, *aLevelWrap;
static lv_obj_t *aRing, *aSteps, *aPct, *aDist, *aKcal, *aGoal, *aLearnLbl, *aGoalLine;
static lv_obj_t *aStatePill, *aStateTxt;
static lv_obj_t *aBars[7], *aDays[7];

// Bubble level & compass widgets
static lv_obj_t *sLvlRingOuter, *sLvlRingInner, *sLvlBubble, *sLvlReadout, *sCompReadout, *sCompNeedle;
static bool sShowLevel = false;

static int barGap() { return (CW - 40 - 7 * BAR_W) / 6; }

static void refresh(bool) {
    char b[64];
    setText(aModeTxt, sShowLevel ? "AKTIVITAET" : "WASSERWAAGE");
    setHidden(aActWrap, sShowLevel);
    setHidden(aLevelWrap, !sShowLevel);

    if (sShowLevel) {
        float roll = 0, pitch = 0, yaw = 0;
        MotionHaptic::getEuler(roll, pitch, yaw);
        // Clamp bubble displacement inside 160px bullseye circle (radius 80, max travel 64px for +/- 30 deg)
        float dx = roll * (64.0f / 30.0f);
        float dy = -pitch * (64.0f / 30.0f);
        float r = sqrtf(dx * dx + dy * dy);
        if (r > 64.0f && r > 0.01f) {
            dx = dx * 64.0f / r;
            dy = dy * 64.0f / r;
        }
        bool isLevel = (fabsf(roll) < 1.5f && fabsf(pitch) < 1.5f);
        lv_obj_set_pos(sLvlBubble, 96 + (int)lroundf(dx) - 14, 96 + (int)lroundf(dy) - 14);
        setBgColorIf(sLvlBubble, isLevel ? cOk() : accent());
        lv_obj_set_style_border_color(sLvlRingInner, isLevel ? cOk() : cText3(), 0);

        snprintf(b, sizeof(b), "Roll %+.1f°   ·   Pitch %+.1f°%s",
                 roll, pitch, isLevel ? "  [WAAGE]" : "");
        setText(sLvlReadout, b);

        static const char* DIRS[8] = {"N", "NO", "O", "SO", "S", "SW", "W", "NW"};
        int dIdx = ((int)lroundf(yaw / 45.0f)) & 7;
        snprintf(b, sizeof(b), "KOMPASS  %03d° %s", (int)lroundf(yaw) % 360, DIRS[dIdx]);
        setText(sCompReadout, b);

        // Position compass indicator dot on outer ring
        float rad = (yaw - 90.0f) * (float)M_PI / 180.0f;
        int cx = 96 + (int)lroundf(cosf(rad) * 88.0f) - 6;
        int cy = 96 + (int)lroundf(sinf(rad) * 88.0f) - 6;
        lv_obj_set_pos(sCompNeedle, cx, cy);
        return;
    }

    uint32_t steps = MotionHaptic::getStepsToday();
    uint32_t goal = SettingsStorage::get().dailyStepGoal;
    if (!goal) goal = 1;
    setArc(aRing, (float)steps / goal);
    snprintf(b, sizeof(b), "%lu", (unsigned long)steps);                 setText(aSteps, b);
    snprintf(b, sizeof(b), "%d%% ZIEL", (int)(steps * 100 / goal));      setText(aPct, b);
    snprintf(b, sizeof(b), "%.2f km", MotionHaptic::getDistanceKm());     setText(aDist, b);
    snprintf(b, sizeof(b), "%lu kcal", (unsigned long)MotionHaptic::getCalories()); setText(aKcal, b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)goal);                  setText(aGoal, b);

    ActivityState st = MotionHaptic::getActivityState();
    setText(aStateTxt, MotionHaptic::getActivityLabel());
    lv_color_t stCol = (st == ACT_WALKING || st == ACT_RUNNING) ? cOk() :
                       (st == ACT_HAND_MOTION) ? cWarn() :
                       (st == ACT_SLEEPING) ? lv_color_hex(0x5E5CE6) : cText2();
    setBgColorIf(aStatePill, stCol);
    setTextColorIf(aStateTxt, stCol);

    snprintf(b, sizeof(b), "Kadenz %u/min  ·  Hand-Filter: %lu",
             (unsigned)MotionHaptic::getLearnedCadenceSpm(),
             (unsigned long)MotionHaptic::getFilteredFalseSteps());
    setText(aLearnLbl, b);

    uint32_t hist[7];
    MotionHaptic::getHistory(hist);
    uint32_t peak = goal;
    for (int i = 0; i < 7; i++) if (hist[i] > peak) peak = hist[i];
    int gy = BAR_BASE - (int)((uint64_t)goal * BAR_MAX / peak);
    if (lv_obj_get_y(aGoalLine) != gy) lv_obj_set_y(aGoalLine, gy);

    struct tm lt;
    RtcManager::getLocalTimeInfo(&lt);
    static const char* D1[] = {"S", "M", "D", "M", "D", "F", "S"};
    for (int i = 0; i < 7; i++) {
        int h = (int)((uint64_t)hist[i] * BAR_MAX / peak);
        if (h < 4) h = 4;
        if (lv_obj_get_height(aBars[i]) != h) {
            lv_obj_set_height(aBars[i], h);
            lv_obj_set_y(aBars[i], BAR_BASE - h);
        }
        setText(aDays[i], D1[(lt.tm_wday + 1 + i) % 7]);
    }
}

void buildActivity(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "ACTIVITY");

    aModePill = box(p, 134, 30);
    lv_obj_align(aModePill, LV_ALIGN_TOP_RIGHT, -M, 24);
    lv_obj_set_style_radius(aModePill, 15, 0);
    lv_obj_set_style_bg_opa(aModePill, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(aModePill, accent(), 0);
    lv_obj_add_flag(aModePill, LV_OBJ_FLAG_CLICKABLE);
    aModeTxt = caption(aModePill, "WASSERWAAGE", accent());
    accReg(aModeTxt, ACC_TEXT);
    lv_obj_center(aModeTxt);
    lv_obj_add_event_cb(aModePill, [](lv_event_t*) {
        sShowLevel = !sShowLevel;
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    // ---- View 0: Activity & Smart Step/Sleep Learning ----
    aActWrap = box(p, SCR_W, SCR_H - 68);
    lv_obj_set_pos(aActWrap, 0, 68);

    aStatePill = box(aActWrap, 146, 24);
    lv_obj_set_pos(aStatePill, M, 4);
    lv_obj_set_style_radius(aStatePill, 12, 0);
    lv_obj_set_style_bg_opa(aStatePill, LV_OPA_20, 0);
    aStateTxt = caption(aStatePill, "RUHE / STEHEN", cText2());
    lv_obj_center(aStateTxt);

    aLearnLbl = caption(aActWrap, "Kadenz 106/min  ·  Hand-Filter: 0", cText3());
    lv_obj_set_pos(aLearnLbl, M + 154, 8);

    aRing = ring(aActWrap, 168, 14, accent());
    accReg(aRing, ACC_ARC, LV_PART_INDICATOR);
    lv_obj_set_pos(aRing, M, 36);
    aSteps = label(aRing, F.numM, cText(), "0");
    lv_obj_align(aSteps, LV_ALIGN_CENTER, 0, -10);
    aPct = caption(aRing, "", cText2());
    lv_obj_align(aPct, LV_ALIGN_CENTER, 0, 22);

    const int cx = M + 168 + 22;
    const char* caps[3] = {"DISTANZ", "ENERGIE", "TAGESZIEL"};
    lv_obj_t** vals[3] = {&aDist, &aKcal, &aGoal};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = caption(aActWrap, caps[i], cText3());
        lv_obj_set_pos(c, cx, 40 + i * 54);
        *vals[i] = label(aActWrap, F.body, cText(), "");
        lv_obj_set_pos(*vals[i], cx, 58 + i * 54);
    }

    lv_obj_t* cd = card(aActWrap, CW, 168);
    lv_obj_set_pos(cd, M, 232);
    lv_obj_t* cap = caption(cd, "7-TAGE VERLAUF (SMART STEP GATE)", cText3());
    lv_obj_set_pos(cap, 20, 12);
    aGoalLine = box(cd, CW - 40, 1);
    lv_obj_set_style_bg_opa(aGoalLine, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(aGoalLine, cText3(), 0);
    lv_obj_set_pos(aGoalLine, 20, BAR_BASE - BAR_MAX);
    for (int i = 0; i < 7; i++) {
        int x = 20 + i * (BAR_W + barGap());
        aBars[i] = box(cd, BAR_W, 4);
        lv_obj_set_style_radius(aBars[i], 8, 0);
        lv_obj_set_style_bg_opa(aBars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(aBars[i], i == 6 ? accent() : cStroke(), 0);
        if (i == 6) accReg(aBars[i], ACC_BG);
        lv_obj_set_pos(aBars[i], x, BAR_BASE - 4);
        aDays[i] = caption(cd, "", i == 6 ? cText() : cText3());
        lv_obj_set_width(aDays[i], BAR_W);
        lv_obj_set_style_text_align(aDays[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(aDays[i], x, 138);
    }

    // ---- View 1: BHI260AP Digital Bubble Level & Gyro Compass ----
    aLevelWrap = box(p, SCR_W, SCR_H - 68);
    lv_obj_set_pos(aLevelWrap, 0, 68);
    setHidden(aLevelWrap, true);

    lv_obj_t* lvlCard = card(aLevelWrap, CW, 310);
    lv_obj_set_pos(lvlCard, M, 8);

    sCompReadout = label(lvlCard, F.body, accent(), "KOMPASS  000° N");
    accReg(sCompReadout, ACC_TEXT);
    lv_obj_align(sCompReadout, LV_ALIGN_TOP_MID, 0, 12);

    sLvlRingOuter = box(lvlCard, 192, 192);
    lv_obj_align(sLvlRingOuter, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(sLvlRingOuter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sLvlRingOuter, lv_color_hex(0x0B0F17), 0);
    lv_obj_set_style_bg_opa(sLvlRingOuter, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sLvlRingOuter, 2, 0);
    lv_obj_set_style_border_color(sLvlRingOuter, cStroke(), 0);

    // Crosshairs
    lv_obj_t* chH = box(sLvlRingOuter, 176, 1);
    lv_obj_center(chH);
    lv_obj_set_style_bg_opa(chH, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(chH, cText3(), 0);
    lv_obj_t* chV = box(sLvlRingOuter, 1, 176);
    lv_obj_center(chV);
    lv_obj_set_style_bg_opa(chV, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(chV, cText3(), 0);

    sLvlRingInner = box(sLvlRingOuter, 44, 44);
    lv_obj_center(sLvlRingInner);
    lv_obj_set_style_radius(sLvlRingInner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(sLvlRingInner, 2, 0);
    lv_obj_set_style_border_color(sLvlRingInner, cText3(), 0);

    sCompNeedle = box(sLvlRingOuter, 12, 12);
    lv_obj_set_style_radius(sCompNeedle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sCompNeedle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sCompNeedle, cCrit(), 0);

    sLvlBubble = box(sLvlRingOuter, 28, 28);
    lv_obj_set_style_radius(sLvlBubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sLvlBubble, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sLvlBubble, cOk(), 0);
    lv_obj_set_pos(sLvlBubble, 96 - 14, 96 - 14);

    sLvlReadout = label(lvlCard, F.small, cText(), "Roll +0.0°   ·   Pitch +0.0°");
    lv_obj_align(sLvlReadout, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_t* calBtn = lv_button_create(aLevelWrap);
    lv_obj_set_size(calBtn, CW, 62);
    lv_obj_set_pos(calBtn, M, 330);
    lv_obj_set_style_radius(calBtn, R_CARD, 0);
    lv_obj_set_style_shadow_width(calBtn, 0, 0);
    lv_obj_set_style_bg_color(calBtn, cSurf2(), 0);
    lv_obj_set_style_border_width(calBtn, 2, 0);
    lv_obj_set_style_border_color(calBtn, accent(), 0);
    accReg(calBtn, ACC_BORDER);
    lv_obj_t* calTxt = label(calBtn, F.small, accent(), "0° NULLPUNKT KALIBRIEREN");
    accReg(calTxt, ACC_TEXT);
    lv_obj_center(calTxt);
    lv_obj_add_event_cb(calBtn, [](lv_event_t*) {
        MotionHaptic::calibrateLevelAndCompass();
        haptic(HAPTIC_SUCCESS);
        DisplayEngine::showNotification("BHI260AP Kalibriert", "Waage (0.0°) und Nord-Referenz gesetzt", 2200);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 150;
    d.ttlMs = 30000;
    nav::registerPage(nav::PG_ACTIVITY, d);
}

}  // namespace pages
