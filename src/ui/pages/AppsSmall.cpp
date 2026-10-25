// Smaller apps: stopwatch, power menu, phone connect, agenda & Google Calendar.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
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
        SettingsStorage::flushNow();
        RtcManager::periodicMaintenance();
        delay(150);
        ESP.restart();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* off = powerButton(p, 292, ICO_POWER_SETTINGS_NEW, "Hold to power off", cCrit(), cText());
    lv_obj_add_event_cb(off, [](lv_event_t*) {
        SettingsStorage::flushNow();
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

// ============================================================== AGENDA / GOOGLE CALENDAR
static lv_obj_t* sCalMonthLbl = nullptr;
static lv_obj_t* sCalSubLbl = nullptr;
static lv_obj_t* sCalCells[42];
static lv_obj_t* sCalCellTxts[42];
static int sCalLastMday = -1;
static int sCalLastMon = -1;

static int sCalMode = 0; // 0 = Month Grid, 1 = Upcoming Google Calendar Agenda
static lv_obj_t* sCalTabBtns[2] = {nullptr};
static lv_obj_t* sMonthCard = nullptr;
static lv_obj_t* sAgendaCard = nullptr;
static lv_obj_t* sEventRows[6] = {nullptr};
static lv_obj_t* sEventTime[6] = {nullptr};
static lv_obj_t* sEventTitle[6] = {nullptr};
static lv_obj_t* sEventLoc[6] = {nullptr};
static lv_obj_t* sEventEmpty = nullptr;
static lv_obj_t* sSyncBtn = nullptr;

static void refreshAgenda(bool force) {
    struct tm t;
    RtcManager::getLocalTimeInfo(&t);

    for (int i = 0; i < 2; i++) {
        if (sCalTabBtns[i]) setBgColorIf(sCalTabBtns[i], (i == sCalMode) ? accent() : cSurf2());
    }
    setHidden(sMonthCard, sCalMode != 0);
    setHidden(sAgendaCard, sCalMode != 1);

    if (sCalMode == 0) {
        if (!force && t.tm_mday == sCalLastMday && t.tm_mon == sCalLastMon) return;
        sCalLastMday = t.tm_mday;
        sCalLastMon = t.tm_mon;

        static const char* MONTHS_DE[12] = {
            "JANUAR", "FEBRUAR", "MAERZ", "APRIL", "MAI", "JUNI",
            "JULI", "AUGUST", "SEPTEMBER", "OKTOBER", "NOVEMBER", "DEZEMBER"
        };
        static const char* WDAYS_DE[7] = {
            "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"
        };
        int year = t.tm_year + 1900;
        int mon = t.tm_mon;
        char hdr[40];
        snprintf(hdr, sizeof(hdr), "%s %d", MONTHS_DE[mon % 12], year);
        setText(sCalMonthLbl, hdr);

        int kw = (t.tm_yday + 7 - ((t.tm_wday + 6) % 7)) / 7 + 1;
        char sub[64];
        snprintf(sub, sizeof(sub), "%s, %d. · KW %d · Tag %d",
                 WDAYS_DE[t.tm_wday % 7], t.tm_mday, kw, t.tm_yday + 1);
        setText(sCalSubLbl, sub);

        static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int daysInMon = mdays[mon % 12];
        if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) daysInMon = 29;

        int todayWdayMon = (t.tm_wday + 6) % 7;
        int firstWdayMon = (todayWdayMon - ((t.tm_mday - 1) % 7) + 7) % 7;

        for (int i = 0; i < 42; i++) {
            int dayNum = i - firstWdayMon + 1;
            if (dayNum >= 1 && dayNum <= daysInMon) {
                char db[8];
                snprintf(db, sizeof(db), "%d", dayNum);
                setText(sCalCellTxts[i], db);
                bool isToday = (dayNum == t.tm_mday);
                bool isWeekend = ((i % 7) >= 5);
                lv_obj_set_style_bg_opa(sCalCells[i], isToday ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                setBgColorIf(sCalCells[i], isToday ? accent() : cSurf());
                setTextColorIf(sCalCellTxts[i], isToday ? cBg() : (isWeekend ? cText2() : cText()));
            } else {
                setText(sCalCellTxts[i], "");
                lv_obj_set_style_bg_opa(sCalCells[i], LV_OPA_TRANSP, 0);
            }
        }
    } else {
        CalendarEvent evs[6];
        uint8_t n = BleCompanion::getUpcomingEvents(evs, 6);
        bool conn = BleCompanion::isConnected();

        char sub[64];
        snprintf(sub, sizeof(sub), "%u Termine synchronisiert · %s",
                 (unsigned)n, conn ? "BLE verbunden" : "BLE getrennt");
        setText(sCalSubLbl, sub);
        setHidden(sEventEmpty, n != 0);

        for (int i = 0; i < 6; i++) {
            bool has = (i < n);
            setHidden(sEventRows[i], !has);
            if (!has) continue;

            time_t st = (time_t)evs[i].startEpoch;
            struct tm evTm;
            localtime_r(&st, &evTm);

            char timeBuf[32];
            if (evs[i].allDay) {
                snprintf(timeBuf, sizeof(timeBuf), "%02d.%02d. Ganztag", evTm.tm_mday, evTm.tm_mon + 1);
            } else {
                snprintf(timeBuf, sizeof(timeBuf), "%02d.%02d. %02d:%02d",
                         evTm.tm_mday, evTm.tm_mon + 1, evTm.tm_hour, evTm.tm_min);
            }
            setText(sEventTime[i], timeBuf);
            setText(sEventTitle[i], evs[i].title);
            setText(sEventLoc[i], evs[i].loc[0] ? evs[i].loc : "Kein Ort");
        }
    }
}

void buildAgenda(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "KALENDER");

    sCalMonthLbl = label(p, F.body, accent(), "");
    accReg(sCalMonthLbl, ACC_TEXT);
    lv_obj_align(sCalMonthLbl, LV_ALIGN_TOP_RIGHT, -M, 20);

    // Mode Selector Pills: MONAT | TERMINE (Google Calendar)
    const int pw = (CW - 10) / 2;
    static const char* P_LABELS[2] = {"MONAT", "TERMINE (SYNC)"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t* b = box(p, pw, 32);
        lv_obj_set_pos(b, M + i * (pw + 10), 56);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, (i == 0) ? accent() : cSurf2(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            sCalMode = (int)(intptr_t)lv_event_get_user_data(e);
            if (sCalMode == 1 && BleCompanion::isConnected()) {
                BleCompanion::triggerCalendarSync();
            }
            haptic(HAPTIC_CLICK);
            refreshAgenda(true);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_center(caption(b, P_LABELS[i], cText()));
        sCalTabBtns[i] = b;
    }

    sCalSubLbl = caption(p, "", cText2());
    lv_obj_set_pos(sCalSubLbl, M, 94);

    // =========================================================================
    // View 0: Month Grid Card
    // =========================================================================
    sMonthCard = card(p, CW, 350);
    lv_obj_set_pos(sMonthCard, M, 116);

    static const char* dow[7] = {"MO", "DI", "MI", "DO", "FR", "SA", "SO"};
    const int cellW = (CW - 24) / 7;
    for (int d = 0; d < 7; d++) {
        lv_obj_t* dl = caption(sMonthCard, dow[d], d >= 5 ? accent() : cText3());
        if (d >= 5) accReg(dl, ACC_TEXT);
        lv_obj_set_pos(dl, 12 + d * cellW + (cellW - 22) / 2, 12);
    }

    for (int i = 0; i < 42; i++) {
        int col = i % 7;
        int row = i / 7;
        lv_obj_t* cell = box(sMonthCard, cellW - 4, 43);
        lv_obj_set_pos(cell, 14 + col * cellW, 42 + row * 48);
        lv_obj_set_style_radius(cell, 12, 0);
        sCalCells[i] = cell;
        sCalCellTxts[i] = label(cell, F.small, cText(), "");
        lv_obj_center(sCalCellTxts[i]);
    }

    // =========================================================================
    // View 1: Google Calendar Agenda List Card
    // =========================================================================
    sAgendaCard = card(p, CW, 350);
    lv_obj_set_pos(sAgendaCard, M, 116);

    sEventEmpty = label(sAgendaCard, F.small, cText2(),
                        "Keine Termine empfangen.\nHandy über BLE mit der App verbinden.");
    lv_obj_set_pos(sEventEmpty, 16, 40);

    for (int i = 0; i < 6; i++) {
        lv_obj_t* r = box(sAgendaCard, CW - 24, 46);
        lv_obj_set_pos(r, 12, 10 + i * 50);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, cSurf2(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_60, 0);

        sEventTime[i] = label(r, F.small, accent(), "");
        accReg(sEventTime[i], ACC_TEXT);
        lv_obj_set_pos(sEventTime[i], 10, 4);

        sEventTitle[i] = label(r, F.small, cText(), "");
        lv_obj_set_pos(sEventTitle[i], 120, 4);
        lv_label_set_long_mode(sEventTitle[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(sEventTitle[i], 210);

        sEventLoc[i] = caption(r, "", cText3());
        lv_obj_set_pos(sEventLoc[i], 10, 26);
        sEventRows[i] = r;
    }

    sSyncBtn = box(sAgendaCard, CW - 24, 38);
    lv_obj_set_pos(sSyncBtn, 12, 304);
    lv_obj_set_style_radius(sSyncBtn, 12, 0);
    lv_obj_set_style_bg_color(sSyncBtn, accent(), 0);
    lv_obj_set_style_bg_opa(sSyncBtn, LV_OPA_COVER, 0);
    accReg(sSyncBtn, ACC_BG);
    lv_obj_add_flag(sSyncBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sSyncBtn, [](lv_event_t*) {
        BleCompanion::triggerCalendarSync();
        haptic(HAPTIC_CLICK);
        DisplayEngine::showNotification("Kalender Sync", "Synchronisierungsanfrage per BLE gesendet", 2000, "sync");
        refreshAgenda(true);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(sSyncBtn, F.small, cBg(), "GOOGLE KALENDER JETZT SYNCHRONISIEREN"));

    setHidden(sAgendaCard, true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshAgenda;
    d.refreshMs = 5000;
    d.ttlMs = 60000;
    d.onShow = [] {
        if (BleCompanion::isConnected()) {
            BleCompanion::triggerCalendarSync();
        }
    };
    nav::registerPage(nav::PG_AGENDA, d);
}

// ============================================================== WATCHFACES SELECTOR
static lv_obj_t* sFaceCards[FACE_COUNT];
static lv_obj_t* sFaceIcons[FACE_COUNT];
static lv_obj_t* sFaceLabels[FACE_COUNT];
static lv_obj_t* sFaceChecks[FACE_COUNT];

static void refreshFaces(bool) {
    int cur = SettingsStorage::get().watchfaceIndex;
    for (int i = 0; i < FACE_COUNT; i++) {
        if (!sFaceCards[i]) continue;
        bool active = (i == cur);
        setBgColorIf(sFaceCards[i], active ? accent() : cSurf());
        setBorderWidthIf(sFaceCards[i], active ? 0 : 1);
        if (sFaceIcons[i])  setTextColorIf(sFaceIcons[i],  active ? cBg() : accent());
        if (sFaceLabels[i]) setTextColorIf(sFaceLabels[i], active ? cBg() : cText());
        if (sFaceChecks[i]) {
            setTextColorIf(sFaceChecks[i], cBg());
            setHidden(sFaceChecks[i], !active);
        }
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
    lv_obj_set_size(list, CW, 390);
    lv_obj_set_pos(list, M, 74);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    for (int i = 0; i < FACE_COUNT; i++) {
        lv_obj_t* c = card(list, CW, 52);
        lv_obj_set_style_radius(c, 18, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            SettingsStorage::get().watchfaceIndex = idx;
            SettingsStorage::save();
            refreshFaces(true);
            nav::markDirty(nav::PG_FACE);
            haptic(HAPTIC_CLICK);
            nav::home();
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        sFaceCards[i] = c;
        sFaceIcons[i] = label(c, F.icoS, accent(), ICO_WATCH);
        lv_obj_align(sFaceIcons[i], LV_ALIGN_LEFT_MID, 16, 0);
        sFaceLabels[i] = label(c, F.small, cText(), faceNames[i]);
        lv_obj_align(sFaceLabels[i], LV_ALIGN_LEFT_MID, 50, 0);

        sFaceChecks[i] = label(c, F.icoS, cBg(), ICO_CHECK);
        lv_obj_align(sFaceChecks[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }
    refreshFaces(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshFaces;
    d.refreshMs = 500;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_APP_FACES, d);
}

// ============================================================== SETTINGS APP
static const uint16_t ACCENT_PALETTE[6] = {
    THEME_CYBER_CYAN, THEME_ELECTRIC_LIME, THEME_NEON_AMBER,
    THEME_MAGENTA_PULSE, THEME_CRIMSON_WAR, THEME_STEALTH_BLUE
};
static lv_obj_t* sColorDots[6];
static lv_obj_t* sSetSoundVal = nullptr;
static lv_obj_t* sSetBeaconVal = nullptr;
static lv_obj_t* sSetGoalVal = nullptr;

static lv_color_t colorFrom565(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F) << 3; r |= r >> 5;
    uint8_t g = ((c >> 5) & 0x3F) << 2;  g |= g >> 6;
    uint8_t b = (c & 0x1F) << 3;         b |= b >> 5;
    return lv_color_make(r, g, b);
}

static void refreshSettings(bool) {
    SystemSettings& s = SettingsStorage::get();
    for (int i = 0; i < 6; i++) {
        if (!sColorDots[i]) continue;
        bool sel = (s.accentColor == ACCENT_PALETTE[i]);
        lv_obj_set_style_border_width(sColorDots[i], sel ? 3 : 0, 0);
    }
    if (sSetSoundVal)  setText(sSetSoundVal, s.soundEnabled ? "ON" : "OFF");
    if (sSetBeaconVal) setText(sSetBeaconVal, s.loraBeaconEnabled ? "ON" : "OFF");
    if (sSetGoalVal) {
        char b[24];
        snprintf(b, sizeof(b), "%lu", (unsigned long)s.dailyStepGoal);
        setText(sSetGoalVal, b);
    }
}

void buildSettings(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "SETTINGS");

    // Accent color card
    lv_obj_t* cc = card(p, CW, 92);
    lv_obj_set_pos(cc, M, 76);
    lv_obj_t* cl = caption(cc, "ACCENT COLOR", cText2());
    lv_obj_set_pos(cl, 18, 12);
    const int dotSpacing = (CW - 36) / 6;
    for (int i = 0; i < 6; i++) {
        lv_obj_t* dot = box(cc, 40, 40);
        lv_obj_set_pos(dot, 22 + i * dotSpacing, 38);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, colorFrom565(ACCENT_PALETTE[i]), 0);
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dot, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            SettingsStorage::get().accentColor = ACCENT_PALETTE[idx];
            SettingsStorage::save();
            refreshSettings(true);
            haptic(HAPTIC_CLICK);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        sColorDots[i] = dot;
    }

    auto makeOptionRow = [&](int y, const char* ic, const char* txt, lv_obj_t** valOut, lv_event_cb_t cb) {
        lv_obj_t* r = card(p, CW, 56);
        lv_obj_set_style_radius(r, 18, 0);
        lv_obj_set_pos(r, M, y);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t* iObj = label(r, F.icoS, accent(), ic);
        accReg(iObj, ACC_TEXT);
        lv_obj_align(iObj, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t* tObj = label(r, F.small, cText(), txt);
        lv_obj_align(tObj, LV_ALIGN_LEFT_MID, 50, 0);
        if (valOut) {
            *valOut = label(r, F.small, accent(), "");
            accReg(*valOut, ACC_TEXT);
            lv_obj_align(*valOut, LV_ALIGN_RIGHT_MID, -18, 0);
        }
    };

    makeOptionRow(178, ICO_VOLUME_UP, "Sound Effects", &sSetSoundVal, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.soundEnabled = !s.soundEnabled;
        SettingsStorage::save();
        if (s.soundEnabled) AudioEngine::playSoundFx(SFX_CHIME);
        refreshSettings(true);
        haptic(HAPTIC_CLICK);
    });

    makeOptionRow(242, ICO_PODCASTS, "LoRa Auto-Beacon", &sSetBeaconVal, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.loraBeaconEnabled = !s.loraBeaconEnabled;
        SettingsStorage::save();
        refreshSettings(true);
        haptic(HAPTIC_CLICK);
    });

    makeOptionRow(306, ICO_FOOTPRINT, "Daily Step Goal", &sSetGoalVal, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        static const uint32_t goals[] = {5000, 8000, 10000, 12000, 15000, 20000};
        int next = 2;
        for (int i = 0; i < 6; i++) {
            if (s.dailyStepGoal == goals[i]) { next = (i + 1) % 6; break; }
        }
        s.dailyStepGoal = goals[next];
        SettingsStorage::save();
        nav::markDirty(nav::PG_ACTIVITY);
        refreshSettings(true);
        haptic(HAPTIC_CLICK);
    });

    makeOptionRow(370, ICO_POWER_SETTINGS_NEW, "Power & Restart Menu", nullptr, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        nav::openApp(nav::PG_APP_POWER);
    });

    refreshSettings(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshSettings;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_APP_SETTINGS, d);
}

}  // namespace pages
