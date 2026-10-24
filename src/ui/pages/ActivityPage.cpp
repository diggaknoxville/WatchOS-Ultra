// Activity: daily steps ring, distance / energy / goal and a 7-day history.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "MotionHaptic.h"
#include "RtcManager.h"

using namespace ui;

namespace pages {

static const int BAR_W = 30, BAR_MAX = 96, BAR_BASE = 144;
static lv_obj_t *aRing, *aSteps, *aPct, *aDist, *aKcal, *aGoal, *aGoalLine;
static lv_obj_t *aBars[7], *aDays[7];

static int barGap() { return (CW - 40 - 7 * BAR_W) / 6; }

static void refresh(bool) {
    char b[48];
    uint32_t steps = MotionHaptic::getStepsToday();
    uint32_t goal = SettingsStorage::get().dailyStepGoal;
    if (!goal) goal = 1;
    setArc(aRing, (float)steps / goal);
    snprintf(b, sizeof(b), "%lu", (unsigned long)steps);                 setText(aSteps, b);
    snprintf(b, sizeof(b), "%d%% OF GOAL", (int)(steps * 100 / goal));    setText(aPct, b);
    snprintf(b, sizeof(b), "%.2f km", MotionHaptic::getDistanceKm());     setText(aDist, b);
    snprintf(b, sizeof(b), "%lu kcal", (unsigned long)MotionHaptic::getCalories()); setText(aKcal, b);
    snprintf(b, sizeof(b), "%lu steps", (unsigned long)goal);            setText(aGoal, b);

    uint32_t hist[7];
    MotionHaptic::getHistory(hist);
    uint32_t peak = goal;
    for (int i = 0; i < 7; i++) if (hist[i] > peak) peak = hist[i];
    int gy = BAR_BASE - (int)((uint64_t)goal * BAR_MAX / peak);
    if (lv_obj_get_y(aGoalLine) != gy) lv_obj_set_y(aGoalLine, gy);

    struct tm lt;
    RtcManager::getLocalTimeInfo(&lt);
    static const char* D1[] = {"S", "M", "T", "W", "T", "F", "S"};
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
    aRing = ring(p, 176, 14, accent());
    accReg(aRing, ACC_ARC, LV_PART_INDICATOR);
    lv_obj_set_pos(aRing, M, 92);
    aSteps = label(aRing, F.numM, cText(), "0");
    lv_obj_align(aSteps, LV_ALIGN_CENTER, 0, -10);
    aPct = caption(aRing, "", cText2());
    lv_obj_align(aPct, LV_ALIGN_CENTER, 0, 22);

    const int cx = M + 176 + 22;
    const char* caps[3] = {"DISTANCE", "ENERGY", "DAILY GOAL"};
    lv_obj_t** vals[3] = {&aDist, &aKcal, &aGoal};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* c = caption(p, caps[i], cText3());
        lv_obj_set_pos(c, cx, 100 + i * 58);
        *vals[i] = label(p, F.body, cText(), "");
        lv_obj_set_pos(*vals[i], cx, 118 + i * 58);
    }

    lv_obj_t* cd = card(p, CW, 182);
    lv_obj_set_pos(cd, M, 290);
    lv_obj_t* cap = caption(cd, "LAST 7 DAYS", cText3());
    lv_obj_set_pos(cap, 20, 16);
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
        lv_obj_set_pos(aDays[i], x, 150);
    }

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 30000;
    nav::registerPage(nav::PG_ACTIVITY, d);
}

}  // namespace pages
