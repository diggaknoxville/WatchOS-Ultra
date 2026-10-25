// Control centre sheet (swipe down from ANY screen):
// 2 spacious pages: Page 0 = Schnellschalter (6 large toggle cards + conditional media player)
//                   Page 1 = System & Display (Brightness slider with +/- 5%, Timeout pills, Haptics, Wrist Wake, Power)
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "AudioEngine.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include "RtcManager.h"

using namespace ui;

namespace pages {

#define N_TOGGLES 6
struct Toggle { lv_obj_t *tile, *icon, *label; };
static Toggle sT[N_TOGGLES];
static lv_obj_t *sClock = nullptr;
static lv_obj_t *sTabBtn = nullptr;
static lv_obj_t *sTabLbl = nullptr;

static lv_obj_t *sWrapToggles = nullptr;
static lv_obj_t *sWrapSystem = nullptr;
static int sActiveTab = 0; // 0 = Schalter, 1 = System

// System page widgets
static lv_obj_t *sSlider = nullptr;
static lv_obj_t *sPct = nullptr;
static lv_obj_t *sWristTile = nullptr, *sWristIcon = nullptr, *sWristLbl = nullptr;
static lv_obj_t *sHapticTile = nullptr, *sHapticIcon = nullptr, *sHapticLbl = nullptr;
static lv_obj_t *sTimeoutPills[5] = {nullptr};

// Conditional Media card on Page 0
static lv_obj_t *sMediaCard = nullptr;
static lv_obj_t *sMediaTitle = nullptr;
static lv_obj_t *sMediaArtist = nullptr;
static lv_obj_t *sMediaPlayBtn = nullptr;
static lv_obj_t *sMediaPlayIcon = nullptr;
static lv_obj_t *sMediaBleDot = nullptr;
static lv_obj_t *sQuickInfoCard = nullptr;
static lv_obj_t *sQuickInfoLbl = nullptr;

static const uint16_t TIMEOUT_PRESETS[] = {15, 30, 60, 120, 0};
static const char* TIMEOUT_LABELS[] = {"15s", "30s", "60s", "2m", "Nie"};
#define N_TIMEOUT_PRESETS 5

static bool state(int i) {
    SystemSettings& s = SettingsStorage::get();
    switch (i) {
        case 0: return CompanionServer::isApActive();
        case 1: return BleCompanion::isEnabled();
        case 2: return GpsManager::isEnabled();
        case 3: return RadioLoRa::isEnabled();
        case 4: return s.aodEnabled;
        default: return DisplayEngine::torchOn();
    }
}

static void paint(int i) {
    if (i < 0 || i >= N_TOGGLES || !sT[i].tile) return;
    bool on = state(i);
    setBgColorIf(sT[i].tile, on ? accent() : cSurf());
    setBorderWidthIf(sT[i].tile, on ? 0 : 1);
    setTextColorIf(sT[i].icon, on ? cBg() : cText());
    setTextColorIf(sT[i].label, on ? cBg() : cText2());
}

static void onToggle(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    SystemSettings& s = SettingsStorage::get();
    switch (i) {
        case 0:
            s.wifiApEnabled = !s.wifiApEnabled;
            CompanionServer::requestAp(s.wifiApEnabled);
            nav::markDirty(nav::PG_APP_CONNECT);
            DisplayEngine::showNotification(s.wifiApEnabled ? "WiFi AP an" : "WiFi AP aus",
                                            s.wifiApEnabled ? "192.168.4.1 (vortex2026)" : "WLAN abgeschaltet (Stromsparen)",
                                            2200, ICO_WIFI);
            break;
        case 1: {
            bool en = !BleCompanion::isEnabled();
            BleCompanion::setEnabled(en);
            nav::markDirty(nav::PG_APP_CONNECT);
            DisplayEngine::showNotification(en ? "Bluetooth an" : "Bluetooth aus",
                                            en ? "BLE Companion aktiv" : "BLE abgeschaltet (Stromsparen)",
                                            2200, ICO_SMARTPHONE);
            break;
        }
        case 2: {
            bool en = !GpsManager::isEnabled();
            GpsManager::setEnabled(en);
            nav::markDirty(nav::PG_POSITION);
            DisplayEngine::showNotification(en ? "GPS an" : "GPS aus",
                                            en ? "Satellitensuche aktiv" : "GNSS abgeschaltet (Stromsparen)",
                                            2200, ICO_SATELLITE_ALT);
            break;
        }
        case 3: {
            bool en = !RadioLoRa::isEnabled();
            RadioLoRa::setEnabled(en);
            nav::markDirty(nav::PG_LORA);
            DisplayEngine::showNotification(en ? "LoRa an" : "LoRa aus",
                                            en ? "SX1262 Mesh aktiv" : "LoRa Funk abgeschaltet",
                                            2200, ICO_PODCASTS);
            break;
        }
        case 4:
            s.aodEnabled = !s.aodEnabled;
            DisplayEngine::showNotification(s.aodEnabled ? "Always-On an" : "Always-On aus",
                                            s.aodEnabled ? "AOD spart Strom" : "Display schaltet ganz ab",
                                            2000, ICO_WATCH);
            break;
        case 5:
            DisplayEngine::setTorch(!DisplayEngine::torchOn());
            break;
    }
    SettingsStorage::save();
    paint(i);
    haptic(HAPTIC_CLICK);
}

static void updateTimeoutPills() {
    uint16_t cur = SettingsStorage::get().autoSleepSec;
    for (int i = 0; i < N_TIMEOUT_PRESETS; i++) {
        if (!sTimeoutPills[i]) continue;
        bool sel = (TIMEOUT_PRESETS[i] == cur);
        setBgColorIf(sTimeoutPills[i], sel ? accent() : cSurf2());
        lv_obj_t* lbl = lv_obj_get_child(sTimeoutPills[i], 0);
        if (lbl) setTextColorIf(lbl, sel ? cBg() : cText());
    }
}

static void refresh(bool) {
    for (int i = 0; i < N_TOGGLES; i++) paint(i);

    struct tm t;
    RtcManager::getLocalTimeInfo(&t);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    setText(sClock, b);

    // Brightness
    if (sSlider && sPct) {
        int v = SettingsStorage::get().brightness;
        if (lv_slider_get_value(sSlider) != v && !lv_obj_has_state(sSlider, LV_STATE_PRESSED)) {
            lv_slider_set_value(sSlider, v, LV_ANIM_OFF);
            lv_label_set_text_fmt(sPct, "%d%%", v * 100 / 255);
        }
    }

    // System Page Toggles
    SystemSettings& s = SettingsStorage::get();
    if (sWristTile) {
        setBgColorIf(sWristTile, s.wristWakeEnabled ? accent() : cSurf());
        setTextColorIf(sWristIcon, s.wristWakeEnabled ? cBg() : cText());
        setTextColorIf(sWristLbl, s.wristWakeEnabled ? cBg() : cText2());
    }
    if (sHapticTile) {
        setBgColorIf(sHapticTile, s.hapticEnabled ? accent() : cSurf());
        setTextColorIf(sHapticIcon, s.hapticEnabled ? cBg() : cText());
        setTextColorIf(sHapticLbl, s.hapticEnabled ? cBg() : cText2());
    }
    updateTimeoutPills();

    // Media Controller on Page 0: only show when playing/active!
    const MediaState& ms = BleCompanion::getMediaState();
    bool isConnected = BleCompanion::isConnected();
    bool showMedia = isConnected && ms.isPlaying;

    if (sMediaCard && sQuickInfoCard) {
        if (showMedia) {
            lv_obj_remove_flag(sMediaCard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(sQuickInfoCard, LV_OBJ_FLAG_HIDDEN);
            if (sMediaTitle) setText(sMediaTitle, ms.title);
            if (sMediaArtist) setText(sMediaArtist, ms.artist);
            if (sMediaPlayIcon) setText(sMediaPlayIcon, ms.isPlaying ? ICO_PAUSE : ICO_PLAY_ARROW);
            if (sMediaBleDot) setBgColorIf(sMediaBleDot, isConnected ? cOk() : cCrit());
        } else {
            lv_obj_add_flag(sMediaCard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(sQuickInfoCard, LV_OBJ_FLAG_HIDDEN);
            if (sQuickInfoLbl) {
                char ib[64];
                snprintf(ib, sizeof(ib), "Batterie: %d%% (%.2fV)  ·  %s",
                         PowerManager::getBatteryPercent(),
                         PowerManager::getBatteryVoltage(),
                         isConnected ? "BLE Verbunden" : "Bereit");
                setText(sQuickInfoLbl, ib);
            }
        }
    }
}

static bool allowGesture(int, int y0, int) {
    // Only allow vertical pull close from safe non-slider zones
    if (sActiveTab == 1 && y0 >= 60 && y0 <= 150) return false;
    return true;
}

void buildControl(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "CONTROL");

    // Tab Switch Pill (SCHALTER <-> SYSTEM)
    sTabBtn = box(p, 142, 30);
    lv_obj_align(sTabBtn, LV_ALIGN_TOP_RIGHT, -(M + 80), 24);
    lv_obj_set_style_radius(sTabBtn, 15, 0);
    lv_obj_set_style_bg_opa(sTabBtn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(sTabBtn, accent(), 0);
    lv_obj_add_flag(sTabBtn, LV_OBJ_FLAG_CLICKABLE);
    sTabLbl = caption(sTabBtn, "SCHALTER >", accent());
    accReg(sTabLbl, ACC_TEXT);
    lv_obj_center(sTabLbl);

    sClock = label(p, F.title, cText2(), "");
    lv_obj_align(sClock, LV_ALIGN_TOP_RIGHT, -M, 18);

    // =========================================================================
    // Page 0: SCHNELLSCHALTER
    // =========================================================================
    sWrapToggles = box(p, SCR_W, SCR_H - 68);
    lv_obj_set_pos(sWrapToggles, 0, 68);

    static const char* icons[N_TOGGLES] = {
        ICO_WIFI, ICO_SMARTPHONE, ICO_SATELLITE_ALT, ICO_PODCASTS, ICO_WATCH, ICO_FLASHLIGHT_ON
    };
    static const char* names[N_TOGGLES] = {
        "WiFi AP", "Bluetooth", "GPS GNSS", "LoRa Mesh", "Always-On", "Taschenlampe"
    };

    const int w = (CW - 10) / 2, h = 58, y0 = 6;
    for (int i = 0; i < N_TOGGLES; i++) {
        lv_obj_t* c = card(sWrapToggles, w, h);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_pos(c, M + (i % 2) * (w + 10), y0 + (i / 2) * (h + 10));
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, onToggle, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        sT[i].tile = c;
        sT[i].icon = label(c, F.icoS, cText(), icons[i]);
        lv_obj_align(sT[i].icon, LV_ALIGN_LEFT_MID, 16, 0);
        sT[i].label = label(c, F.small, cText2(), names[i]);
        lv_obj_align(sT[i].label, LV_ALIGN_LEFT_MID, 50, 0);
        paint(i);
    }

    // Conditional Media Card (y = 224, h = 84)
    sMediaCard = card(sWrapToggles, CW, 84);
    lv_obj_set_style_radius(sMediaCard, 20, 0);
    lv_obj_set_pos(sMediaCard, M, 222);

    lv_obj_t* mIco = label(sMediaCard, F.icoS, cCrit(), ICO_PLAY_ARROW);
    lv_obj_set_pos(mIco, 12, 10);

    sMediaTitle = label(sMediaCard, F.small, cText(), "YouTube Music");
    lv_obj_set_pos(sMediaTitle, 38, 8);
    lv_label_set_long_mode(sMediaTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sMediaTitle, 240);

    sMediaArtist = caption(sMediaCard, "Aktiv", cText2());
    lv_obj_set_pos(sMediaArtist, 38, 28);
    lv_obj_set_width(sMediaArtist, 240);

    sMediaBleDot = box(sMediaCard, 10, 10);
    lv_obj_set_style_radius(sMediaBleDot, 5, 0);
    lv_obj_set_style_bg_color(sMediaBleDot, cOk(), 0);
    lv_obj_set_style_bg_opa(sMediaBleDot, LV_OPA_COVER, 0);
    lv_obj_align(sMediaBleDot, LV_ALIGN_TOP_RIGHT, -12, 12);

    auto makeMediaBtn = [&](int x, int bw, const char* icon, bool isIco, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* b = box(sMediaCard, bw, 34);
        lv_obj_set_pos(b, x, 44);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, cSurf2(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t* ic = label(b, isIco ? F.icoS : F.small, cText(), icon);
        lv_obj_center(ic);
        return b;
    };

    makeMediaBtn(10, 56, ICO_CHEVRON_LEFT, true, [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:PREV");
        haptic(HAPTIC_TICK);
    });

    sMediaPlayBtn = makeMediaBtn(72, 78, ICO_PLAY_ARROW, true, [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:PLAY_PAUSE");
        const MediaState& ms = BleCompanion::getMediaState();
        BleCompanion::setMediaState(!ms.isPlaying, ms.title, ms.artist, ms.volumePct);
        haptic(HAPTIC_CLICK);
        refresh(true);
    });
    sMediaPlayIcon = lv_obj_get_child(sMediaPlayBtn, 0);

    makeMediaBtn(156, 56, ICO_SKIP_NEXT, true, [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:NEXT");
        haptic(HAPTIC_TICK);
    });

    makeMediaBtn(222, 68, "Vol -", false, [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_DOWN");
        haptic(HAPTIC_TICK);
    });

    makeMediaBtn(296, 68, "Vol +", false, [](lv_event_t*) {
        BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_UP");
        haptic(HAPTIC_TICK);
    });

    // Clean Quick Status Card when media is inactive
    sQuickInfoCard = card(sWrapToggles, CW, 84);
    lv_obj_set_style_radius(sQuickInfoCard, 20, 0);
    lv_obj_set_pos(sQuickInfoCard, M, 222);
    lv_obj_add_flag(sQuickInfoCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sQuickInfoCard, [](lv_event_t*) {
        nav::openApp(nav::PG_APP_MEDIA);
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* qiIco = label(sQuickInfoCard, F.icoS, accent(), ICO_INFO);
    lv_obj_set_pos(qiIco, 16, 16);
    lv_obj_t* qiTitle = label(sQuickInfoCard, F.small, cText(), "Systemstatus · Tippen für Media App");
    lv_obj_set_pos(qiTitle, 46, 14);
    sQuickInfoLbl = caption(sQuickInfoCard, "Batterie 100% · BLE Bereit", cText2());
    lv_obj_set_pos(sQuickInfoLbl, 46, 42);

    // Drawer / Apps button at bottom of Page 0 (y = 320)
    lv_obj_t* appBtn = card(sWrapToggles, CW, 54);
    lv_obj_set_style_radius(appBtn, 18, 0);
    lv_obj_set_pos(appBtn, M, 320);
    lv_obj_add_flag(appBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(appBtn, [](lv_event_t*) {
        nav::go(nav::PG_LAUNCHER);
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* appIco = label(appBtn, F.icoS, accent(), ICO_APPS);
    lv_obj_align(appIco, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t* appTxt = label(appBtn, F.small, cText(), "Alle Apps im Drawer öffnen");
    lv_obj_align(appTxt, LV_ALIGN_LEFT_MID, 54, 0);

    // =========================================================================
    // Page 1: SYSTEM & DISPLAY
    // =========================================================================
    sWrapSystem = box(p, SCR_W, SCR_H - 68);
    lv_obj_set_pos(sWrapSystem, 0, 68);
    lv_obj_add_flag(sWrapSystem, LV_OBJ_FLAG_HIDDEN); // Hidden by default

    // Brightness Card (h = 80)
    lv_obj_t* bCard = card(sWrapSystem, CW, 80);
    lv_obj_set_style_radius(bCard, 20, 0);
    lv_obj_set_pos(bCard, M, 6);

    lv_obj_t* bl = label(bCard, F.icoS, accent(), ICO_LIGHT_MODE);
    lv_obj_set_pos(bl, 14, 12);
    lv_obj_t* bTitle = label(bCard, F.small, cText(), "Display-Helligkeit");
    lv_obj_set_pos(bTitle, 44, 10);
    sPct = label(bCard, F.small, accent(), "85%");
    lv_obj_align(sPct, LV_ALIGN_TOP_RIGHT, -14, 10);

    sSlider = lv_slider_create(bCard);
    lv_obj_set_size(sSlider, CW - 28, 32);
    lv_obj_set_pos(sSlider, 14, 38);
    lv_slider_set_range(sSlider, 10, 255);
    lv_slider_set_value(sSlider, SettingsStorage::get().brightness, LV_ANIM_OFF);
    lv_obj_set_style_radius(sSlider, R_CARD, LV_PART_MAIN);
    lv_obj_set_style_radius(sSlider, R_CARD, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sSlider, cSurf2(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sSlider, accent(), LV_PART_INDICATOR);
    accReg(sSlider, ACC_BG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sSlider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(sSlider, [](lv_event_t* e) {
        int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
        PowerManager::setBrightness(v);
        lv_label_set_text_fmt(sPct, "%d%%", v * 100 / 255);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sSlider, [](lv_event_t*) {
        SettingsStorage::save();
    }, LV_EVENT_RELEASED, NULL);

    // Screen Timeout Card (h = 80)
    lv_obj_t* tCard = card(sWrapSystem, CW, 80);
    lv_obj_set_style_radius(tCard, 20, 0);
    lv_obj_set_pos(tCard, M, 96);

    lv_obj_t* tIcon = label(tCard, F.icoS, accent(), ICO_TIMER);
    lv_obj_set_pos(tIcon, 14, 10);
    lv_obj_t* tTitle = label(tCard, F.small, cText(), "Bildschirm-Timeout");
    lv_obj_set_pos(tTitle, 44, 8);

    const int pw = (CW - 28 - 4 * 6) / 5; // 5 pills
    for (int i = 0; i < N_TIMEOUT_PRESETS; i++) {
        lv_obj_t* pb = box(tCard, pw, 32);
        lv_obj_set_pos(pb, 14 + i * (pw + 6), 38);
        lv_obj_set_style_radius(pb, 10, 0);
        lv_obj_set_style_bg_color(pb, cSurf2(), 0);
        lv_obj_set_style_bg_opa(pb, LV_OPA_COVER, 0);
        lv_obj_add_flag(pb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pb, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            SettingsStorage::get().autoSleepSec = TIMEOUT_PRESETS[idx];
            SettingsStorage::save();
            updateTimeoutPills();
            haptic(HAPTIC_CLICK);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* pl = label(pb, F.small, cText(), TIMEOUT_LABELS[i]);
        lv_obj_center(pl);
        sTimeoutPills[i] = pb;
    }

    // Wrist Wake & Haptics (2 large cards, h = 58)
    sWristTile = card(sWrapSystem, w, 58);
    lv_obj_set_style_radius(sWristTile, 20, 0);
    lv_obj_set_pos(sWristTile, M, 186);
    lv_obj_add_flag(sWristTile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sWristTile, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.wristWakeEnabled = !s.wristWakeEnabled;
        SettingsStorage::save();
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    sWristIcon = label(sWristTile, F.icoS, cText(), ICO_VISIBILITY);
    lv_obj_align(sWristIcon, LV_ALIGN_LEFT_MID, 16, 0);
    sWristLbl = label(sWristTile, F.small, cText2(), "Wrist Wake");
    lv_obj_align(sWristLbl, LV_ALIGN_LEFT_MID, 50, 0);

    sHapticTile = card(sWrapSystem, w, 58);
    lv_obj_set_style_radius(sHapticTile, 20, 0);
    lv_obj_set_pos(sHapticTile, M + w + 10, 186);
    lv_obj_add_flag(sHapticTile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sHapticTile, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.hapticEnabled = !s.hapticEnabled;
        SettingsStorage::save();
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    sHapticIcon = label(sHapticTile, F.icoS, cText(), ICO_VIBRATION);
    lv_obj_align(sHapticIcon, LV_ALIGN_LEFT_MID, 16, 0);
    sHapticLbl = label(sHapticTile, F.small, cText2(), "Haptik");
    lv_obj_align(sHapticLbl, LV_ALIGN_LEFT_MID, 50, 0);

    // Power & Restart Shortcut (y = 254)
    lv_obj_t* pwrBtn = card(sWrapSystem, CW, 54);
    lv_obj_set_style_radius(pwrBtn, 18, 0);
    lv_obj_set_pos(pwrBtn, M, 254);
    lv_obj_add_flag(pwrBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pwrBtn, [](lv_event_t*) {
        nav::openApp(nav::PG_APP_POWER);
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* pwIco = label(pwrBtn, F.icoS, cCrit(), ICO_POWER_SETTINGS_NEW);
    lv_obj_align(pwIco, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t* pwTxt = label(pwrBtn, F.small, cText(), "Ausschalten / Neustart");
    lv_obj_align(pwTxt, LV_ALIGN_LEFT_MID, 54, 0);

    // Tab button event: switch between SCHALTER and SYSTEM
    lv_obj_add_event_cb(sTabBtn, [](lv_event_t*) {
        sActiveTab = (sActiveTab == 0) ? 1 : 0;
        if (sActiveTab == 0) {
            lv_obj_remove_flag(sWrapToggles, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(sWrapSystem, LV_OBJ_FLAG_HIDDEN);
            setText(sTabLbl, "SCHALTER >");
        } else {
            lv_obj_add_flag(sWrapToggles, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(sWrapSystem, LV_OBJ_FLAG_HIDDEN);
            setText(sTabLbl, "< SYSTEM");
        }
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    // Bottom grabber indicator
    lv_obj_t* grab = box(p, 40, 5);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, cText3(), 0);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -8);

    refresh(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 500;
    d.ttlMs = 10000;
    d.allowSheetGesture = allowGesture;
    nav::registerPage(nav::PG_CONTROL, d);
}

}  // namespace pages
