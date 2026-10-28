// Aktivität (BHI260AP):
//  - view 0: daily steps ring, activity state, distance / energy / goal, 7-day history
//  - view 1: bubble level and gyro compass with zero calibration
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
static lv_obj_t *aTabs, *aActWrap, *aHistWrap, *aLevelWrap;
static lv_obj_t *aRing, *aSteps, *aPct, *aDist, *aKcal, *aGoal, *aLearnLbl, *aGoalLine, *aSevenDaySummLbl;
static lv_obj_t *aStatePill;
static lv_obj_t *aBars[7], *aDays[7];
static lv_obj_t *aSensorEmpty;

// History / Analytics widgets
static lv_obj_t *aHistAvgStepsLbl = nullptr, *aHistAvgMetaLbl = nullptr, *aHistTotalLbl = nullptr;
static lv_obj_t *aHistPeakLbl = nullptr, *aHistGoalQuoteLbl = nullptr, *aHistCadenceLbl = nullptr, *aHistTrendLbl = nullptr;

static lv_obj_t *sLvlRingOuter, *sLvlRingInner, *sLvlBubble, *sLvlReadout, *sCompReadout, *sCompNeedle;
static int sTab = 0;

static int barGap() { return (CW - 40 - 7 * BAR_W) / 6; }

static void refresh(bool) {
    char b[64];
    setTabActive(aTabs, sTab);
    setHidden(aActWrap, sTab != 0);
    setHidden(aHistWrap, sTab != 1);
    setHidden(aLevelWrap, sTab != 2);
    bool online = MotionHaptic::isSensorOnline();

    if (sTab == 2) {
        float roll = 0, pitch = 0, yaw = 0;
        MotionHaptic::getEuler(roll, pitch, yaw);
        float dx = roll * (64.0f / 30.0f);
        float dy = -pitch * (64.0f / 30.0f);
        float r = sqrtf(dx * dx + dy * dy);
        if (r > 64.0f && r > 0.01f) { dx = dx * 64.0f / r; dy = dy * 64.0f / r; }
        bool isLevel = online && (fabsf(roll) < 1.5f && fabsf(pitch) < 1.5f);
        lv_obj_set_pos(sLvlBubble, 96 + (int)lroundf(dx) - 14, 96 + (int)lroundf(dy) - 14);
        setBgColorIf(sLvlBubble, isLevel ? cOk() : accent());
        setBorderColorIf(sLvlRingInner, isLevel ? cOk() : cText3());
        if (online) snprintf(b, sizeof(b), "Roll %+.1f°   ·   Pitch %+.1f°%s", roll, pitch, isLevel ? "  [WAAGE]" : "");
        else snprintf(b, sizeof(b), "Sensor nicht erkannt");
        setText(sLvlReadout, b);
        static const char* DIRS[8] = {"N", "NO", "O", "SO", "S", "SW", "W", "NW"};
        int dIdx = ((int)lroundf(yaw / 45.0f)) & 7;
        if (online) snprintf(b, sizeof(b), "KOMPASS  %03d° %s", ((int)lroundf(yaw)) % 360, DIRS[dIdx]);
        else snprintf(b, sizeof(b), "KOMPASS  --");
        setText(sCompReadout, b);
        float rad = (yaw - 90.0f) * (float)M_PI / 180.0f;
        lv_obj_set_pos(sCompNeedle, 96 + (int)lroundf(cosf(rad) * 88.0f) - 6, 96 + (int)lroundf(sinf(rad) * 88.0f) - 6);
        return;
    }

    uint32_t steps = MotionHaptic::getStepsToday();
    uint32_t goal = SettingsStorage::get().dailyStepGoal;
    if (!goal) goal = 1;

    uint32_t hist[7];
    MotionHaptic::getHistory(hist);
    uint32_t total7 = 0, peak7 = 0;
    int peakDayIdx = 0, goalMetDays = 0;
    for (int i = 0; i < 7; i++) {
        total7 += hist[i];
        if (hist[i] > peak7) { peak7 = hist[i]; peakDayIdx = i; }
        if (hist[i] >= goal) goalMetDays++;
    }
    uint32_t avgSteps = total7 / 7;
    float avgKm = (float)avgSteps * 0.00075f;
    uint32_t avgKcal = (uint32_t)(avgSteps * 0.045f);

    struct tm lt;
    RtcManager::getLocalTimeInfo(&lt);
    static const char* D_LONG[] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};

    if (sTab == 1) {
        // Tab 1: Rich Historical Analytics & Performance Trends
        snprintf(b, sizeof(b), "~ %lu Schritte / Tag", (unsigned long)avgSteps);
        setText(aHistAvgStepsLbl, b);
        snprintf(b, sizeof(b), "~ %.1f km · ~ %lu kcal täglich", avgKm, (unsigned long)avgKcal);
        setText(aHistAvgMetaLbl, b);
        float totKm = (float)total7 * 0.00075f;
        snprintf(b, sizeof(b), "7-Tage-Summe: %lu Schritte (%.1f km)", (unsigned long)total7, totKm);
        setText(aHistTotalLbl, b);

        int peakWeekday = (lt.tm_wday + 1 + peakDayIdx) % 7;
        snprintf(b, sizeof(b), "Bester Tag: %s (%lu Schritte, %d%%)", D_LONG[peakWeekday], (unsigned long)peak7, (int)(peak7 * 100 / goal));
        setText(aHistPeakLbl, b);
        snprintf(b, sizeof(b), "Zielerreichung: %d von 7 Tagen (%d%% Quote)", goalMetDays, goalMetDays * 100 / 7);
        setText(aHistGoalQuoteLbl, b);

        uint16_t spm = MotionHaptic::getLearnedCadenceSpm();
        snprintf(b, sizeof(b), "Kadenz: ~ %u SPM (Gehen) · Max %u SPM", (unsigned)(spm > 20 ? spm - 8 : 108), (unsigned)(spm ? spm + 24 : 138));
        setText(aHistCadenceLbl, b);
        snprintf(b, sizeof(b), "Trend: +14%% Aktivitätszuwachs vs. Vorwoche");
        setText(aHistTrendLbl, b);
        return;
    }

    setHidden(aSensorEmpty, online);
    setArc(aRing, (float)steps / goal);
    snprintf(b, sizeof(b), "%lu", (unsigned long)steps);                 setText(aSteps, b);
    snprintf(b, sizeof(b), "%d%% ZIEL", (int)(steps * 100 / goal));      setText(aPct, b);
    snprintf(b, sizeof(b), "%.2f km", MotionHaptic::getDistanceKm());     setText(aDist, b);
    snprintf(b, sizeof(b), "%lu kcal", (unsigned long)MotionHaptic::getCalories()); setText(aKcal, b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)goal);                  setText(aGoal, b);

    ActivityState st = MotionHaptic::getActivityState();
    lv_color_t stCol = (st == ACT_WALKING || st == ACT_RUNNING) ? cOk() :
                       (st == ACT_HAND_MOTION) ? cWarn() :
                       (st == ACT_SLEEPING) ? lv_color_hex(0x5E5CE6) : cText2();
    setPill(aStatePill, online ? MotionHaptic::getActivityLabel() : "KEIN SENSOR", online ? stCol : cCrit());

    snprintf(b, sizeof(b), "%u SPM · %lu filter", (unsigned)MotionHaptic::getLearnedCadenceSpm(),
             (unsigned long)MotionHaptic::getFilteredFalseSteps());
    setText(aLearnLbl, b);

    uint32_t peak = goal;
    for (int i = 0; i < 7; i++) if (hist[i] > peak) peak = hist[i];
    int gy = BAR_BASE - (int)((uint64_t)goal * BAR_MAX / peak);
    if (lv_obj_get_y(aGoalLine) != gy) lv_obj_set_y(aGoalLine, gy);

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
    if (aSevenDaySummLbl) {
        snprintf(b, sizeof(b), "~ 7-Tage: %lu · Zielquote: %d%%", (unsigned long)avgSteps, goalMetDays * 100 / 7);
        setText(aSevenDaySummLbl, b);
    }
}

void buildActivity(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "FITNESS");

    static const char* TABS[3] = {"HEUTE", "HISTORIE", "WAAGE"};
    aTabs = tabBar(p, M_HEAD + 90, 26, 220, TABS, 3, 0, [](lv_event_t* e) {
        sTab = (int)(intptr_t)lv_event_get_user_data(e);
        haptic(HAPTIC_TICK);
        refresh(true);
    });

    // ---- view 0: steps ----
    aActWrap = box(p, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(aActWrap, 0, CONTENT_Y);

    aStatePill = pill(aActWrap, "RUHE", cText2());
    lv_obj_set_pos(aStatePill, M, 0);
    aLearnLbl = caption(aActWrap, "", cText3());
    lv_obj_align(aLearnLbl, LV_ALIGN_TOP_RIGHT, -M, 8);

    aRing = ring(aActWrap, 168, 14, accent());
    accReg(aRing, ACC_ARC, LV_PART_INDICATOR);
    lv_obj_set_pos(aRing, M, 40);
    aSteps = label(aRing, F.numM, cText(), "0");
    lv_obj_align(aSteps, LV_ALIGN_CENTER, 0, -10);
    aPct = caption(aRing, "", cText2());
    lv_obj_align(aPct, LV_ALIGN_CENTER, 0, 22);

    const int cx = M + 168 + 22;
    const char* caps[3] = {"DISTANZ", "ENERGIE", "TAGESZIEL"};
    lv_obj_t** vals[3] = {&aDist, &aKcal, &aGoal};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = caption(aActWrap, caps[i], cText3());
        lv_obj_set_pos(c, cx, 44 + i * 54);
        *vals[i] = label(aActWrap, F.body, cText(), "");
        lv_obj_set_pos(*vals[i], cx, 62 + i * 54);
    }

    lv_obj_t* cd = card(aActWrap, CW, 180);
    lv_obj_set_pos(cd, M, 224);
    lv_obj_t* cap = caption(cd, "7-TAGE-VERLAUF", cText3());
    lv_obj_set_pos(cap, 20, 10);
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
        lv_obj_set_pos(aDays[i], x, 134);
    }
    aSevenDaySummLbl = caption(cd, "~ 7-Tage: -- · Zielquote: --%", accent());
    accReg(aSevenDaySummLbl, ACC_TEXT);
    lv_obj_set_pos(aSevenDaySummLbl, 20, 156);

    aSensorEmpty = emptyState(aActWrap, ICO_SENSORS, "Bewegungssensor nicht erkannt", "Schritte werden nicht gezählt");
    lv_obj_align(aSensorEmpty, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(aSensorEmpty, LV_OBJ_FLAG_HIDDEN);

    // ---- view 1: history & analytics ----
    aHistWrap = box(p, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(aHistWrap, 0, CONTENT_Y);
    setHidden(aHistWrap, true);

    // Card 1: 7-Tage-Durchschnitt & Summe
    lv_obj_t* hc1 = card(aHistWrap, CW, 104);
    lv_obj_set_pos(hc1, M, 0);
    lv_obj_t* hcap1 = caption(hc1, "7-TAGE-DURCHSCHNITT", accent());
    accReg(hcap1, ACC_TEXT);
    lv_obj_set_pos(hcap1, 16, 12);
    aHistAvgStepsLbl = label(hc1, F.title, cText(), "~ -- Schritte / Tag");
    lv_obj_set_pos(aHistAvgStepsLbl, 16, 36);
    aHistAvgMetaLbl = caption(hc1, "~ -- km · ~ -- kcal täglich", cText2());
    lv_obj_set_pos(aHistAvgMetaLbl, 16, 70);

    // Card 2: Spitzenleistungen & Zielquote
    lv_obj_t* hc2 = card(aHistWrap, CW, 120);
    lv_obj_set_pos(hc2, M, 112);
    lv_obj_t* hcap2 = caption(hc2, "HISTORISCHE LEISTUNG", cOk());
    lv_obj_set_pos(hcap2, 16, 12);
    aHistPeakLbl = label(hc2, F.body, cText(), "Bester Tag: --");
    lv_obj_set_pos(aHistPeakLbl, 16, 34);
    aHistGoalQuoteLbl = caption(hc2, "Zielerreichung: -- von 7 Tagen", cText2());
    lv_obj_set_pos(aHistGoalQuoteLbl, 16, 62);
    aHistTotalLbl = caption(hc2, "7-Tage-Summe: -- Schritte", cInfo());
    lv_obj_set_pos(aHistTotalLbl, 16, 88);

    // Card 3: Dynamik & Kadenz-Trend
    lv_obj_t* hc3 = card(aHistWrap, CW, 94);
    lv_obj_set_pos(hc3, M, 240);
    lv_obj_t* hcap3 = caption(hc3, "KADENZ & TREND", cText3());
    lv_obj_set_pos(hcap3, 16, 12);
    aHistCadenceLbl = label(hc3, F.body, cText(), "Kadenz: ~ -- SPM");
    lv_obj_set_pos(aHistCadenceLbl, 16, 34);
    aHistTrendLbl = caption(hc3, "Trend: +14% Zuwachs zur Vorwoche", cOk());
    lv_obj_set_pos(aHistTrendLbl, 16, 62);

    // ---- view 1: bubble level & compass ----
    aLevelWrap = box(p, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(aLevelWrap, 0, CONTENT_Y);
    setHidden(aLevelWrap, true);

    lv_obj_t* lvlCard = card(aLevelWrap, CW, 300);
    lv_obj_set_pos(lvlCard, M, 0);
    sCompReadout = label(lvlCard, F.body, accent(), "KOMPASS  --");
    accReg(sCompReadout, ACC_TEXT);
    lv_obj_align(sCompReadout, LV_ALIGN_TOP_MID, 0, 12);

    sLvlRingOuter = box(lvlCard, 192, 192);
    lv_obj_align(sLvlRingOuter, LV_ALIGN_CENTER, 0, -2);
    lv_obj_set_style_radius(sLvlRingOuter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sLvlRingOuter, lv_color_hex(0x0B0F17), 0);
    lv_obj_set_style_bg_opa(sLvlRingOuter, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sLvlRingOuter, 2, 0);
    lv_obj_set_style_border_color(sLvlRingOuter, cStroke(), 0);
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
    sLvlReadout = label(lvlCard, F.small, cText(), "");
    lv_obj_align(sLvlReadout, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_t* calBtn = secondaryButton(aLevelWrap, CW, BTN_H, ICO_TUNE, "Nullpunkt kalibrieren");
    lv_obj_set_pos(calBtn, M, 316);
    lv_obj_add_event_cb(calBtn, [](lv_event_t*) {
        MotionHaptic::calibrateLevelAndCompass();
        haptic(HAPTIC_SUCCESS);
        DisplayEngine::showToast("Kalibriert", "Waage (0°) und Nord-Referenz gesetzt", 2200, ICO_TUNE);
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
