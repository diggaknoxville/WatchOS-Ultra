// Smaller apps: stopwatch, power menu, phone connect, agenda placeholder.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "CompanionServer.h"
#include "PowerManager.h"
#include "SettingsStorage.h"
#include "RtcManager.h"
#include "AudioEngine.h"
#include "DisplayEngine.h"
#include "Config.h"
#include <LilyGoLib.h>

using namespace ui;

namespace pages {

// ============================================================== STOPWATCH
static lv_obj_t *swTime, *swCs, *swStart, *swStartTxt, *swLap, *swLapTxt, *swLaps[4];
static bool swRun = false;
static uint32_t swStartMs = 0, swAccum = 0, swLapTimes[4];
static int swLapN = 0;

static uint32_t swElapsed() { return swAccum + (swRun ? millis() - swStartMs : 0); }

static void swFmt(char* b, size_t n, uint32_t ms, bool cs) {
    uint32_t m = ms / 60000, s = (ms / 1000) % 60, c = (ms % 1000) / 10;
    if (cs) snprintf(b, n, "%02lu:%02lu.%02lu", (unsigned long)m, (unsigned long)s, (unsigned long)c);
    else snprintf(b, n, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
}

static void swRefresh(bool) {
    char b[24];
    uint32_t e = swElapsed();
    swFmt(b, sizeof(b), e, false);
    setText(swTime, b);
    snprintf(b, sizeof(b), ".%02lu", (unsigned long)((e % 1000) / 10));
    setText(swCs, b);
    setText(swStartTxt, swRun ? "STOP" : (e ? "RESUME" : "START"));
    setBgColorIf(swStart, swRun ? cCrit() : cOk());
    setText(swLapTxt, swRun ? "LAP" : "RESET");
    for (int i = 0; i < 4; i++) {
        bool has = i < swLapN;
        setHidden(swLaps[i], !has);
        if (!has) continue;
        char t[24];
        swFmt(t, sizeof(t), swLapTimes[i], true);
        snprintf(b, sizeof(b), "LAP %d   %s", swLapN - i, t);
        setText(swLaps[i], b);
    }
}

void buildStopwatch(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "STOPWATCH");
    lv_obj_t* row = box(p, SCR_W, 80);
    lv_obj_set_pos(row, 0, 104);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    swTime = label(row, F.numL, cText(), "00:00");
    swCs = label(row, F.numM, accent(), ".00");
    accReg(swCs, ACC_TEXT);

    for (int i = 0; i < 4; i++) {
        swLaps[i] = label(p, F.small, i == 0 ? cText() : cText2(), "");
        lv_obj_align(swLaps[i], LV_ALIGN_TOP_MID, 0, 208 + i * 30);
    }

    const int bw = (CW - 14) / 2;
    swLap = lv_button_create(p);
    lv_obj_set_size(swLap, bw, 76);
    lv_obj_set_pos(swLap, M, 386);
    lv_obj_set_style_radius(swLap, R_CARD, 0);
    lv_obj_set_style_shadow_width(swLap, 0, 0);
    lv_obj_set_style_bg_color(swLap, cSurf2(), 0);
    swLapTxt = label(swLap, F.body, cText(), "RESET");
    lv_obj_center(swLapTxt);
    lv_obj_add_event_cb(swLap, [](lv_event_t*) {
        if (swRun) {
            memmove(swLapTimes + 1, swLapTimes, sizeof(uint32_t) * 3);
            swLapTimes[0] = swElapsed();
            if (swLapN < 4) swLapN++;
        } else {
            swAccum = 0;
            swLapN = 0;
        }
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    swStart = lv_button_create(p);
    lv_obj_set_size(swStart, bw, 76);
    lv_obj_set_pos(swStart, M + bw + 14, 386);
    lv_obj_set_style_radius(swStart, R_CARD, 0);
    lv_obj_set_style_shadow_width(swStart, 0, 0);
    lv_obj_set_style_bg_color(swStart, cOk(), 0);
    swStartTxt = label(swStart, F.body, cBg(), "START");
    lv_obj_center(swStartTxt);
    lv_obj_add_event_cb(swStart, [](lv_event_t*) {
        if (swRun) { swAccum += millis() - swStartMs; swRun = false; }
        else { swStartMs = millis(); swRun = true; }
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);

    nav::PageDef d;
    d.root = p;
    d.refresh = swRefresh;
    d.refreshMs = 0;
    d.ttlMs = 2000;
    nav::registerPage(nav::PG_APP_STOPWATCH, d);
}

// ============================================================== POWER MENU
static lv_obj_t* powerButton(lv_obj_t* p, int y, const char* icon, const char* text, lv_color_t bg, lv_color_t fg) {
    lv_obj_t* b = lv_button_create(p);
    lv_obj_set_size(b, CW, 84);
    lv_obj_set_pos(b, M, y);
    lv_obj_set_style_radius(b, R_CARD, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_t* ic = label(b, F.icoM, fg, icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t* t = label(b, F.body, fg, text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 60, 0);
    return b;
}

void buildPower(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "POWER");
    lv_obj_t* sleepB = powerButton(p, 100, ICO_BEDTIME, "Display off", cSurf2(), cText());
    lv_obj_add_event_cb(sleepB, [](lv_event_t*) {
        nav::home();
        PowerManager::setPowerState(POWER_SLEEP);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* rst = powerButton(p, 196, ICO_RESTART_ALT, "Restart", cSurf2(), cText());
    lv_obj_add_event_cb(rst, [](lv_event_t*) {
        SettingsStorage::save();
        RtcManager::periodicMaintenance();
        delay(150);
        ESP.restart();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* off = powerButton(p, 292, ICO_POWER_SETTINGS_NEW, "Hold to power off", cCrit(), cText());
    lv_obj_add_event_cb(off, [](lv_event_t*) {
        SettingsStorage::save();
        haptic(HAPTIC_ALERT_LONG);
        delay(300);
        instance.pmu.shutdown();
    }, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t* hint = caption(p, "SIDE KEY OR SWIPE UP TO CANCEL", cText3());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);

    nav::PageDef d;
    d.root = p;
    d.refreshMs = 0xFFFF;
    d.ttlMs = 0;
    nav::registerPage(nav::PG_APP_POWER, d);
}

// ============================================================== CONNECT
void buildConnect(lv_obj_t* scr) {
    if (!CompanionServer::isApActive()) {
        CompanionServer::requestAp(true);
        SettingsStorage::get().wifiApEnabled = true;
        SettingsStorage::save();
    }
    lv_obj_t* p = pageRoot(scr);
    title(p, "PHONE");
    lv_obj_t* frame = box(p, 232, 232);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_radius(frame, 24, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_t* qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(qr, 200);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    char payload[96];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", CompanionServer::getApName(), AP_PASSWORD);
    lv_qrcode_update(qr, payload, strlen(payload));
    lv_obj_center(qr);

    lv_obj_t* ssid = label(p, F.body, cText(), CompanionServer::getApName());
    lv_obj_align(ssid, LV_ALIGN_TOP_MID, 0, 340);
    char pw[48];
    snprintf(pw, sizeof(pw), "PASSWORD  %s", AP_PASSWORD);
    lv_obj_t* pwl = caption(p, pw, cText2());
    lv_obj_align(pwl, LV_ALIGN_TOP_MID, 0, 378);
    lv_obj_t* hint = label(p, F.small, cText3(), "Scan to join, then open 192.168.4.1");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 412);

    nav::PageDef d;
    d.root = p;
    d.refreshMs = 0xFFFF;
    d.ttlMs = 0;
    nav::registerPage(nav::PG_APP_CONNECT, d);
}

// ============================================================== AGENDA (placeholder)
void buildAgenda(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "AGENDA");
    lv_obj_t* ic = label(p, F.icoXL, cText3(), ICO_CALENDAR_MONTH);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -40);
    lv_obj_t* t = label(p, F.body, cText2(), "No calendar connected");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t* s = caption(p, "ADD AN ICS LINK IN THE PHONE APP", cText3());
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 66);

    nav::PageDef d;
    d.root = p;
    d.refreshMs = 0xFFFF;
    d.ttlMs = 0;
    nav::registerPage(nav::PG_AGENDA, d);
}

// ============================================================== WATCHFACES SELECTOR
static lv_obj_t* sFaceCards[FACE_COUNT];
static lv_obj_t* sFaceChecks[FACE_COUNT];

static void refreshFaces(bool) {
    int cur = SettingsStorage::get().watchfaceIndex;
    for (int i = 0; i < FACE_COUNT; i++) {
        if (!sFaceCards[i]) continue;
        bool active = (i == cur);
        setBgColorIf(sFaceCards[i], active ? accent() : cSurf());
        if (sFaceChecks[i]) setHidden(sFaceChecks[i], !active);
    }
}

void buildFaces(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "WATCHFACES");

    static const char* faceNames[FACE_COUNT] = {
        "Modular", "Chronometer", "Minimal", "Wayfinder Ultra",
        "Tourbillon Skeleton", "Casio Vintage", "Bauhaus", "Cyberpunk",
        "Word Clock", "Neon Horizon", "Pilot Flieger", "Diver Pro",
        "Radar Tactical", "Lifetime"
    };

    lv_obj_t* list = lv_obj_create(p);
    lv_obj_set_size(list, CW, 380);
    lv_obj_set_pos(list, M, 74);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    for (int i = 0; i < FACE_COUNT; i++) {
        lv_obj_t* c = card(list, CW, 50);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            SettingsStorage::get().watchfaceIndex = idx;
            SettingsStorage::save();
            haptic(HAPTIC_CLICK);
            nav::home();
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        sFaceCards[i] = c;
        bool active = (SettingsStorage::get().watchfaceIndex == i);
        setBgColorIf(c, active ? accent() : cSurf());

        lv_obj_t* ic = label(c, F.icoS, cText(), ICO_WATCH);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t* lbl = label(c, F.small, cText(), faceNames[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 50, 0);

        sFaceChecks[i] = label(c, F.icoS, cText(), ICO_CHECK);
        lv_obj_align(sFaceChecks[i], LV_ALIGN_RIGHT_MID, -16, 0);
        setHidden(sFaceChecks[i], !active);
    }

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshFaces;
    d.refreshMs = 500;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_APP_FACES, d);
}

}  // namespace pages
