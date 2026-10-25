// Control centre sheet (swipe down on the watch face): quick toggles and a
// full-width brightness slider + screen timeout selector. Swipe up to close.
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

#define N_TOGGLES 8
struct Toggle { lv_obj_t *tile, *icon, *label; };
static Toggle sT[N_TOGGLES];
static lv_obj_t *sSlider, *sPct, *sClock;
static lv_obj_t *sTimeoutCard, *sTimeoutVal;
static int sSliderY = 0;
static int sTimeoutY = 0;

static const uint16_t TIMEOUT_PRESETS[] = {15, 30, 60, 120, 0};
static const char* TIMEOUT_LABELS[] = {"15s", "30s", "60s", "2 Min", "Nie"};
#define N_TIMEOUT_PRESETS 5

static void updateTimeoutLabel() {
    if (!sTimeoutVal) return;
    uint16_t cur = SettingsStorage::get().autoSleepSec;
    const char* txt = "30s";
    for (int i = 0; i < N_TIMEOUT_PRESETS; i++) {
        if (TIMEOUT_PRESETS[i] == cur) {
            txt = TIMEOUT_LABELS[i];
            break;
        }
    }
    if (cur == 0) txt = "Nie";
    else if (cur != 15 && cur != 30 && cur != 60 && cur != 120) {
        static char customBuf[16];
        snprintf(customBuf, sizeof(customBuf), "%ds", cur);
        txt = customBuf;
    }
    setText(sTimeoutVal, txt);
}

static bool state(int i) {
    SystemSettings& s = SettingsStorage::get();
    switch (i) {
        case 0: return CompanionServer::isApActive();
        case 1: return BleCompanion::isEnabled();
        case 2: return GpsManager::isEnabled();
        case 3: return RadioLoRa::isEnabled();
        case 4: return s.aodEnabled;
        case 5: return s.wristWakeEnabled;
        case 6: return s.hapticEnabled;
        default: return DisplayEngine::torchOn();
    }
}

static void paint(int i) {
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
                                            2500, ICO_WIFI);
            break;
        case 1: {
            bool en = !BleCompanion::isEnabled();
            BleCompanion::setEnabled(en);
            nav::markDirty(nav::PG_APP_CONNECT);
            DisplayEngine::showNotification(en ? "Bluetooth an" : "Bluetooth aus",
                                            en ? "BLE Companion aktiv" : "BLE abgeschaltet (Stromsparen)",
                                            2500, ICO_SMARTPHONE);
            break;
        }
        case 2: {
            bool en = !GpsManager::isEnabled();
            GpsManager::setEnabled(en);
            nav::markDirty(nav::PG_POSITION);
            DisplayEngine::showNotification(en ? "GPS an" : "GPS aus",
                                            en ? "Satellitensuche aktiv" : "GNSS abgeschaltet (Stromsparen)",
                                            2500, ICO_SATELLITE_ALT);
            break;
        }
        case 3: {
            bool en = !RadioLoRa::isEnabled();
            RadioLoRa::setEnabled(en);
            nav::markDirty(nav::PG_LORA);
            DisplayEngine::showNotification(en ? "LoRa an" : "LoRa aus",
                                            en ? "SX1262 868 MHz Empfang aktiv" : "LoRa Funk im Deep-Sleep",
                                            2500, ICO_PODCASTS);
            break;
        }
        case 4: s.aodEnabled = !s.aodEnabled; break;
        case 5: s.wristWakeEnabled = !s.wristWakeEnabled; break;
        case 6: s.hapticEnabled = !s.hapticEnabled; break;
        case 7: DisplayEngine::setTorch(true); haptic(HAPTIC_CLICK); return;
    }
    SettingsStorage::save();
    paint(i);
    haptic(HAPTIC_CLICK);
}

static void onTimeoutClick(lv_event_t*) {
    uint16_t cur = SettingsStorage::get().autoSleepSec;
    int nextIdx = 1; // default to 30s
    for (int i = 0; i < N_TIMEOUT_PRESETS; i++) {
        if (TIMEOUT_PRESETS[i] == cur) {
            nextIdx = (i + 1) % N_TIMEOUT_PRESETS;
            break;
        }
    }
    SettingsStorage::get().autoSleepSec = TIMEOUT_PRESETS[nextIdx];
    SettingsStorage::save();
    updateTimeoutLabel();
    haptic(HAPTIC_CLICK);
    if (SettingsStorage::get().autoSleepSec == 0) {
        DisplayEngine::showNotification("Display Timeout", "Display bleibt dauerhaft an (Nie)", 2500, ICO_LIGHT_MODE);
    } else {
        char msg[48];
        snprintf(msg, sizeof(msg), "Auto-Dim nach %s Inaktivitaet", TIMEOUT_LABELS[nextIdx]);
        DisplayEngine::showNotification("Display Timeout", msg, 2000, ICO_TIMER);
    }
}

static void refresh(bool) {
    for (int i = 0; i < N_TOGGLES; i++) paint(i);
    struct tm t;
    RtcManager::getLocalTimeInfo(&t);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    setText(sClock, b);
    int v = SettingsStorage::get().brightness;
    if (lv_slider_get_value(sSlider) != v && !lv_obj_has_state(sSlider, LV_STATE_PRESSED)) {
        lv_slider_set_value(sSlider, v, LV_ANIM_OFF);
        lv_label_set_text_fmt(sPct, "%d%%", v * 100 / 255);
    }
    updateTimeoutLabel();
}

static bool allowGesture(int, int y0, int) {
    // If the touch started inside the slider or timeout button, do not steal touch as sheet close!
    if (y0 >= sSliderY - 8 && y0 <= sSliderY + 54) return false;
    if (y0 >= sTimeoutY - 6 && y0 <= sTimeoutY + 48) return false;
    return true;
}

void buildControl(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "CONTROL");
    sClock = label(p, F.title, cText2(), "");
    lv_obj_align(sClock, LV_ALIGN_TOP_RIGHT, -M, 22);

    static const char* icons[N_TOGGLES] = {ICO_WIFI, ICO_SMARTPHONE, ICO_SATELLITE_ALT, ICO_PODCASTS,
                                           ICO_WATCH, ICO_VISIBILITY, ICO_VIBRATION, ICO_FLASHLIGHT_ON};
    static const char* names[N_TOGGLES] = {"WiFi AP", "Bluetooth", "GPS", "LoRa",
                                           "Always-On", "Wrist Wake", "Haptics", "Torch"};
    const int w = (CW - 12) / 2, h = 60, y0 = 74;
    for (int i = 0; i < N_TOGGLES; i++) {
        lv_obj_t* c = card(p, w, h);
        lv_obj_set_style_radius(c, 22, 0);
        lv_obj_set_pos(c, M + (i % 2) * (w + 12), y0 + (i / 2) * (h + 8));
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, onToggle, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        sT[i].tile = c;
        sT[i].icon = label(c, F.icoS, cText(), icons[i]);
        lv_obj_align(sT[i].icon, LV_ALIGN_LEFT_MID, 16, 0);
        sT[i].label = label(c, F.small, cText2(), names[i]);
        lv_obj_align(sT[i].label, LV_ALIGN_LEFT_MID, 48, 0);
        paint(i);
    }
    sSliderY = y0 + 4 * (h + 8) + 4; // 74 + 272 + 4 = 350

    sSlider = lv_slider_create(p);
    lv_obj_set_size(sSlider, CW, 46);
    lv_obj_set_pos(sSlider, M, sSliderY);
    lv_slider_set_range(sSlider, 10, 255);
    lv_slider_set_value(sSlider, SettingsStorage::get().brightness, LV_ANIM_OFF);
    lv_obj_set_style_radius(sSlider, R_CARD, LV_PART_MAIN);
    lv_obj_set_style_radius(sSlider, R_CARD, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sSlider, cSurf(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sSlider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sSlider, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sSlider, accent(), LV_PART_INDICATOR);
    accReg(sSlider, ACC_BG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sSlider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sSlider, 0, LV_PART_KNOB);
    lv_obj_t* bl = label(sSlider, F.icoS, cText(), ICO_LIGHT_MODE);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 18, 0);
    sPct = label(sSlider, F.small, cText(), "");
    lv_obj_align(sPct, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_label_set_text_fmt(sPct, "%d%%", SettingsStorage::get().brightness * 100 / 255);
    lv_obj_add_event_cb(sSlider, [](lv_event_t* e) {
        int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
        PowerManager::setBrightness(v);
        lv_label_set_text_fmt(sPct, "%d%%", v * 100 / 255);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sSlider, [](lv_event_t*) {
        SettingsStorage::save();
    }, LV_EVENT_RELEASED, NULL);

    // Timeout card: interactive pill to cycle display sleep timeout
    sTimeoutY = sSliderY + 52; // 350 + 52 = 402
    sTimeoutCard = card(p, CW, 40);
    lv_obj_set_style_radius(sTimeoutCard, 20, 0);
    lv_obj_set_pos(sTimeoutCard, M, sTimeoutY);
    lv_obj_add_flag(sTimeoutCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sTimeoutCard, onTimeoutClick, LV_EVENT_CLICKED, NULL);

    lv_obj_t* tIcon = label(sTimeoutCard, F.icoS, accent(), ICO_TIMER);
    accReg(tIcon, ACC_TEXT);
    lv_obj_align(tIcon, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t* tTitle = label(sTimeoutCard, F.small, cText(), "Screen Timeout");
    lv_obj_align(tTitle, LV_ALIGN_LEFT_MID, 50, 0);
    sTimeoutVal = label(sTimeoutCard, F.small, accent(), "30s");
    accReg(sTimeoutVal, ACC_TEXT);
    lv_obj_align(sTimeoutVal, LV_ALIGN_RIGHT_MID, -18, 0);
    updateTimeoutLabel();

    // grabber: hints that the sheet closes with an upward swipe
    lv_obj_t* grab = box(p, 40, 5);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, cText3(), 0);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -8);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 500;
    d.ttlMs = 10000;
    d.allowSheetGesture = allowGesture;
    nav::registerPage(nav::PG_CONTROL, d);
}

}  // namespace pages
