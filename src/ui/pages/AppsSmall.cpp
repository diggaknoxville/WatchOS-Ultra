// Smaller apps: stopwatch, power menu, phone connect, calendar, face picker,
// settings and the media remote.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
#include "PowerManager.h"
#include "SettingsStorage.h"
#include "RtcManager.h"
#include "AudioEngine.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include "Config.h"
#include <LilyGoLib.h>
#include <type_traits>

using namespace ui;

namespace pages {

// ============================================================== STOPWATCH
static lv_obj_t *swTime, *swCs, *swStart, *swLap, *swLaps[4];
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
    setButtonText(swStart, swRun ? "STOPP" : (e ? "WEITER" : "START"));
    setBgColorIf(swStart, swRun ? cCrit() : cOk());
    setButtonText(swLap, swRun ? "RUNDE" : "RESET");
    for (int i = 0; i < 4; i++) {
        bool has = i < swLapN;
        setHidden(swLaps[i], !has);
        if (!has) continue;
        char t[24];
        swFmt(t, sizeof(t), swLapTimes[i], true);
        snprintf(b, sizeof(b), "RUNDE %d   %s", swLapN - i, t);
        setText(swLaps[i], b);
    }
}

void buildStopwatch(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "STOPPUHR");
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
    swLap = secondaryButton(p, bw, 76, nullptr, "RESET");
    lv_obj_set_pos(swLap, M, 386);
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

    swStart = primaryButton(p, bw, 76, nullptr, "START");
    lv_obj_set_pos(swStart, M + bw + 14, 386);
    lv_obj_set_style_bg_color(swStart, cOk(), 0);
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
static void doRestart() {
    SettingsStorage::flushNow();
    RtcManager::periodicMaintenance();
    ESP.restart();
}

static void onRestartConfirm(bool ok, void*) { if (ok) doRestart(); }

void buildPower(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "ENERGIE");

    lv_obj_t* sleepB = secondaryButton(p, CW, 76, ICO_BEDTIME, "Display aus");
    lv_obj_set_pos(sleepB, M, 96);
    lv_obj_add_event_cb(sleepB, [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        nav::home();
        PowerManager::requestSleep();              // honoured by PowerManager for a grace window
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* rst = secondaryButton(p, CW, 76, ICO_RESTART_ALT, "Neustart");
    lv_obj_set_pos(rst, M, 184);
    lv_obj_add_event_cb(rst, [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        confirmDialog("Neustart?", "Die Uhr startet neu. Einstellungen werden gespeichert.",
                      "Neustart", "Abbrechen", onRestartConfirm, nullptr, false);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* off = dangerButton(p, CW, 76, ICO_POWER_SETTINGS_NEW, "Ausschalten (halten)");
    lv_obj_set_pos(off, M, 272);
    lv_obj_add_event_cb(off, [](lv_event_t*) {
        SettingsStorage::flushNow();
        haptic(HAPTIC_ALERT_LONG);
        if (PowerManager::lockI2C(500)) {
            instance.pmu.shutdown();
            PowerManager::unlockI2C();
        }
    }, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t* hint = caption(p, "SEITENTASTE = ZURÜCK", cText3());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_t* hint2 = caption(p, "VON UNTEN NACH OBEN WISCHEN = ZURÜCK", cText3());
    lv_obj_align(hint2, LV_ALIGN_BOTTOM_MID, 0, -(nav::HOME_EDGE / 2 - 8));   // inside the home edge zone
    lv_obj_t* grab = box(p, 40, 5);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, cText3(), 0);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -8);

    nav::PageDef d;
    d.root = p;
    d.refreshMs = 0xFFFF;
    d.ttlMs = 0;
    nav::registerPage(nav::PG_APP_POWER, d);
}

// ============================================================== CONNECT
// 6-digit BLE pairing passkey (BleCompanion::getPasskey() returns uint32_t)
[[maybe_unused]] static bool blePasskeyText(char* out, size_t n) {
    snprintf(out, n, "%06lu", (unsigned long)BleCompanion::getPasskey() % 1000000UL);
    return true;
}

static lv_obj_t *cnActive, *cnInactive, *cnQr, *cnSsid, *cnPw, *cnPin, *cnBle, *cnStartBtn, *cnStatusPill;
static char cnPayload[96] = {0};

static void refreshConnect(bool) {
    char b[80];
    bool ap = CompanionServer::isApActive();
    bool want = SettingsStorage::get().wifiApEnabled;
    setHidden(cnActive, !ap);
    setHidden(cnInactive, ap);
    if (ap) {
        setPill(cnStatusPill, "HOTSPOT AKTIV", cOk());
        const char* name = CompanionServer::getApName();
        const char* pw = CompanionServer::getApPassword();
        const char* pin = CompanionServer::getPortalPin();
        char payload[96];
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", name ? name : "", pw ? pw : "");
        if (strcmp(payload, cnPayload) != 0) {
            strncpy(cnPayload, payload, sizeof(cnPayload) - 1);
            cnPayload[sizeof(cnPayload) - 1] = 0;
            lv_qrcode_update(cnQr, cnPayload, strlen(cnPayload));
        }
        setText(cnSsid, name ? name : "--");
        snprintf(b, sizeof(b), "PASSWORT  %s", pw ? pw : "--");
        setText(cnPw, b);
        snprintf(b, sizeof(b), "PORTAL-PIN  %s", pin ? pin : "--");
        setText(cnPin, b);
    } else {
        setPill(cnStatusPill, want ? "HOTSPOT STARTET" : "HOTSPOT AUS", want ? cWarn() : cText2());
        setButtonText(cnStartBtn, want ? "Hotspot startet..." : "Hotspot starten");
        setButtonEnabled(cnStartBtn, !want);
    }
    if (BleCompanion::isEnabled()) {
        snprintf(b, sizeof(b), "Bluetooth: %s", BleCompanion::isConnected() ? "Verbunden" : "Bereit zur Kopplung");
    } else {
        snprintf(b, sizeof(b), "Bluetooth ist aus");
    }
    setText(cnBle, b);
}

void buildConnect(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "VERBINDEN");
    cnStatusPill = pill(p, "HOTSPOT AUS", cText2());
    lv_obj_align(cnStatusPill, LV_ALIGN_TOP_RIGHT, -M, 24);

    // --- hotspot running: QR + credentials ---
    cnActive = box(p, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(cnActive, 0, CONTENT_Y);
    lv_obj_t* frame = box(cnActive, 196, 196);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(frame, 20, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    cnQr = lv_qrcode_create(frame);
    lv_qrcode_set_size(cnQr, 172);
    lv_qrcode_set_dark_color(cnQr, lv_color_black());
    lv_qrcode_set_light_color(cnQr, lv_color_white());
    lv_qrcode_update(cnQr, "WIFI:T:WPA;S:-;P:-;;", 20);
    lv_obj_center(cnQr);

    cnSsid = label(cnActive, F.body, cText(), "--");
    lv_obj_align(cnSsid, LV_ALIGN_TOP_MID, 0, 208);
    cnPw = caption(cnActive, "PASSWORT  --", cText2());
    lv_obj_align(cnPw, LV_ALIGN_TOP_MID, 0, 242);
    cnPin = caption(cnActive, "PORTAL-PIN  --", accent());
    accReg(cnPin, ACC_TEXT);
    lv_obj_align(cnPin, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_t* hint = label(cnActive, F.small, cText3(), "QR scannen, dann 192.168.4.1 öffnen");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 296);

    // --- hotspot off: explain + start button ---
    cnInactive = box(p, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(cnInactive, 0, CONTENT_Y);
    lv_obj_t* es = emptyState(cnInactive, ICO_WIFI_OFF, "Hotspot ist aus",
                              "Startet ein WLAN für die Companion-Web-App");
    lv_obj_align(es, LV_ALIGN_TOP_MID, 0, 8);
    cnStartBtn = primaryButton(cnInactive, CW, BTN_H, ICO_WIFI, "Hotspot starten");
    lv_obj_align(cnStartBtn, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_add_event_cb(cnStartBtn, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.wifiApEnabled = true;                     // explicit user action
        SettingsStorage::save();
        CompanionServer::requestAp(true);
        haptic(HAPTIC_CLICK);
        DisplayEngine::showToast("Hotspot", "Wird gestartet...", 2000, ICO_WIFI);
        nav::markDirty(nav::PG_CONTROL);
        refreshConnect(true);
    }, LV_EVENT_CLICKED, NULL);

    // --- Bluetooth line (both states) ---
    cnBle = label(p, F.small, cText2(), "Bluetooth ist aus");
    lv_obj_set_width(cnBle, CW);
    lv_obj_set_style_text_align(cnBle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(cnBle, LV_ALIGN_BOTTOM_MID, 0, -40);

    setHidden(cnActive, true);
    refreshConnect(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshConnect;
    d.refreshMs = 1000;
    d.ttlMs = 5000;
    d.onShow = [] {
        // never start the AP from a builder; only if the user has enabled it
        if (SettingsStorage::get().wifiApEnabled && !CompanionServer::isApActive()) CompanionServer::requestAp(true);
    };
    nav::registerPage(nav::PG_APP_CONNECT, d);
}

// ============================================================== KALENDER
static lv_obj_t* sCalMonthLbl = nullptr;
static lv_obj_t* sCalSubLbl = nullptr;
static lv_obj_t* sCalCells[42];
static lv_obj_t* sCalCellTxts[42];
static int sCalLastMday = -1;
static int sCalLastMon = -1;

static int sCalMode = 0; // 0 = month grid, 1 = synced events
static lv_obj_t* sCalTabs = nullptr;
static lv_obj_t* sMonthCard = nullptr;
static lv_obj_t* sAgendaCard = nullptr;
static lv_obj_t* sEventRows[6] = {nullptr};
static lv_obj_t* sEventTime[6] = {nullptr};
static lv_obj_t* sEventTitle[6] = {nullptr};
static lv_obj_t* sEventLoc[6] = {nullptr};
static lv_obj_t* sEventEmpty = nullptr;
static lv_obj_t* sSyncBtn = nullptr;
static lv_obj_t* sSyncLbl = nullptr;
static uint32_t sSyncReqMs = 0;

static void requestCalendarSync(bool fromButton) {
    if (!BleCompanion::isConnected()) {
        if (fromButton) DisplayEngine::showToast("Kalender", "Handy nicht per Bluetooth verbunden", 2200, ICO_SMARTPHONE);
        return;
    }
    bool ok = BleCompanion::triggerCalendarSync();      // rate-limited (60 s)
    if (ok) sSyncReqMs = millis();
    else if (fromButton) DisplayEngine::showToast("Kalender", "Bereits angefragt, bitte kurz warten", 2200, ICO_SYNC);
}

static void refreshAgenda(bool force) {
    struct tm t;
    RtcManager::getLocalTimeInfo(&t);

    setTabActive(sCalTabs, sCalMode);
    setHidden(sMonthCard, sCalMode != 0);
    setHidden(sAgendaCard, sCalMode != 1);

    if (sCalMode == 0) {
        if (!force && t.tm_mday == sCalLastMday && t.tm_mon == sCalLastMon) return;
        sCalLastMday = t.tm_mday;
        sCalLastMon = t.tm_mon;

        int year = t.tm_year + 1900;
        int mon = t.tm_mon;
        char hdr[40];
        snprintf(hdr, sizeof(hdr), "%s %d", MO_DE_L[mon % 12], year);
        setText(sCalMonthLbl, hdr);

        char kw[8];
        strftime(kw, sizeof(kw), "%V", &t);                 // ISO 8601 week
        char sub[64];
        static const char* WDAYS_DE[7] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
        snprintf(sub, sizeof(sub), "%s, %d. · KW %s · Tag %d", WDAYS_DE[t.tm_wday % 7], t.tm_mday, kw, t.tm_yday + 1);
        setText(sCalSubLbl, sub);

        int daysInMon = daysInMonth(year, mon);
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
                setBgOpaIf(sCalCells[i], isToday ? LV_OPA_COVER : LV_OPA_TRANSP);
                setBgColorIf(sCalCells[i], isToday ? accent() : cSurf());
                setTextColorIf(sCalCellTxts[i], isToday ? cBg() : (isWeekend ? cText2() : cText()));
            } else {
                setText(sCalCellTxts[i], "");
                setBgOpaIf(sCalCells[i], LV_OPA_TRANSP);
            }
        }
    } else {
        static CalendarEvent evs[6];
        uint8_t n = BleCompanion::getUpcomingEvents(evs, 6);
        bool conn = BleCompanion::isConnected();

        char sub[64];
        snprintf(sub, sizeof(sub), "%u Termine · %s", (unsigned)n, conn ? "Bluetooth verbunden" : "Bluetooth getrennt");
        setText(sCalSubLbl, sub);
        setHidden(sEventEmpty, n != 0);

        char st[48];
        uint32_t last = BleCompanion::lastCalendarSyncMs();
        if (sSyncReqMs && millis() - sSyncReqMs < 8000 && (last == 0 || last < sSyncReqMs)) {
            snprintf(st, sizeof(st), "Synchronisiert...");
        } else if (last) {
            char a[24];
            agoMs(a, sizeof(a), last);
            snprintf(st, sizeof(st), "Zuletzt %s", a);
        } else {
            snprintf(st, sizeof(st), "Noch nicht synchronisiert");
        }
        setText(sSyncLbl, st);
        setButtonEnabled(sSyncBtn, conn);

        for (int i = 0; i < 6; i++) {
            bool has = (i < n);
            setHidden(sEventRows[i], !has);
            if (!has) continue;
            time_t s = (time_t)evs[i].startEpoch;
            struct tm evTm;
            localtime_r(&s, &evTm);
            char timeBuf[32];
            if (evs[i].allDay) snprintf(timeBuf, sizeof(timeBuf), "%02d.%02d. ganztags", evTm.tm_mday, evTm.tm_mon + 1);
            else snprintf(timeBuf, sizeof(timeBuf), "%02d.%02d. %02d:%02d", evTm.tm_mday, evTm.tm_mon + 1, evTm.tm_hour, evTm.tm_min);
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
    lv_obj_align(sCalMonthLbl, LV_ALIGN_TOP_RIGHT, -M, 24);

    static const char* TABS[2] = {"MONAT", "TERMINE"};
    sCalTabs = tabBar(p, M, CONTENT_Y, CW, TABS, 2, 0, [](lv_event_t* e) {
        sCalMode = (int)(intptr_t)lv_event_get_user_data(e);
        haptic(HAPTIC_TICK);
        refreshAgenda(true);
    });

    sCalSubLbl = caption(p, "", cText2());
    lv_obj_set_pos(sCalSubLbl, M, CONTENT_Y + 40);

    // --- month grid ---
    sMonthCard = card(p, CW, 350);
    lv_obj_set_pos(sMonthCard, M, CONTENT_Y + 64);
    static const char* dow[7] = {"MO", "DI", "MI", "DO", "FR", "SA", "SO"};
    const int cellW = (CW - 24) / 7;
    for (int d = 0; d < 7; d++) {
        lv_obj_t* dl = caption(sMonthCard, dow[d], d >= 5 ? accent() : cText3());
        if (d >= 5) accReg(dl, ACC_TEXT);
        lv_obj_set_pos(dl, 12 + d * cellW + (cellW - 22) / 2, 12);
    }
    for (int i = 0; i < 42; i++) {
        lv_obj_t* cell = box(sMonthCard, cellW - 4, 43);
        lv_obj_set_pos(cell, 14 + (i % 7) * cellW, 42 + (i / 7) * 48);
        lv_obj_set_style_radius(cell, 12, 0);
        sCalCells[i] = cell;
        sCalCellTxts[i] = label(cell, F.small, cText(), "");
        lv_obj_center(sCalCellTxts[i]);
    }

    // --- synced events ---
    sAgendaCard = card(p, CW, 350);
    lv_obj_set_pos(sAgendaCard, M, CONTENT_Y + 64);
    sEventEmpty = emptyState(sAgendaCard, ICO_EVENT, "Keine Termine", "Companion-App per Bluetooth verbinden");
    lv_obj_align(sEventEmpty, LV_ALIGN_TOP_MID, 0, 30);
    for (int i = 0; i < 6; i++) {
        lv_obj_t* r = box(sAgendaCard, CW - 24, 40);
        lv_obj_set_pos(r, 12, 10 + i * 44);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, cSurf2(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_60, 0);
        sEventTime[i] = caption(r, "", accent());
        accReg(sEventTime[i], ACC_TEXT);
        lv_obj_set_pos(sEventTime[i], 10, 2);
        sEventTitle[i] = label(r, F.small, cText(), "");
        lv_obj_set_pos(sEventTitle[i], 118, 8);
        lv_label_set_long_mode(sEventTitle[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(sEventTitle[i], 210);
        sEventLoc[i] = caption(r, "", cText3());
        lv_obj_set_pos(sEventLoc[i], 10, 20);
        sEventRows[i] = r;
    }
    sSyncLbl = caption(sAgendaCard, "Noch nicht synchronisiert", cText3());
    lv_obj_set_pos(sSyncLbl, 16, 282);
    sSyncBtn = primaryButton(sAgendaCard, CW - 24, 40, ICO_SYNC, "Jetzt synchronisieren");
    lv_obj_set_pos(sSyncBtn, 12, 300);
    lv_obj_add_event_cb(sSyncBtn, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        requestCalendarSync(true);
        refreshAgenda(true);
    }, LV_EVENT_CLICKED, NULL);

    setHidden(sAgendaCard, true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshAgenda;
    d.refreshMs = 2000;
    d.ttlMs = 60000;
    d.onShow = [] { requestCalendarSync(false); };      // once per show, rate-limited by BleCompanion
    nav::registerPage(nav::PG_AGENDA, d);
}

// ============================================================== FACE PICKER
static int sPick = 0;
static lv_obj_t *fpName, *fpDesc, *fpIdx, *fpApply, *fpActiveTag;
static lv_obj_t* fpDots[FACE_COUNT];

static void refreshFaces(bool) {
    int cur = SettingsStorage::get().watchfaceIndex;
    char b[16];
    setText(fpName, faceName(sPick));
    setText(fpDesc, faceDescription(sPick));
    snprintf(b, sizeof(b), "%d / %d", sPick + 1, FACE_COUNT);
    setText(fpIdx, b);
    bool active = (sPick == cur);
    setHidden(fpActiveTag, !active);
    setButtonText(fpApply, active ? "Aktiv" : "Auswählen");
    setButtonEnabled(fpApply, !active);
    for (int i = 0; i < FACE_COUNT; i++) {
        setBgColorIf(fpDots[i], i == sPick ? accent() : (i == cur ? cText2() : cStroke()));
    }
}

static void pickStep(int d) {
    sPick = ((sPick + d) % FACE_COUNT + FACE_COUNT) % FACE_COUNT;
    haptic(HAPTIC_TICK);
    refreshFaces(true);
}

void buildFaces(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "ZIFFERBLATT");

    lv_obj_t* c = card(p, CW, 250);
    lv_obj_set_pos(c, M, CONTENT_Y);
    lv_obj_t* ic = label(c, F.icoXL, accent(), ICO_WATCH);
    accReg(ic, ACC_TEXT);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 22);
    fpIdx = caption(c, "1 / 14", cText3());
    lv_obj_align(fpIdx, LV_ALIGN_TOP_RIGHT, -16, 12);
    fpActiveTag = pill(c, "AKTIV", cOk());
    lv_obj_align(fpActiveTag, LV_ALIGN_TOP_LEFT, 12, 8);
    fpName = label(c, F.title, cText(), "");
    lv_obj_align(fpName, LV_ALIGN_TOP_MID, 0, 104);
    fpDesc = label(c, F.small, cText2(), "");
    lv_obj_set_width(fpDesc, CW - 32);
    lv_obj_set_style_text_align(fpDesc, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(fpDesc, LV_LABEL_LONG_WRAP);
    lv_obj_align(fpDesc, LV_ALIGN_TOP_MID, 0, 150);

    lv_obj_t* dots = box(c, CW - 32, 10);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 6, 0);
    for (int i = 0; i < FACE_COUNT; i++) {
        fpDots[i] = box(dots, 8, 8);
        lv_obj_set_style_radius(fpDots[i], 4, 0);
        lv_obj_set_style_bg_opa(fpDots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(fpDots[i], cStroke(), 0);
    }

    // carousel controls: prev / select / next
    lv_obj_t* prev = iconButton(p, 64, ICO_CHEVRON_LEFT, cSurf2(), cText());
    lv_obj_set_pos(prev, M, CONTENT_Y + 266);
    lv_obj_add_event_cb(prev, [](lv_event_t*) { pickStep(-1); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* next = iconButton(p, 64, ICO_CHEVRON_RIGHT, cSurf2(), cText());
    lv_obj_set_pos(next, M + CW - 64, CONTENT_Y + 266);
    lv_obj_add_event_cb(next, [](lv_event_t*) { pickStep(1); }, LV_EVENT_CLICKED, NULL);
    fpApply = primaryButton(p, CW - 2 * 76, 64, ICO_CHECK, "Auswählen");
    lv_obj_set_pos(fpApply, M + 76, CONTENT_Y + 266);
    lv_obj_add_event_cb(fpApply, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        faceSelect(sPick);                          // stores + toast
        refreshFaces(true);
        nav::home();
    }, LV_EVENT_CLICKED, NULL);

    // swipe inside the card also browses
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, [](lv_event_t* e) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT) pickStep(1);
        else if (dir == LV_DIR_RIGHT) pickStep(-1);
        (void)e;
    }, LV_EVENT_GESTURE, NULL);

    lv_obj_t* hint = caption(p, "LANGES DRÜCKEN AUF DEM ZIFFERBLATT WECHSELT AUCH", cText3());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    sPick = SettingsStorage::get().watchfaceIndex % FACE_COUNT;
    refreshFaces(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshFaces;
    d.refreshMs = 500;
    d.ttlMs = 15000;
    d.onShow = [] { sPick = SettingsStorage::get().watchfaceIndex % FACE_COUNT; refreshFaces(true); };
    nav::registerPage(nav::PG_APP_FACES, d);
}

// ============================================================== EINSTELLUNGEN
static const uint16_t ACCENT_PALETTE[6] = {
    THEME_CYBER_CYAN, THEME_ELECTRIC_LIME, THEME_NEON_AMBER,
    THEME_MAGENTA_PULSE, THEME_CRIMSON_WAR, THEME_STEALTH_BLUE
};
static const uint16_t TIMEOUT_STEPS[] = {15, 30, 60, 0};        // 0 = Nie
static const uint32_t GOAL_STEPS[] = {5000, 8000, 10000, 12000, 15000, 20000};

enum SetRow : uint8_t {
    SR_AOD = 0, SR_WRIST, SR_WIFI, SR_BLE, SR_GPS, SR_LORA, SR_BEACON, SR_SOUND, SR_HAPTIC,   // toggles
    SR_TIMEOUT, SR_GOAL, SR_TZ, SR_SYNC, SR_ABOUT,                                           // value rows
    SR_COUNT
};
static lv_obj_t* sSetRows[SR_COUNT] = {nullptr};
static lv_obj_t* sColorDots[6];

static bool setRowState(int id) {
    SystemSettings& s = SettingsStorage::get();
    switch (id) {
        case SR_AOD:    return s.aodEnabled;
        case SR_WRIST:  return s.wristWakeEnabled;
        case SR_WIFI:   return s.wifiApEnabled;
        case SR_BLE:    return BleCompanion::isEnabled();
        case SR_GPS:    return GpsManager::isEnabled();
        case SR_LORA:   return RadioLoRa::isEnabled();
        case SR_BEACON: return s.loraBeaconEnabled;
        case SR_SOUND:  return s.soundEnabled;
        case SR_HAPTIC: return s.hapticEnabled;
        default: return false;
    }
}

static void refreshSettings(bool) {
    SystemSettings& s = SettingsStorage::get();
    for (int i = 0; i < 6; i++) {
        if (sColorDots[i]) setBorderWidthIf(sColorDots[i], (s.accentColor == ACCENT_PALETTE[i]) ? 3 : 0);
    }
    for (int i = SR_AOD; i <= SR_HAPTIC; i++) setToggleRow(sSetRows[i], setRowState(i));
    char b[48];
    if (s.autoSleepSec == 0) snprintf(b, sizeof(b), "Nie");
    else snprintf(b, sizeof(b), "%u s", (unsigned)s.autoSleepSec);
    setValueRow(sSetRows[SR_TIMEOUT], b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)s.dailyStepGoal);
    setValueRow(sSetRows[SR_GOAL], b);
    const char* tz = RtcManager::timezoneName();
    setValueRow(sSetRows[SR_TZ], tz && tz[0] ? tz : "--");
    char a[24];
    ago(a, sizeof(a), RtcManager::lastSyncEpoch());
    const char* src = RtcManager::lastSyncSource();
    snprintf(b, sizeof(b), "%s %s", src && src[0] ? src : "--", a);
    setValueRow(sSetRows[SR_SYNC], b);
}

static void onSetToggle(lv_event_t* e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    SystemSettings& s = SettingsStorage::get();
    switch (id) {
        case SR_AOD:    s.aodEnabled = !s.aodEnabled; break;
        case SR_WRIST:  s.wristWakeEnabled = !s.wristWakeEnabled; break;
        case SR_WIFI:
            s.wifiApEnabled = !s.wifiApEnabled;
            CompanionServer::requestAp(s.wifiApEnabled);
            nav::markDirty(nav::PG_APP_CONNECT);
            if (s.wifiApEnabled) DisplayEngine::showToast("Hotspot", "Zugangsdaten unter Verbinden", 2200, ICO_WIFI);
            break;
        case SR_BLE:    BleCompanion::setEnabled(!BleCompanion::isEnabled()); nav::markDirty(nav::PG_APP_CONNECT); break;
        case SR_GPS:    GpsManager::setEnabled(!GpsManager::isEnabled()); nav::markDirty(nav::PG_POSITION); break;
        case SR_LORA:   RadioLoRa::setEnabled(!RadioLoRa::isEnabled()); nav::markDirty(nav::PG_LORA); break;
        case SR_BEACON: s.loraBeaconEnabled = !s.loraBeaconEnabled; break;
        case SR_SOUND:  s.soundEnabled = !s.soundEnabled; if (s.soundEnabled) AudioEngine::playSoundFx(SFX_CHIME); break;
        case SR_HAPTIC: s.hapticEnabled = !s.hapticEnabled; break;
        default: break;
    }
    SettingsStorage::save();
    nav::markDirty(nav::PG_CONTROL);
    haptic(HAPTIC_TICK);
    refreshSettings(true);
}

static void onFactoryConfirm(bool ok, void*) {
    if (!ok) return;
    SettingsStorage::resetDefaults();
    SettingsStorage::flushNow();
    ESP.restart();
}

void buildSettings(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "EINSTELLUNGEN");

    lv_obj_t* list = lv_obj_create(p);
    lv_obj_set_size(list, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(list, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_hor(list, M, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    // ---- ANZEIGE ----
    sectionHeader(list, -1, "ANZEIGE");
    lv_obj_t* cc = card(list, CW, 92);
    lv_obj_set_style_radius(cc, R_TILE, 0);
    lv_obj_t* cl = caption(cc, "AKZENTFARBE", cText2());
    lv_obj_set_pos(cl, 18, 12);
    const int dotSpacing = (CW - 36) / 6;
    for (int i = 0; i < 6; i++) {
        lv_obj_t* dot = box(cc, 40, 40);
        lv_obj_set_pos(dot, 22 + i * dotSpacing, 38);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, colorFrom565(ACCENT_PALETTE[i]), 0);
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);
        pressable(dot);
        lv_obj_add_event_cb(dot, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            SettingsStorage::get().accentColor = ACCENT_PALETTE[idx];
            SettingsStorage::save();
            refreshSettings(true);
            haptic(HAPTIC_TICK);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        sColorDots[i] = dot;
    }
    sSetRows[SR_AOD]   = toggleRow(list, -1, ICO_WATCH, "Always-On-Display", false, onSetToggle, (void*)(intptr_t)SR_AOD);
    sSetRows[SR_WRIST] = toggleRow(list, -1, ICO_VISIBILITY, "Handgelenk-Wecken", false, onSetToggle, (void*)(intptr_t)SR_WRIST);
    sSetRows[SR_TIMEOUT] = valueRow(list, -1, ICO_TIMER, "Bildschirm-Timeout", "--", [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        int n = sizeof(TIMEOUT_STEPS) / sizeof(TIMEOUT_STEPS[0]);
        int next = 0;
        for (int i = 0; i < n; i++) if (s.autoSleepSec == TIMEOUT_STEPS[i]) { next = (i + 1) % n; break; }
        s.autoSleepSec = TIMEOUT_STEPS[next];
        SettingsStorage::save();
        nav::markDirty(nav::PG_CONTROL);
        haptic(HAPTIC_TICK);
        refreshSettings(true);
    });

    // ---- VERBINDUNGEN ----
    sectionHeader(list, -1, "VERBINDUNGEN");
    sSetRows[SR_WIFI]   = toggleRow(list, -1, ICO_WIFI, "WLAN-Hotspot", false, onSetToggle, (void*)(intptr_t)SR_WIFI);
    sSetRows[SR_BLE]    = toggleRow(list, -1, ICO_SMARTPHONE, "Bluetooth", false, onSetToggle, (void*)(intptr_t)SR_BLE);
    sSetRows[SR_GPS]    = toggleRow(list, -1, ICO_SATELLITE_ALT, "GPS", false, onSetToggle, (void*)(intptr_t)SR_GPS);
    sSetRows[SR_LORA]   = toggleRow(list, -1, ICO_PODCASTS, "LoRa Mesh", false, onSetToggle, (void*)(intptr_t)SR_LORA);
    sSetRows[SR_BEACON] = toggleRow(list, -1, ICO_CELL_TOWER, "LoRa Positions-Beacon", false, onSetToggle, (void*)(intptr_t)SR_BEACON);
    lv_obj_t* cn = valueRow(list, -1, ICO_QR_CODE_2, "Verbinden (QR, PIN)", "", [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        nav::openApp(nav::PG_APP_CONNECT);
    });
    (void)cn;

    // ---- ZEIT ----
    sectionHeader(list, -1, "ZEIT");
    sSetRows[SR_TZ]   = valueRow(list, -1, ICO_SCHEDULE, "Zeitzone", "--", nullptr);
    sSetRows[SR_SYNC] = valueRow(list, -1, ICO_SYNC, "Letzte Zeitsync", "--", nullptr);

    // ---- SYSTEM ----
    sectionHeader(list, -1, "SYSTEM");
    sSetRows[SR_SOUND]  = toggleRow(list, -1, ICO_VOLUME_UP, "Töne", false, onSetToggle, (void*)(intptr_t)SR_SOUND);
    sSetRows[SR_HAPTIC] = toggleRow(list, -1, ICO_VIBRATION, "Haptik", false, onSetToggle, (void*)(intptr_t)SR_HAPTIC);
    sSetRows[SR_GOAL] = valueRow(list, -1, ICO_FOOTPRINT, "Tägliches Schrittziel", "--", [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        int n = sizeof(GOAL_STEPS) / sizeof(GOAL_STEPS[0]);
        int next = 2;
        for (int i = 0; i < n; i++) if (s.dailyStepGoal == GOAL_STEPS[i]) { next = (i + 1) % n; break; }
        s.dailyStepGoal = GOAL_STEPS[next];
        SettingsStorage::save();
        nav::markDirty(nav::PG_ACTIVITY);
        nav::markDirty(nav::PG_FACE);
        haptic(HAPTIC_TICK);
        refreshSettings(true);
    });
    char ver[40];
    snprintf(ver, sizeof(ver), "%s (%s)", OS_VERSION, OS_BUILD);
    sSetRows[SR_ABOUT] = valueRow(list, -1, ICO_INFO, "Über WatchOS Ultra", ver, [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        char msg[120];
        snprintf(msg, sizeof(msg), "%s\nVersion %s, Build %s\nLilyGO T-Watch Ultra (ESP32-S3)", OS_NAME, OS_VERSION, OS_BUILD);
        confirmDialog("Über", msg, "OK", "Schliessen", nullptr, nullptr, false);
    });
    lv_obj_t* pw = valueRow(list, -1, ICO_POWER_SETTINGS_NEW, "Energie / Neustart", "", [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        nav::openApp(nav::PG_APP_POWER);
    });
    (void)pw;
    lv_obj_t* fr = valueRow(list, -1, ICO_DELETE, "Werkseinstellungen", "", [](lv_event_t*) {
        haptic(HAPTIC_TICK);
        confirmDialog("Werkseinstellungen?", "Alle Einstellungen werden zurückgesetzt und die Uhr startet neu.",
                      "Zurücksetzen", "Abbrechen", onFactoryConfirm, nullptr, true);
    });
    lv_obj_t* frIc = lv_obj_get_child(fr, 0);
    if (frIc) lv_obj_set_style_text_color(frIc, cCrit(), 0);

    refreshSettings(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshSettings;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_APP_SETTINGS, d);
}

// ============================================================== MEDIA REMOTE
static lv_obj_t *sAppMediaTitle, *sAppMediaArtist, *sAppMediaPlay, *sAppMediaPill;
static lv_obj_t *sAppMediaVolBar, *sAppMediaVolLbl;
static lv_obj_t *sAppMediaBtns[5];

static void refreshAppMedia(bool) {
    MediaState ms = BleCompanion::getMediaState();
    bool bleOn = BleCompanion::isEnabled();
    bool bleOk = BleCompanion::isConnected();
    bool hasTrack = bleOk && ms.title[0];

    setText(sAppMediaTitle, hasTrack ? ms.title : "Keine Wiedergabe");
    if (!bleOk) setText(sAppMediaArtist, bleOn ? "Handy per Bluetooth verbinden" : "Bluetooth ist aus");
    else if (ms.artist[0]) setText(sAppMediaArtist, ms.artist);
    else setText(sAppMediaArtist, ms.isPlaying ? "Wiedergabe" : "Pausiert");

    lv_obj_t* playIc = lv_obj_get_child(sAppMediaPlay, 0);
    if (playIc) setText(playIc, (bleOk && ms.isPlaying) ? ICO_PAUSE : ICO_PLAY_ARROW);

    if (bleOk) setPill(sAppMediaPill, "VERBUNDEN", cOk());
    else if (bleOn) setPill(sAppMediaPill, "NICHT VERBUNDEN", cWarn());
    else setPill(sAppMediaPill, "BLUETOOTH AUS", cCrit());

    setBar(sAppMediaVolBar, bleOk ? ms.volumePct : 0);
    char vb[16];
    if (bleOk) snprintf(vb, sizeof(vb), "%u%%", (unsigned)ms.volumePct);
    else snprintf(vb, sizeof(vb), "--");
    setText(sAppMediaVolLbl, vb);
    for (int i = 0; i < 5; i++) setButtonEnabled(sAppMediaBtns[i], bleOk);
}

void buildAppMedia(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "MEDIEN");
    sAppMediaPill = pill(p, "BLUETOOTH AUS", cCrit());
    lv_obj_align(sAppMediaPill, LV_ALIGN_TOP_RIGHT, -M, 24);

    lv_obj_t* cardMeta = card(p, CW, 160);
    lv_obj_set_pos(cardMeta, M, CONTENT_Y);
    lv_obj_t* artBox = box(cardMeta, 64, 64);
    lv_obj_set_pos(artBox, 18, 18);
    lv_obj_set_style_radius(artBox, 18, 0);
    lv_obj_set_style_bg_opa(artBox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(artBox, cSurf2(), 0);
    lv_obj_t* ico = label(artBox, F.icoM, accent(), ICO_SMARTPHONE);
    accReg(ico, ACC_TEXT);
    lv_obj_center(ico);

    sAppMediaTitle = label(cardMeta, F.body, cText(), "Keine Wiedergabe");
    lv_obj_set_pos(sAppMediaTitle, 96, 20);
    lv_label_set_long_mode(sAppMediaTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sAppMediaTitle, CW - 116);
    sAppMediaArtist = caption(cardMeta, "Bluetooth ist aus", cText2());
    lv_obj_set_pos(sAppMediaArtist, 96, 52);
    lv_obj_set_width(sAppMediaArtist, CW - 116);
    lv_label_set_long_mode(sAppMediaArtist, LV_LABEL_LONG_DOT);

    lv_obj_t* volCap = caption(cardMeta, "LAUTSTÄRKE", cText3());
    lv_obj_set_pos(volCap, 18, 100);
    sAppMediaVolBar = lv_bar_create(cardMeta);
    lv_obj_set_size(sAppMediaVolBar, CW - 100, 8);
    lv_obj_set_pos(sAppMediaVolBar, 18, 126);
    lv_obj_set_style_radius(sAppMediaVolBar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(sAppMediaVolBar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sAppMediaVolBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sAppMediaVolBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sAppMediaVolBar, accent(), LV_PART_INDICATOR);
    accReg(sAppMediaVolBar, ACC_BG, LV_PART_INDICATOR);
    lv_bar_set_range(sAppMediaVolBar, 0, 100);
    sAppMediaVolLbl = caption(cardMeta, "--", cText2());
    lv_obj_align(sAppMediaVolLbl, LV_ALIGN_TOP_RIGHT, -18, 120);

    // transport: prev / play / next
    const int ty = CONTENT_Y + 184;
    sAppMediaBtns[0] = iconButton(p, 72, ICO_CHEVRON_LEFT, cSurf2(), cText());
    lv_obj_set_pos(sAppMediaBtns[0], M + 24, ty + 12);
    lv_obj_add_event_cb(sAppMediaBtns[0], [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:PREV");
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    sAppMediaPlay = iconButton(p, 96, ICO_PLAY_ARROW, accent(), cBg());
    accReg(sAppMediaPlay, ACC_BG);
    lv_obj_set_pos(sAppMediaPlay, (SCR_W - 96) / 2, ty);
    lv_obj_add_event_cb(sAppMediaPlay, [](lv_event_t*) {
        MediaState ms = BleCompanion::getMediaState();
        BleCompanion::sendMediaCommand("CMD:MEDIA:PLAY_PAUSE");
        BleCompanion::setMediaPlaying(!ms.isPlaying);      // optimistic local state
        haptic(HAPTIC_CLICK);
        refreshAppMedia(true);
        nav::markDirty(nav::PG_CONTROL);
    }, LV_EVENT_CLICKED, NULL);
    sAppMediaBtns[1] = sAppMediaPlay;

    sAppMediaBtns[2] = iconButton(p, 72, ICO_SKIP_NEXT, cSurf2(), cText());
    lv_obj_set_pos(sAppMediaBtns[2], SCR_W - M - 24 - 72, ty + 12);
    lv_obj_add_event_cb(sAppMediaBtns[2], [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:NEXT");
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    // volume
    const int vy = ty + 120;
    const int vw = (CW - 14) / 2;
    sAppMediaBtns[3] = secondaryButton(p, vw, 52, ICO_VOLUME_OFF, "Leiser");
    lv_obj_set_pos(sAppMediaBtns[3], M, vy);
    lv_obj_add_event_cb(sAppMediaBtns[3], [](lv_event_t*) {
        MediaState ms = BleCompanion::getMediaState();
        BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_DOWN");
        BleCompanion::setMediaVolume(ms.volumePct > 5 ? ms.volumePct - 5 : 0);
        haptic(HAPTIC_TICK);
        refreshAppMedia(true);
    }, LV_EVENT_CLICKED, NULL);
    sAppMediaBtns[4] = secondaryButton(p, vw, 52, ICO_VOLUME_UP, "Lauter");
    lv_obj_set_pos(sAppMediaBtns[4], M + vw + 14, vy);
    lv_obj_add_event_cb(sAppMediaBtns[4], [](lv_event_t*) {
        MediaState ms = BleCompanion::getMediaState();
        BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_UP");
        BleCompanion::setMediaVolume(ms.volumePct < 95 ? ms.volumePct + 5 : 100);
        haptic(HAPTIC_TICK);
        refreshAppMedia(true);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* hint = caption(p, "FERNBEDIENUNG FÜR DAS HANDY (BLUETOOTH)", cText3());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    refreshAppMedia(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshAppMedia;
    d.refreshMs = 500;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_APP_MEDIA, d);
}

// ======================================================= GLAS-KALIBRIERUNG ===
static lv_obj_t *sCalibCoordLbl = nullptr, *sCalibDistLbl = nullptr;

static void onCalibTouch(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    int x = pt.x, y = pt.y;
    int dL = x;
    int dR = SCR_W - 1 - x;
    int dT = y;
    int dB = SCR_H - 1 - y;

    char cBuf[32], dBuf[48];
    snprintf(cBuf, sizeof(cBuf), "X: %3d   Y: %3d", x, y);
    snprintf(dBuf, sizeof(dBuf), "L:%d  R:%d  T:%d  B:%d", dL, dR, dT, dB);

    if (sCalibCoordLbl) lv_label_set_text(sCalibCoordLbl, cBuf);
    if (sCalibDistLbl) lv_label_set_text(sCalibDistLbl, dBuf);
}

void buildCalib(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);

    // 1. Concentric rectangular borders at specific pixel offsets
    static const struct { int d; uint32_t color; } RECTS[] = {
        { 0, 0xFF0000},  // 0 px:  Red
        { 4, 0xFF7F00},  // 4 px:  Orange
        { 8, 0xFFFF00},  // 8 px:  Yellow
        {12, 0x00FF00},  // 12 px: Green
        {16, 0x00FFFF},  // 16 px: Cyan
        {20, 0x007FFF},  // 20 px: Blue
        {28, 0xBF5AF2},  // 28 px: Violet
        {36, 0x8E8E93},  // 36 px: Gray
    };
    for (size_t i = 0; i < sizeof(RECTS)/sizeof(RECTS[0]); i++) {
        int d = RECTS[i].d;
        lv_obj_t* r = lv_obj_create(p);
        lv_obj_set_pos(r, d, d);
        lv_obj_set_size(r, SCR_W - 2 * d, SCR_H - 2 * d);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(RECTS[i].color), 0);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_remove_flag(r, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }

    // 2. Comprehensive Spectrum of Corner Radii (reaching all the way to center/inner display)
    static const struct { int r; uint32_t color; } RADII[] = {
        { 20, 0xFF1744},  // Crimson
        { 30, 0xFF5252},  // Coral
        { 40, 0xFF6D00},  // Deep Orange
        { 50, 0xFF9100},  // Orange
        { 60, 0xFFD600},  // Amber
        { 70, 0xFFFF00},  // Yellow
        { 80, 0xAEEA00},  // Lime Yellow
        { 90, 0x76FF03},  // Bright Lime
        {100, 0x00E676},  // Spring Green
        {110, 0x1DE9B6},  // Teal Mint
        {120, 0x00E5FF},  // Cyan
        {130, 0x00B0FF},  // Sky Blue
        {140, 0x2979FF},  // Electric Blue
        {150, 0x651FFF},  // Deep Indigo
        {160, 0x7C4DFF},  // Purple
        {175, 0xD500F9},  // Neon Violet
        {190, 0xF50057},  // Hot Pink
        {205, 0xFFFFFF},  // White (Half-Width Dome)
    };

    // Draw full-dimension outer rounded boxes with increasing corner radius
    for (size_t i = 0; i < sizeof(RADII)/sizeof(RADII[0]); i++) {
        lv_obj_t* c = lv_obj_create(p);
        lv_obj_set_pos(c, 0, 0);
        lv_obj_set_size(c, SCR_W, SCR_H);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(RADII[i].color), 0);
        lv_obj_set_style_radius(c, RADII[i].r, 0);
        lv_obj_remove_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }

    // 3. Inner Inset Curves: concentric rounded rectangles at 10px and 20px insets
    static const int INSET_RADII[] = {30, 50, 70, 90, 110, 130, 150, 170};
    for (int r : INSET_RADII) {
        // 12px inset
        lv_obj_t* in1 = lv_obj_create(p);
        lv_obj_set_pos(in1, 12, 12);
        lv_obj_set_size(in1, SCR_W - 24, SCR_H - 24);
        lv_obj_set_style_bg_opa(in1, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(in1, 1, 0);
        lv_obj_set_style_border_color(in1, lv_color_hex(0x224455), 0);
        lv_obj_set_style_radius(in1, r, 0);
        lv_obj_remove_flag(in1, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }

    // 4. Pure Circular Corner Arcs (Quadrant Circles centered at corners: 40px to 180px)
    static const int CIRC_RADII[] = {40, 60, 80, 100, 120, 140, 160, 180};
    for (int cr : CIRC_RADII) {
        // Top-Left circular arc
        lv_obj_t* caTL = lv_obj_create(p);
        lv_obj_set_pos(caTL, -cr, -cr);
        lv_obj_set_size(caTL, 2 * cr, 2 * cr);
        lv_obj_set_style_bg_opa(caTL, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(caTL, 1, 0);
        lv_obj_set_style_border_color(caTL, lv_color_hex(0x334466), 0);
        lv_obj_set_style_radius(caTL, LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(caTL, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        // Top-Right circular arc
        lv_obj_t* caTR = lv_obj_create(p);
        lv_obj_set_pos(caTR, SCR_W - cr, -cr);
        lv_obj_set_size(caTR, 2 * cr, 2 * cr);
        lv_obj_set_style_bg_opa(caTR, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(caTR, 1, 0);
        lv_obj_set_style_border_color(caTR, lv_color_hex(0x334466), 0);
        lv_obj_set_style_radius(caTR, LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(caTR, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }

    // 5. Corner radius labels along the diagonal (Top-Left)
    for (size_t i = 0; i < sizeof(RADII)/sizeof(RADII[0]); i++) {
        int r = RADII[i].r;
        // On diagonal: x = y = r * (1 - cos(45°)) = r * 0.2929
        int dxy = (int)(r * 0.2929f);
        char rb[8];
        snprintf(rb, sizeof(rb), "%d", r);
        lv_obj_t* rl = lv_label_create(p);
        lv_label_set_text(rl, rb);
        lv_obj_set_style_text_font(rl, F.cap, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(RADII[i].color), 0);
        lv_obj_set_pos(rl, dxy + 2, dxy + 2);

        // Also place a tick/label along the top edge where the curve begins
        if (r <= 190 && (i % 2 == 0)) {
            lv_obj_t* topLbl = lv_label_create(p);
            lv_label_set_text(topLbl, rb);
            lv_obj_set_style_text_font(topLbl, F.cap, 0);
            lv_obj_set_style_text_color(topLbl, lv_color_hex(RADII[i].color), 0);
            lv_obj_set_pos(topLbl, r - 8, 2);
        }
    }

    // 6. Compact Center Caliper Card (leaves entire corners and perimeter fully visible)
    lv_obj_t* cd = card(p, 270, 150);
    lv_obj_align(cd, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(cd, lv_color_hex(0x101622), 0);
    lv_obj_set_style_bg_opa(cd, LV_OPA_90, 0);
    lv_obj_set_style_border_width(cd, 1, 0);
    lv_obj_set_style_border_color(cd, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_radius(cd, 16, 0);

    lv_obj_t* titleLbl = label(cd, F.body, lv_color_hex(0x00FFFF), "GLAS-KALIBRIERUNG");
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 6);

    sCalibCoordLbl = label(cd, F.numM, lv_color_white(), "X: ---   Y: ---");
    lv_obj_align(sCalibCoordLbl, LV_ALIGN_TOP_MID, 0, 32);

    sCalibDistLbl = label(cd, F.small, lv_color_hex(0x30D158), "L:--  R:--  T:--  B:--");
    lv_obj_align(sCalibDistLbl, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_t* leg = caption(cd, "Kurven R20..R205  |  Rand 0..36px", cText3());
    lv_obj_align(leg, LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t* backBtn = secondaryButton(cd, 120, 28, ICO_ARROW_BACK, "Schließen");
    lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(backBtn, [](lv_event_t*) {
        nav::home();
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);

    // Track touch events on page
    lv_obj_add_event_cb(p, onCalibTouch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(p, onCalibTouch, LV_EVENT_PRESSING, NULL);

    nav::PageDef d;
    d.root = p;
    d.refresh = nullptr;
    d.refreshMs = 0;
    d.ttlMs = 0;
    nav::registerPage(nav::PG_APP_CALIB, d);
}

}  // namespace pages
