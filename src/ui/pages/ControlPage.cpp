// Control centre sheet (swipe down on the watch face): quick toggles and a
// full-width brightness slider. Swipe up to close.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "AudioEngine.h"
#include "CompanionServer.h"
#include "GpsManager.h"
#include "DisplayEngine.h"
#include "RtcManager.h"

using namespace ui;

namespace pages {

#define N_TOGGLES 8
struct Toggle { lv_obj_t *tile, *icon, *label; };
static Toggle sT[N_TOGGLES];
static lv_obj_t *sSlider, *sPct, *sClock;
static int sSliderY = 0;
static int sPressBrightness = -1;     // value when the finger landed on the slider

static bool state(int i) {
    SystemSettings& s = SettingsStorage::get();
    switch (i) {
        case 0: return s.hapticEnabled;
        case 1: return s.soundEnabled;
        case 2: return s.wifiApEnabled;
        case 3: return s.loraBeaconEnabled;
        case 4: return s.aodEnabled;
        case 5: return s.wristWakeEnabled;
        case 6: return GpsManager::isPowered();
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
        case 0: s.hapticEnabled = !s.hapticEnabled; break;
        case 1: s.soundEnabled = !s.soundEnabled;
                if (s.soundEnabled) AudioEngine::playSoundFx(SFX_CHIME);
                break;
        case 2: s.wifiApEnabled = !s.wifiApEnabled; CompanionServer::requestAp(s.wifiApEnabled); break;
        case 3: s.loraBeaconEnabled = !s.loraBeaconEnabled; break;
        case 4: s.aodEnabled = !s.aodEnabled; break;
        case 5: s.wristWakeEnabled = !s.wristWakeEnabled; break;
        case 6: GpsManager::requestFix();
                DisplayEngine::showNotification("GNSS", "Acquiring satellites for time and position");
                haptic(HAPTIC_CLICK);
                return;
        case 7: DisplayEngine::setTorch(true); haptic(HAPTIC_CLICK); return;
    }
    SettingsStorage::save();
    paint(i);
    haptic(HAPTIC_CLICK);
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
}

// A vertical swipe that starts on the slider still closes the sheet (that is
// where the thumb naturally lands); the slider reverts the value it jumped to
// on press when the navigator takes the touch over (INDEV_RESET).
static bool allowGesture(int, int, int) { return true; }

void buildControl(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "CONTROL");
    sClock = label(p, F.title, cText2(), "");
    lv_obj_align(sClock, LV_ALIGN_TOP_RIGHT, -M, 22);

    static const char* icons[N_TOGGLES] = {ICO_VIBRATION, ICO_VOLUME_UP, ICO_SMARTPHONE, ICO_PODCASTS,
                                           ICO_WATCH, ICO_VISIBILITY, ICO_MY_LOCATION, ICO_FLASHLIGHT_ON};
    static const char* names[N_TOGGLES] = {"Haptics", "Sound", "Phone Link", "LoRa Beacon",
                                           "Always-On", "Wrist Wake", "Locate", "Torch"};
    const int w = (CW - 12) / 2, h = 72, y0 = 86;
    for (int i = 0; i < N_TOGGLES; i++) {
        lv_obj_t* c = card(p, w, h);
        lv_obj_set_style_radius(c, 24, 0);
        lv_obj_set_pos(c, M + (i % 2) * (w + 12), y0 + (i / 2) * (h + 10));
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, onToggle, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        sT[i].tile = c;
        sT[i].icon = label(c, F.icoS, cText(), icons[i]);
        lv_obj_align(sT[i].icon, LV_ALIGN_LEFT_MID, 18, 0);
        sT[i].label = label(c, F.small, cText2(), names[i]);
        lv_obj_align(sT[i].label, LV_ALIGN_LEFT_MID, 52, 0);
        paint(i);
    }
    sSliderY = y0 + 4 * (h + 10);

    sSlider = lv_slider_create(p);
    lv_obj_set_size(sSlider, CW, 52);
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
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 20, 0);
    sPct = label(sSlider, F.small, cText(), "");
    lv_obj_align(sPct, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_label_set_text_fmt(sPct, "%d%%", SettingsStorage::get().brightness * 100 / 255);
    lv_obj_add_event_cb(sSlider, [](lv_event_t* e) {
        int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
        PowerManager::setBrightness(v);
        SettingsStorage::get().brightness = v;
        lv_label_set_text_fmt(sPct, "%d%%", v * 100 / 255);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sSlider, [](lv_event_t*) {
        sPressBrightness = SettingsStorage::get().brightness;
    }, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sSlider, [](lv_event_t*) {
        if (sPressBrightness < 0) return;
        PowerManager::setBrightness(sPressBrightness);
        SettingsStorage::get().brightness = sPressBrightness;
        lv_slider_set_value(sSlider, sPressBrightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(sPct, "%d%%", sPressBrightness * 100 / 255);
        sPressBrightness = -1;
    }, LV_EVENT_INDEV_RESET, NULL);       // navigator took the touch over
    lv_obj_add_event_cb(sSlider, [](lv_event_t*) {
        sPressBrightness = -1;
        SettingsStorage::save();
    }, LV_EVENT_RELEASED, NULL);

    // grabber: hints that the sheet closes with an upward swipe
    lv_obj_t* grab = box(p, 40, 5);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, cText3(), 0);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -10);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 500;
    d.ttlMs = 10000;
    d.allowSheetGesture = allowGesture;
    nav::registerPage(nav::PG_CONTROL, d);
}

}  // namespace pages
