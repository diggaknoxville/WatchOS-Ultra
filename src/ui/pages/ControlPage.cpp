// Control centre sheet (swipe down from the face / any screen top edge):
//   tab 0 SCHALTER: 2x3 toggle grid (WLAN, Bluetooth, GPS, LoRa, Nicht stören,
//                   Taschenlampe) + media remote card (when the phone plays) + apps
//   tab 1 SYSTEM:   brightness slider, screen timeout, always-on, wrist wake, haptics
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include "RtcManager.h"

using namespace ui;

namespace pages {

enum CtlToggle : uint8_t { CT_WIFI = 0, CT_BLE, CT_GPS, CT_LORA, CT_DND, CT_TORCH, CT_N };
static lv_obj_t* sTiles[CT_N];
static lv_obj_t *sClock, *sTabs, *sWrapToggles, *sWrapSystem;
static int sActiveTab = 0;

// system tab
static lv_obj_t *sSlider, *sPct, *sTimeoutRow, *sAodTile, *sWristTile, *sHapticTile;
static const uint16_t TIMEOUT_STEPS[] = {15, 30, 60, 0};

// media / info card
static lv_obj_t *sMediaCard, *sMediaTitle, *sMediaArtist, *sMediaPlay;
static lv_obj_t *sInfoCard, *sInfoLbl;

static const int TILE_W = (CW - 10) / 2;
static const int TILE_H = 64;
static const int GRID_Y = CONTENT_Y + 40;

// ---- tile: icon + label, accent when on ------------------------------------
static lv_obj_t* makeTile(lv_obj_t* parent, int x, int y, int w, int h, const char* icon, const char* text,
                          lv_event_cb_t cb, void* user) {
    lv_obj_t* c = card(parent, w, h);
    lv_obj_set_style_radius(c, R_TILE, 0);
    lv_obj_set_pos(c, x, y);
    pressable(c);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* ic = label(c, F.icoS, cText(), icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t* t = label(c, F.small, cText2(), text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 50, 0);
    return c;
}

static void paintTile(lv_obj_t* tile, bool on) {
    if (!tile) return;
    setBgColorIf(tile, on ? accent() : cSurf());
    setBorderWidthIf(tile, on ? 0 : 1);
    setTextColorIf(lv_obj_get_child(tile, 0), on ? cBg() : cText());
    setTextColorIf(lv_obj_get_child(tile, 1), on ? cBg() : cText2());
}

static bool toggleState(int i) {
    switch (i) {
        case CT_WIFI:  return CompanionServer::isApActive() || SettingsStorage::get().wifiApEnabled;
        case CT_BLE:   return BleCompanion::isEnabled();
        case CT_GPS:   return GpsManager::isEnabled();
        case CT_LORA:  return RadioLoRa::isEnabled();
        case CT_DND:   return DisplayEngine::doNotDisturb();
        default:       return DisplayEngine::torchOn();
    }
}

static void onToggle(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    SystemSettings& s = SettingsStorage::get();
    switch (i) {
        case CT_WIFI:
            s.wifiApEnabled = !s.wifiApEnabled;
            CompanionServer::requestAp(s.wifiApEnabled);
            SettingsStorage::save();
            nav::markDirty(nav::PG_APP_CONNECT);
            if (s.wifiApEnabled) DisplayEngine::showToast("Hotspot", "Zugangsdaten unter Verbinden", 2200, ICO_WIFI);
            break;
        case CT_BLE:
            BleCompanion::setEnabled(!BleCompanion::isEnabled());
            nav::markDirty(nav::PG_APP_CONNECT);
            break;
        case CT_GPS: {
            bool en = !GpsManager::isEnabled();
            GpsManager::setEnabled(en);
            nav::markDirty(nav::PG_POSITION);
            if (en) DisplayEngine::showToast("GPS", "Satellitensuche läuft", 2000, ICO_SATELLITE_ALT);
            break;
        }
        case CT_LORA:
            RadioLoRa::setEnabled(!RadioLoRa::isEnabled());
            nav::markDirty(nav::PG_LORA);
            break;
        case CT_DND:
            DisplayEngine::setDoNotDisturb(!DisplayEngine::doNotDisturb());
            break;
        case CT_TORCH:
            DisplayEngine::setTorch(!DisplayEngine::torchOn());
            break;
    }
    nav::markDirty(nav::PG_APP_SETTINGS);
    paintTile(sTiles[i], toggleState(i));
    haptic(HAPTIC_TICK);
}

static void refresh(bool) {
    for (int i = 0; i < CT_N; i++) paintTile(sTiles[i], toggleState(i));

    struct tm t;
    RtcManager::getLocalTimeInfo(&t);
    char b[72];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    setText(sClock, b);

    setTabActive(sTabs, sActiveTab);
    setHidden(sWrapToggles, sActiveTab != 0);
    setHidden(sWrapSystem, sActiveTab != 1);

    SystemSettings& s = SettingsStorage::get();
    int v = s.brightness;
    if (lv_slider_get_value(sSlider) != v && !lv_obj_has_state(sSlider, LV_STATE_PRESSED)) {
        lv_slider_set_value(sSlider, v, LV_ANIM_OFF);
        snprintf(b, sizeof(b), "%d%%", v * 100 / 255);
        setText(sPct, b);
    }
    if (s.autoSleepSec == 0) snprintf(b, sizeof(b), "Nie");
    else snprintf(b, sizeof(b), "%u s", (unsigned)s.autoSleepSec);
    setValueRow(sTimeoutRow, b);
    paintTile(sAodTile, s.aodEnabled);
    paintTile(sWristTile, s.wristWakeEnabled);
    paintTile(sHapticTile, s.hapticEnabled);

    // media card only when the phone actually reports something
    MediaState ms = BleCompanion::getMediaState();
    bool conn = BleCompanion::isConnected();
    bool showMedia = conn && (ms.isPlaying || ms.title[0]);
    setHidden(sMediaCard, !showMedia);
    setHidden(sInfoCard, showMedia);
    if (showMedia) {
        setText(sMediaTitle, ms.title[0] ? ms.title : "Wiedergabe");
        setText(sMediaArtist, ms.artist[0] ? ms.artist : (ms.isPlaying ? "Läuft" : "Pausiert"));
        setText(lv_obj_get_child(sMediaPlay, 0), ms.isPlaying ? ICO_PAUSE : ICO_PLAY_ARROW);
    } else {
        int bat = PowerManager::getBatteryPercent();
        const char* pwr = PowerManager::isCharging() ? "lädt" : (PowerManager::isVbusConnected() ? "USB" : "Akku");
        snprintf(b, sizeof(b), "Akku %d%% (%s) · %s", bat, pwr, conn ? "Handy verbunden" : "Kein Handy");
        setText(sInfoLbl, b);
    }
}

static bool allowGesture(int, int y0, int) {
    // the brightness slider owns vertical drags on the system tab
    if (sActiveTab == 1 && y0 >= GRID_Y && y0 <= GRID_Y + 84) return false;
    return true;
}

void buildControl(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "STEUERUNG");
    sClock = label(p, F.title, cText2(), "");
    lv_obj_align(sClock, LV_ALIGN_TOP_RIGHT, -M, 18);

    static const char* TABS[2] = {"SCHALTER", "SYSTEM"};
    sTabs = tabBar(p, M, CONTENT_Y, CW, TABS, 2, 0, [](lv_event_t* e) {
        sActiveTab = (int)(intptr_t)lv_event_get_user_data(e);
        haptic(HAPTIC_TICK);
        refresh(true);
    });

    // ===================================================== tab 0: SCHALTER
    sWrapToggles = box(p, SCR_W, SCR_H - GRID_Y);
    lv_obj_set_pos(sWrapToggles, 0, GRID_Y);

    static const char* icons[CT_N] = {ICO_WIFI, ICO_SMARTPHONE, ICO_SATELLITE_ALT, ICO_PODCASTS, ICO_DO_NOT_DISTURB_ON, ICO_FLASHLIGHT_ON};
    static const char* names[CT_N] = {"Hotspot", "Bluetooth", "GPS", "LoRa Mesh", "Nicht stören", "Taschenlampe"};
    for (int i = 0; i < CT_N; i++) {
        sTiles[i] = makeTile(sWrapToggles, M + (i % 2) * (TILE_W + 10), (i / 2) * (TILE_H + 10), TILE_W, TILE_H,
                             icons[i], names[i], onToggle, (void*)(intptr_t)i);
    }
    const int mediaY = 3 * (TILE_H + 10) + 2;          // 224

    // media remote card (conditional)
    sMediaCard = card(sWrapToggles, CW, 84);
    lv_obj_set_style_radius(sMediaCard, R_TILE, 0);
    lv_obj_set_pos(sMediaCard, M, mediaY);
    sMediaTitle = label(sMediaCard, F.small, cText(), "");
    lv_obj_set_pos(sMediaTitle, 16, 8);
    lv_label_set_long_mode(sMediaTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sMediaTitle, CW - 32);
    sMediaArtist = caption(sMediaCard, "", cText2());
    lv_obj_set_pos(sMediaArtist, 16, 30);
    lv_obj_set_width(sMediaArtist, CW - 32);
    lv_label_set_long_mode(sMediaArtist, LV_LABEL_LONG_DOT);
    const int by = 46, bs = 32;
    lv_obj_t* prev = iconButton(sMediaCard, bs, ICO_CHEVRON_LEFT, cSurf2(), cText());
    lv_obj_set_pos(prev, 16, by);
    lv_obj_add_event_cb(prev, [](lv_event_t*) { BleCompanion::sendMediaCommand("CMD:MEDIA:PREV"); haptic(HAPTIC_TICK); }, LV_EVENT_CLICKED, NULL);
    sMediaPlay = iconButton(sMediaCard, bs, ICO_PLAY_ARROW, accent(), cBg());
    accReg(sMediaPlay, ACC_BG);
    lv_obj_set_pos(sMediaPlay, 16 + bs + 10, by);
    lv_obj_add_event_cb(sMediaPlay, [](lv_event_t*) {
        MediaState ms = BleCompanion::getMediaState();
        BleCompanion::sendMediaCommand("CMD:MEDIA:PLAY_PAUSE");
        BleCompanion::setMediaPlaying(!ms.isPlaying);
        haptic(HAPTIC_CLICK);
        nav::markDirty(nav::PG_APP_MEDIA);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* next = iconButton(sMediaCard, bs, ICO_SKIP_NEXT, cSurf2(), cText());
    lv_obj_set_pos(next, 16 + 2 * (bs + 10), by);
    lv_obj_add_event_cb(next, [](lv_event_t*) { BleCompanion::sendMediaCommand("CMD:MEDIA:NEXT"); haptic(HAPTIC_TICK); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* vdn = iconButton(sMediaCard, bs, ICO_VOLUME_OFF, cSurf2(), cText());
    lv_obj_set_pos(vdn, CW - 16 - 2 * bs - 10, by);
    lv_obj_add_event_cb(vdn, [](lv_event_t*) {
        MediaState ms = BleCompanion::getMediaState();
        BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_DOWN");
        BleCompanion::setMediaVolume(ms.volumePct > 5 ? ms.volumePct - 5 : 0);
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* vup = iconButton(sMediaCard, bs, ICO_VOLUME_UP, cSurf2(), cText());
    lv_obj_set_pos(vup, CW - 16 - bs, by);
    lv_obj_add_event_cb(vup, [](lv_event_t*) {
        MediaState ms = BleCompanion::getMediaState();
        BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_UP");
        BleCompanion::setMediaVolume(ms.volumePct < 95 ? ms.volumePct + 5 : 100);
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    // status card when nothing plays (tap -> media remote)
    sInfoCard = card(sWrapToggles, CW, 84);
    lv_obj_set_style_radius(sInfoCard, R_TILE, 0);
    lv_obj_set_pos(sInfoCard, M, mediaY);
    pressable(sInfoCard);
    lv_obj_add_event_cb(sInfoCard, [](lv_event_t*) { haptic(HAPTIC_TICK); nav::openApp(nav::PG_APP_MEDIA); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* qiIco = label(sInfoCard, F.icoS, accent(), ICO_INFO);
    accReg(qiIco, ACC_TEXT);
    lv_obj_set_pos(qiIco, 16, 16);
    lv_obj_t* qiTitle = label(sInfoCard, F.small, cText(), "Status · Tippen für Medien");
    lv_obj_set_pos(qiTitle, 50, 14);
    sInfoLbl = caption(sInfoCard, "", cText2());
    lv_obj_set_pos(sInfoLbl, 50, 44);

    lv_obj_t* appBtn = secondaryButton(sWrapToggles, CW, 48, ICO_APPS, "Alle Apps");
    lv_obj_set_pos(appBtn, M, mediaY + 92);
    lv_obj_add_event_cb(appBtn, [](lv_event_t*) { haptic(HAPTIC_TICK); nav::go(nav::PG_LAUNCHER); }, LV_EVENT_CLICKED, NULL);

    // ===================================================== tab 1: SYSTEM
    sWrapSystem = box(p, SCR_W, SCR_H - GRID_Y);
    lv_obj_set_pos(sWrapSystem, 0, GRID_Y);
    lv_obj_add_flag(sWrapSystem, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* bCard = card(sWrapSystem, CW, 80);
    lv_obj_set_style_radius(bCard, R_TILE, 0);
    lv_obj_set_pos(bCard, M, 0);
    lv_obj_t* bl = label(bCard, F.icoS, accent(), ICO_LIGHT_MODE);
    accReg(bl, ACC_TEXT);
    lv_obj_set_pos(bl, 16, 12);
    lv_obj_t* bTitle = label(bCard, F.small, cText(), "Helligkeit");
    lv_obj_set_pos(bTitle, 50, 10);
    sPct = label(bCard, F.small, accent(), "--%");
    accReg(sPct, ACC_TEXT);
    lv_obj_align(sPct, LV_ALIGN_TOP_RIGHT, -16, 10);
    sSlider = lv_slider_create(bCard);
    lv_obj_set_size(sSlider, CW - 32, 28);
    lv_obj_set_pos(sSlider, 16, 42);
    lv_slider_set_range(sSlider, 10, 255);
    lv_slider_set_value(sSlider, SettingsStorage::get().brightness, LV_ANIM_OFF);
    lv_obj_set_style_radius(sSlider, 14, LV_PART_MAIN);
    lv_obj_set_style_radius(sSlider, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sSlider, cSurf2(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sSlider, accent(), LV_PART_INDICATOR);
    accReg(sSlider, ACC_BG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sSlider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(sSlider, [](lv_event_t* e) {
        int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
        PowerManager::setBrightness((uint8_t)v);
        char b[8];
        snprintf(b, sizeof(b), "%d%%", v * 100 / 255);
        setText(sPct, b);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sSlider, [](lv_event_t*) { SettingsStorage::save(); haptic(HAPTIC_TICK); }, LV_EVENT_RELEASED, NULL);

    sTimeoutRow = valueRow(sWrapSystem, 88, ICO_TIMER, "Bildschirm-Timeout", "--", [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        int n = sizeof(TIMEOUT_STEPS) / sizeof(TIMEOUT_STEPS[0]);
        int nx = 0;
        for (int i = 0; i < n; i++) if (s.autoSleepSec == TIMEOUT_STEPS[i]) { nx = (i + 1) % n; break; }
        s.autoSleepSec = TIMEOUT_STEPS[nx];
        SettingsStorage::save();
        nav::markDirty(nav::PG_APP_SETTINGS);
        haptic(HAPTIC_TICK);
        refresh(true);
    });

    sAodTile = makeTile(sWrapSystem, M, 152, CW, 56, ICO_WATCH, "Always-On-Display", [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.aodEnabled = !s.aodEnabled;
        SettingsStorage::save();
        nav::markDirty(nav::PG_APP_SETTINGS);
        haptic(HAPTIC_TICK);
        refresh(true);
    }, nullptr);
    sWristTile = makeTile(sWrapSystem, M, 216, TILE_W, 56, ICO_VISIBILITY, "Handgelenk", [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.wristWakeEnabled = !s.wristWakeEnabled;
        SettingsStorage::save();
        nav::markDirty(nav::PG_APP_SETTINGS);
        haptic(HAPTIC_TICK);
        refresh(true);
    }, nullptr);
    sHapticTile = makeTile(sWrapSystem, M + TILE_W + 10, 216, TILE_W, 56, ICO_VIBRATION, "Haptik", [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.hapticEnabled = !s.hapticEnabled;
        SettingsStorage::save();
        nav::markDirty(nav::PG_APP_SETTINGS);
        haptic(HAPTIC_TICK);
        refresh(true);
    }, nullptr);

    lv_obj_t* pwrBtn = secondaryButton(sWrapSystem, CW, 48, ICO_POWER_SETTINGS_NEW, "Energie / Neustart");
    lv_obj_set_pos(pwrBtn, M, 280);
    lv_obj_add_event_cb(pwrBtn, [](lv_event_t*) { haptic(HAPTIC_TICK); nav::openApp(nav::PG_APP_POWER); }, LV_EVENT_CLICKED, NULL);

    // bottom grabber
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
