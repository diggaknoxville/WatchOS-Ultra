// LoRa & Meshtastic EU_868 Mesh: radio status, mode toggle, latest messages,
// position beacon and SOS (hold) actions.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"

using namespace ui;

namespace pages {

static lv_obj_t *lModePill, *lModeTxt, *lPill, *lPillTxt, *lCounts, *lMsg[3], *lMeta[3], *lEmpty;

static void refresh(bool) {
    char b[72];
    bool en = RadioLoRa::isEnabled();
    bool on = RadioLoRa::isReady();
    bool mesh = RadioLoRa::isMeshtasticMode();

    setBgColorIf(lModePill, mesh ? lv_color_hex(0xBF5AF2) : accent());
    setTextColorIf(lModeTxt, mesh ? lv_color_hex(0xBF5AF2) : accent());
    setText(lModeTxt, mesh ? "MESHTASTIC" : "KRONOS");

    setBgColorIf(lPill, on ? cOk() : cCrit());
    setTextColorIf(lPillTxt, on ? cOk() : cCrit());
    setText(lPillTxt, !en ? "LORA OFF" : on ? "ACTIVE" : "OFFLINE");

    snprintf(b, sizeof(b), "%s  ·  NODES %u  ·  TX %lu  RX %lu",
             mesh ? "869.5 MHz SF11 (LongFast)" : "868.0 MHz SF9 (Direct)",
             RadioLoRa::getPeerCount(),
             (unsigned long)RadioLoRa::getSentCount(),
             (unsigned long)RadioLoRa::getRecvCount());
    setText(lCounts, b);

    LoraMessage m[3];
    uint8_t n = RadioLoRa::getInbox(m, 3);
    setHidden(lEmpty, n != 0);
    for (int i = 0; i < 3; i++) {
        bool has = i < n;
        setHidden(lMsg[i], !has);
        setHidden(lMeta[i], !has);
        if (!has) continue;
        setText(lMsg[i], m[i].text);
        char a[24];
        ago(a, sizeof(a), m[i].epoch);
        snprintf(b, sizeof(b), "%s  ·  %.0f dBm  ·  SNR %.1f", a, m[i].rssi, m[i].snr);
        setText(lMeta[i], b);
    }
}

void buildLora(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "LORA MESH");

    lModePill = box(p, 108, 30);
    lv_obj_align(lModePill, LV_ALIGN_TOP_RIGHT, -(M + 104), 26);
    lv_obj_set_style_radius(lModePill, 15, 0);
    lv_obj_set_style_bg_opa(lModePill, LV_OPA_30, 0);
    lv_obj_add_flag(lModePill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lModePill, [](lv_event_t*) {
        bool next = !RadioLoRa::isMeshtasticMode();
        RadioLoRa::setMeshtasticMode(next);
        haptic(HAPTIC_CLICK);
        DisplayEngine::showNotification(next ? "Meshtastic EU_868" : "KRONOS Direct",
                                        next ? "869.525 MHz SF11 LongFast AES-128" : "868.0 MHz SF9 Direct Beacon",
                                        2200, ICO_PODCASTS);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lModeTxt = caption(lModePill, "MESHTASTIC", lv_color_hex(0xBF5AF2));
    lv_obj_center(lModeTxt);

    lPill = box(p, 96, 30);
    lv_obj_align(lPill, LV_ALIGN_TOP_RIGHT, -M, 26);
    lv_obj_set_style_radius(lPill, 15, 0);
    lv_obj_set_style_bg_opa(lPill, LV_OPA_30, 0);
    lv_obj_add_flag(lPill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lPill, [](lv_event_t*) {
        RadioLoRa::setEnabled(!RadioLoRa::isEnabled());
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lPillTxt = caption(lPill, "", cOk());
    lv_obj_center(lPillTxt);

    lCounts = caption(p, "", cText3());
    lv_obj_set_pos(lCounts, M, 84);

    lv_obj_t* c = card(p, CW, 196);
    lv_obj_set_pos(c, M, 108);
    lEmpty = label(c, F.small, cText3(), "No messages received yet");
    lv_obj_center(lEmpty);
    for (int i = 0; i < 3; i++) {
        lMsg[i] = label(c, F.small, cText(), "");
        lv_label_set_long_mode(lMsg[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(lMsg[i], CW - 40);
        lv_obj_set_pos(lMsg[i], 20, 14 + i * 60);
        lMeta[i] = caption(c, "", cText3());
        lv_obj_set_pos(lMeta[i], 20, 40 + i * 60);
    }

    lv_obj_t* ping = lv_button_create(p);
    lv_obj_set_size(ping, (CW - 10) / 2, 70);
    lv_obj_set_pos(ping, M, 318);
    lv_obj_set_style_radius(ping, R_CARD, 0);
    lv_obj_set_style_shadow_width(ping, 0, 0);
    lv_obj_set_style_bg_color(ping, cSurf2(), 0);
    lv_obj_set_style_border_width(ping, 2, 0);
    lv_obj_set_style_border_color(ping, accent(), 0);
    accReg(ping, ACC_BORDER);
    lv_obj_t* pl = iconText(ping, ICO_PODCASTS, "PING", accent());
    accReg(pl, ACC_TEXT);
    accReg(lv_obj_get_child(lv_obj_get_parent(pl), 0), ACC_TEXT);
    lv_obj_add_event_cb(ping, [](lv_event_t*) {
        bool ok = RadioLoRa::sendPacket("PING from KRONOS Ultra");
        DisplayEngine::showNotification("LoRa", ok ? "Ping im Mesh gesendet" : "Radio nicht bereit");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* posBtn = lv_button_create(p);
    lv_obj_set_size(posBtn, (CW - 10) / 2, 70);
    lv_obj_set_pos(posBtn, M + (CW - 10) / 2 + 10, 318);
    lv_obj_set_style_radius(posBtn, R_CARD, 0);
    lv_obj_set_style_shadow_width(posBtn, 0, 0);
    lv_obj_set_style_bg_color(posBtn, cSurf2(), 0);
    lv_obj_set_style_border_width(posBtn, 2, 0);
    lv_obj_set_style_border_color(posBtn, lv_color_hex(0xBF5AF2), 0);
    iconText(posBtn, ICO_MY_LOCATION, "POS SEND", lv_color_hex(0xBF5AF2));
    lv_obj_add_event_cb(posBtn, [](lv_event_t*) {
        bool ok = RadioLoRa::sendPositionBeacon();
        DisplayEngine::showNotification("Mesh Position", ok ? "GPS/Mesh-Position gesendet" : "Radio nicht bereit");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* sos = lv_button_create(p);
    lv_obj_set_size(sos, CW, 70);
    lv_obj_set_pos(sos, M, 400);
    lv_obj_set_style_radius(sos, R_CARD, 0);
    lv_obj_set_style_shadow_width(sos, 0, 0);
    lv_obj_set_style_bg_color(sos, cCrit(), 0);
    iconText(sos, ICO_SOS, "HOLD FOR SOS", cText());
    lv_obj_add_event_cb(sos, [](lv_event_t*) {
        RadioLoRa::sendSosBeacon();
        haptic(HAPTIC_ALERT_LONG);
        DisplayEngine::showNotification("SOS", "Emergency beacon transmitted with position");
    }, LV_EVENT_LONG_PRESSED, NULL);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 0;
    nav::registerPage(nav::PG_LORA, d);
}

}  // namespace pages
