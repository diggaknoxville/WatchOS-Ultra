// Wecker app: one daily alarm (persisted) and a countdown timer.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "RtcManager.h"
#include "PowerManager.h"
#include "DisplayEngine.h"
#include "AudioEngine.h"
#include <Preferences.h>

using namespace ui;

namespace pages {

// ---- state -----------------------------------------------------------------
static uint8_t alHour = 7, alMin = 0;
static bool alOn = false;
static bool alDirty = false;            // written to NVS by alarmTick()
static int alFiredYday = -1;            // day of year the alarm last fired (or was set in its own minute)

static uint8_t tmrMin = 5;
static bool tmrRun = false;
static uint32_t tmrStartMs = 0;
static uint32_t tmrLeftMs = 5 * 60000UL;    // remaining when (re)started or stopped

static int alTab = 0;                   // 0 = Wecker, 1 = Timer
static lv_obj_t *alTabs, *alPane, *alHourLbl, *alMinLbl, *alRow, *alInfo;
static lv_obj_t *tmrPane, *tmrTime, *tmrState, *tmrMinLbl, *tmrMinus, *tmrPlus, *tmrStart;

static uint32_t tmrFullMs() { return (uint32_t)tmrMin * 60000UL; }

static uint32_t tmrLeft() {
    if (!tmrRun) return tmrLeftMs;
    uint32_t e = millis() - tmrStartMs;             // overflow-safe
    return e < tmrLeftMs ? tmrLeftMs - e : 0;
}

// ---- persistence -----------------------------------------------------------
static void alLoad() {
    Preferences p;
    if (!p.begin("kronos_alarm", true)) return;
    alHour = p.getUChar("h", 7) % 24;
    alMin = p.getUChar("m", 0) % 60;
    alOn = p.getBool("on", false);
    p.end();
}

static void alSave() {
    Preferences p;
    if (!p.begin("kronos_alarm", false)) return;
    p.putUChar("h", alHour);
    p.putUChar("m", alMin);
    p.putBool("on", alOn);
    p.end();
}

// ---- display ---------------------------------------------------------------
static void alarmRefresh(bool) {
    char b[48];
    setTabActive(alTabs, alTab);
    setHidden(alPane, alTab != 0);
    setHidden(tmrPane, alTab != 1);

    if (alTab == 0) {
        snprintf(b, sizeof(b), "%02u", (unsigned)alHour);
        setText(alHourLbl, b);
        snprintf(b, sizeof(b), "%02u", (unsigned)alMin);
        setText(alMinLbl, b);
        setToggleRow(alRow, alOn);
        if (alOn) snprintf(b, sizeof(b), "TÄGLICH UM %02u:%02u", (unsigned)alHour, (unsigned)alMin);
        else snprintf(b, sizeof(b), "WECKER AUS");
        setText(alInfo, b);
    } else {
        uint32_t s = (tmrLeft() + 999) / 1000;      // round up: 00:00 only when expired
        snprintf(b, sizeof(b), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        setText(tmrTime, b);
        setText(tmrState, tmrRun ? "LÄUFT" : (tmrLeftMs != tmrFullMs() ? "PAUSIERT" : "BEREIT"));
        snprintf(b, sizeof(b), "%u", (unsigned)tmrMin);
        setText(tmrMinLbl, b);
        setButtonEnabled(tmrMinus, !tmrRun);
        setButtonEnabled(tmrPlus, !tmrRun);
        setButtonText(tmrStart, tmrRun ? "Stopp" : "Start");
    }
}

// ---- input -----------------------------------------------------------------
// The alarm changed: persist it and re-arm. An alarm set to the running minute
// waits for tomorrow instead of going off under the user's finger.
static void alChanged() {
    struct tm t;
    RtcManager::getLocalTimeInfo(&t);
    alFiredYday = (t.tm_hour == alHour && t.tm_min == alMin) ? t.tm_yday : -1;
    alDirty = true;
    haptic(HAPTIC_TICK);
    alarmRefresh(true);
}

// user data: 0 = hour +1, 1 = hour -1, 2 = minute +5, 3 = minute -5
static void onAlStep(lv_event_t* e) {
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
        case 0: alHour = (alHour + 1) % 24; break;
        case 1: alHour = (alHour + 23) % 24; break;
        case 2: alMin = (alMin / 5 * 5 + 5) % 60; break;
        default: alMin = (alMin / 5 * 5 + 55) % 60; break;
    }
    alChanged();
}

// user data: +1 / -1 minute, only while the timer is stopped
static void onTmrStep(lv_event_t* e) {
    if (tmrRun) return;
    int m = tmrMin + (int)(intptr_t)lv_event_get_user_data(e);
    if (m < 1 || m > 180) return;
    tmrMin = (uint8_t)m;
    tmrLeftMs = tmrFullMs();
    haptic(HAPTIC_TICK);
    alarmRefresh(true);
}

static lv_obj_t* stepButton(lv_obj_t* parent, int x, int y, const char* icon, lv_event_cb_t cb, int user) {
    lv_obj_t* b = iconButton(parent, 64, icon, cSurf2(), cText());
    lv_obj_set_pos(b, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)(intptr_t)user);
    return b;
}

// ---- firing ----------------------------------------------------------------
static void alarmFire(const char* titleTxt, const char* msg, const char* icon) {
    PowerManager::handleActivity();                 // wakes the display
    DisplayEngine::showNotification(titleTxt, msg, 8000, icon);
    MotionHaptic::triggerHaptic(HAPTIC_ALERT_LONG);
    AudioEngine::playSoundFx(SFX_ALERT);
    nav::markDirty(nav::PG_APP_ALARM);
}

void alarmTick() {
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    if (now - lastMs < 1000) return;
    lastMs = now;

    if (alDirty) {
        alDirty = false;
        alSave();
    }

    if (tmrRun && tmrLeft() == 0) {
        tmrRun = false;
        tmrLeftMs = tmrFullMs();
        alarmFire("Timer", "Abgelaufen", ICO_TIMER);
    }

    if (!alOn) return;
    struct tm t;
    RtcManager::getLocalTimeInfo(&t);
    if (t.tm_hour != alHour || t.tm_min != alMin || t.tm_yday == alFiredYday) return;
    alFiredYday = t.tm_yday;
    char b[16];
    snprintf(b, sizeof(b), "%02u:%02u", (unsigned)alHour, (unsigned)alMin);
    alarmFire("Wecker", b, ICO_ALARM);
}

// ---- build -----------------------------------------------------------------
void buildAlarm(lv_obj_t* scr) {
    alLoad();

    lv_obj_t* p = pageRoot(scr);
    title(p, "WECKER");

    static const char* TABS[2] = {"WECKER", "TIMER"};
    alTabs = tabBar(p, M, CONTENT_Y, CW, TABS, 2, 0, [](lv_event_t* e) {
        alTab = (int)(intptr_t)lv_event_get_user_data(e);
        haptic(HAPTIC_TICK);
        alarmRefresh(true);
    });

    const int paneY = CONTENT_Y + 48;               // 130, panes end at 440
    const int paneH = 310;

    // --- Wecker: [+] / HH : MM / [-], toggle, summary ---
    alPane = box(p, SCR_W, paneH);
    lv_obj_set_pos(alPane, 0, paneY);
    const int hourX = SCR_W / 2 - 80, minX = SCR_W / 2 + 80;    // column centres
    stepButton(alPane, hourX - 32, 0, ICO_ADD, onAlStep, 0);
    stepButton(alPane, minX - 32, 0, ICO_ADD, onAlStep, 2);
    alHourLbl = label(alPane, F.numL, cText(), "07");
    alMinLbl = label(alPane, F.numL, cText(), "00");
    lv_obj_t* digits[2] = {alHourLbl, alMinLbl};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(digits[i], 140);
        lv_obj_set_style_text_align(digits[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(digits[i], (i ? minX : hourX) - 70, 70);
    }
    lv_obj_t* colon = label(alPane, F.numL, accent(), ":");
    accReg(colon, ACC_TEXT);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 70);
    stepButton(alPane, hourX - 32, 140, ICO_REMOVE, onAlStep, 1);
    stepButton(alPane, minX - 32, 140, ICO_REMOVE, onAlStep, 3);

    alRow = toggleRow(alPane, 216, ICO_ALARM, "Wecker aktiv", alOn, [](lv_event_t*) {
        alOn = !alOn;
        alChanged();
    });
    alInfo = caption(alPane, "", cText3());
    lv_obj_align(alInfo, LV_ALIGN_TOP_MID, 0, 282);

    // --- Timer: remaining, [-] minutes [+], reset / start ---
    tmrPane = box(p, SCR_W, paneH);
    lv_obj_set_pos(tmrPane, 0, paneY);
    tmrTime = label(tmrPane, F.numL, cText(), "05:00");
    lv_obj_align(tmrTime, LV_ALIGN_TOP_MID, 0, 0);
    tmrState = caption(tmrPane, "BEREIT", cText2());
    lv_obj_align(tmrState, LV_ALIGN_TOP_MID, 0, 68);

    tmrMinus = stepButton(tmrPane, M, 100, ICO_REMOVE, onTmrStep, -1);
    tmrPlus = stepButton(tmrPane, M + CW - 64, 100, ICO_ADD, onTmrStep, 1);
    tmrMinLbl = label(tmrPane, F.numM, accent(), "5");
    accReg(tmrMinLbl, ACC_TEXT);
    lv_obj_align(tmrMinLbl, LV_ALIGN_TOP_MID, 0, 102);
    lv_obj_t* unit = caption(tmrPane, "MINUTEN", cText3());
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 142);

    const int bw = (CW - 14) / 2;
    lv_obj_t* resetBtn = secondaryButton(tmrPane, bw, 64, nullptr, "Zurücksetzen");
    lv_obj_set_pos(resetBtn, M, 240);
    lv_obj_add_event_cb(resetBtn, [](lv_event_t*) {
        tmrRun = false;
        tmrLeftMs = tmrFullMs();
        haptic(HAPTIC_TICK);
        alarmRefresh(true);
    }, LV_EVENT_CLICKED, NULL);

    tmrStart = primaryButton(tmrPane, bw, 64, nullptr, "Start");
    lv_obj_set_pos(tmrStart, M + bw + 14, 240);
    lv_obj_add_event_cb(tmrStart, [](lv_event_t*) {
        if (tmrRun) {
            tmrLeftMs = tmrLeft();
            tmrRun = false;
            if (tmrLeftMs == 0) tmrLeftMs = tmrFullMs();    // expired, alarmTick() not there yet
        } else {
            tmrStartMs = millis();
            tmrRun = true;
        }
        haptic(HAPTIC_CLICK);
        alarmRefresh(true);
    }, LV_EVENT_CLICKED, NULL);

    alarmRefresh(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = alarmRefresh;
    d.refreshMs = 250;
    d.ttlMs = 5000;
    nav::registerPage(nav::PG_APP_ALARM, d);
}

}  // namespace pages
