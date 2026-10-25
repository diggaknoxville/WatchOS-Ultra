// LoRa & Meshtastic EU_868 Hub Discovery + 868 MHz RF Spectrum Scanner + BLE Anti-Tracker Threat Radar
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"

using namespace ui;

namespace pages {

static lv_obj_t *lModePill = nullptr, *lModeTxt = nullptr;
static lv_obj_t *lPill = nullptr, *lPillTxt = nullptr;
static lv_obj_t *lCounts = nullptr;

// 3 View Tabs: 0 = MESH HUB & NODES, 1 = 868 SPEKTRUM, 2 = TRACKER RADAR
static int sLoraTab = 0;
static lv_obj_t *sTabBtns[3] = {nullptr};
static lv_obj_t *sViewMesh = nullptr;
static lv_obj_t *sViewSpec = nullptr;
static lv_obj_t *sViewRadar = nullptr;

// Tab 0: Real Meshtastic Hub & Peer Nodes + Inbox
static lv_obj_t *sNodeRows[3] = {nullptr};
static lv_obj_t *sNodeTitle[3] = {nullptr};
static lv_obj_t *sNodeMeta[3] = {nullptr};
static lv_obj_t *sNodeEmpty = nullptr;
static lv_obj_t *sLastRxLbl = nullptr;

// Tab 1: 868 MHz Sub-GHz RF Spectrum Analyzer
static lv_obj_t *sSpecHdr = nullptr;
static lv_obj_t *sSpecBars[24] = {nullptr};

// Tab 2: BLE Anti-Tracker Threat Radar
static lv_obj_t *sRadarHdr = nullptr;
static lv_obj_t *sThreatRows[4] = {nullptr};
static lv_obj_t *sThreatName[4] = {nullptr};
static lv_obj_t *sThreatMeta[4] = {nullptr};
static lv_obj_t *sThreatEmpty = nullptr;

static void refresh(bool) {
    char b[80];
    bool en = RadioLoRa::isEnabled();
    bool on = RadioLoRa::isReady();
    MeshPreset pr = RadioLoRa::getPreset();
    bool mesh = (pr != MESH_PRESET_KRONOS_DIR);

    const char* pShort = (pr == MESH_PRESET_LONG_FAST)  ? "LONGFAST" :
                         (pr == MESH_PRESET_MED_FAST)   ? "MEDFAST" :
                         (pr == MESH_PRESET_SHORT_FAST) ? "SHORTFAST" : "KRONOS";
    setBgColorIf(lModePill, mesh ? lv_color_hex(0xBF5AF2) : accent());
    setTextColorIf(lModeTxt, mesh ? lv_color_hex(0xBF5AF2) : accent());
    setText(lModeTxt, pShort);

    setBgColorIf(lPill, on ? cOk() : cCrit());
    setTextColorIf(lPillTxt, on ? cOk() : cCrit());
    setText(lPillTxt, !en ? "LORA OFF" : on ? "ACTIVE" : "OFFLINE");

    snprintf(b, sizeof(b), "%s · HUBS %u · TX %lu RX %lu",
             RadioLoRa::getPresetLabel(),
             RadioLoRa::getPeerCount(),
             (unsigned long)RadioLoRa::getSentCount(),
             (unsigned long)RadioLoRa::getRecvCount());
    setText(lCounts, b);

    for (int i = 0; i < 3; i++) {
        if (sTabBtns[i]) {
            setBgColorIf(sTabBtns[i], (i == sLoraTab) ? accent() : cSurf2());
        }
    }
    setHidden(sViewMesh,  sLoraTab != 0);
    setHidden(sViewSpec,  sLoraTab != 1);
    setHidden(sViewRadar, sLoraTab != 2);
    RadioLoRa::setSpectrumScanActive(sLoraTab == 1);

    if (sLoraTab == 0) {
        LoraPeerNode peers[3];
        uint8_t np = RadioLoRa::getPeers(peers, 3);
        setHidden(sNodeEmpty, np != 0);
        for (int i = 0; i < 3; i++) {
            bool has = (i < np);
            setHidden(sNodeRows[i], !has);
            if (!has) continue;
            char tbuf[56];
            if (peers[i].shortName[0]) {
                snprintf(tbuf, sizeof(tbuf), "%s [%s]  (!%04x)",
                         peers[i].name, peers[i].shortName, (unsigned)(peers[i].nodeId & 0xFFFF));
            } else {
                snprintf(tbuf, sizeof(tbuf), "%s  (!%08lx)",
                         peers[i].name, (unsigned long)peers[i].nodeId);
            }
            setText(sNodeTitle[i], tbuf);

            char a[20];
            ago(a, sizeof(a), peers[i].epoch);
            if (peers[i].hasPos) {
                snprintf(b, sizeof(b), "%.0f dBm · SNR %.1f · GPS %.3f,%.3f · %s",
                         peers[i].rssi, peers[i].snr, peers[i].lat, peers[i].lon, a);
            } else {
                snprintf(b, sizeof(b), "%.0f dBm · SNR %.1f · CH 0x%02X · %s",
                         peers[i].rssi, peers[i].snr, peers[i].chanHash, a);
            }
            setText(sNodeMeta[i], b);
        }

        LoraMessage lastMsg;
        if (RadioLoRa::getInbox(&lastMsg, 1) > 0) {
            snprintf(b, sizeof(b), "Letztes Paket: %s (%.0f dBm)", lastMsg.text, lastMsg.rssi);
        } else {
            snprintf(b, sizeof(b), "Letztes Paket: Warte auf Meshtastic Hub...");
        }
        setText(sLastRxLbl, b);
    } else if (sLoraTab == 1) {
        int8_t bins[24];
        float peakF = 869.525f;
        int8_t peakD = -115;
        RadioLoRa::getSpectrumBins(bins, 24, peakF, peakD);
        snprintf(b, sizeof(b), "863.0–870.0 MHz · Peak: %.3f MHz (%d dBm)", peakF, (int)peakD);
        setText(sSpecHdr, b);

        for (int i = 0; i < 24; i++) {
            if (!sSpecBars[i]) continue;
            // Map -120..-35 dBm to 6..126 px height
            int h = constrain((bins[i] + 122) * 2, 6, 126);
            lv_obj_set_height(sSpecBars[i], h);
            lv_obj_set_y(sSpecBars[i], 158 - h);
            lv_color_t col = (bins[i] > -75) ? cCrit() :
                             (bins[i] > -95) ? cWarn() :
                             (i == 21)       ? lv_color_hex(0xBF5AF2) : accent();
            setBgColorIf(sSpecBars[i], col);
        }
    } else if (sLoraTab == 2) {
        BleThreatInfo threats[4];
        uint8_t nt = RadioLoRa::getThreats(threats, 4);
        bool scanning = RadioLoRa::isThreatScanRunning();

        uint8_t trackerAlerts = 0;
        for (uint8_t i = 0; i < nt; i++) if (threats[i].isTracker) trackerAlerts++;

        if (scanning) {
            snprintf(b, sizeof(b), "Scanne BLE-Äther nach AirTags, SmartTags & Flipper...");
        } else {
            snprintf(b, sizeof(b), "%u BLE-Quellen erkannt · %u Tracker-Warnungen",
                     (unsigned)nt, (unsigned)trackerAlerts);
        }
        setText(sRadarHdr, b);
        setHidden(sThreatEmpty, nt != 0);

        for (int i = 0; i < 4; i++) {
            bool has = (i < nt);
            setHidden(sThreatRows[i], !has);
            if (!has) continue;
            char tb[56];
            snprintf(tb, sizeof(tb), "[%s] %s", threats[i].typeTag, threats[i].name);
            setText(sThreatName[i], tb);
            setTextColorIf(sThreatName[i], threats[i].isTracker ? cCrit() : cText());

            const char* dist = (threats[i].rssi > -55) ? "SEHR NAH (<2m)" :
                               (threats[i].rssi > -75) ? "NAH (~5m)" : "FERN (>10m)";
            snprintf(b, sizeof(b), "RSSI %d dBm · %s · MAC %s",
                     (int)threats[i].rssi, dist, threats[i].mac);
            setText(sThreatMeta[i], b);
        }
    }
}

void buildLora(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "LORA");

    // Preset Cycle Pill (LONGFAST -> MEDFAST -> SHORTFAST -> KRONOS)
    lModePill = box(p, 108, 30);
    lv_obj_align(lModePill, LV_ALIGN_TOP_RIGHT, -(M + 102), 26);
    lv_obj_set_style_radius(lModePill, 15, 0);
    lv_obj_set_style_bg_opa(lModePill, LV_OPA_30, 0);
    lv_obj_add_flag(lModePill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lModePill, [](lv_event_t*) {
        RadioLoRa::cyclePreset();
        RadioLoRa::sendNodeInfoDiscovery();
        haptic(HAPTIC_CLICK);
        DisplayEngine::showNotification("LoRa / Meshtastic Preset",
                                        RadioLoRa::getPresetLabel(),
                                        2000, ICO_PODCASTS);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lModeTxt = caption(lModePill, "LONGFAST", lv_color_hex(0xBF5AF2));
    lv_obj_center(lModeTxt);

    lPill = box(p, 94, 30);
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
    lv_obj_set_pos(lCounts, M, 68);

    // 3 View Mode Selector Pills: HUB & NODES | 868 SPEKTRUM | TRACKER RADAR
    static const char* TAB_NAMES[3] = {"HUB & NODES", "868 SPEKTRUM", "TRACKER RADAR"};
    int tw = (CW - 12) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* tb = box(p, tw, 32);
        lv_obj_set_pos(tb, M + i * (tw + 6), 92);
        lv_obj_set_style_radius(tb, 10, 0);
        lv_obj_set_style_bg_color(tb, (i == 0) ? accent() : cSurf2(), 0);
        lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
        lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tb, [](lv_event_t* e) {
            sLoraTab = (int)(intptr_t)lv_event_get_user_data(e);
            if (sLoraTab == 2) RadioLoRa::triggerThreatScan();
            haptic(HAPTIC_CLICK);
            refresh(true);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_center(caption(tb, TAB_NAMES[i], cText()));
        sTabBtns[i] = tb;
    }

    // =========================================================================
    // View 0: Real Meshtastic Hub & Peer Nodes + Discovery Actions
    // =========================================================================
    sViewMesh = box(p, CW, 354);
    lv_obj_set_pos(sViewMesh, M, 132);

    lv_obj_t* nodeCard = card(sViewMesh, CW, 206);
    lv_obj_set_pos(nodeCard, 0, 0);

    sNodeEmpty = label(nodeCard, F.small, cText2(),
                       "Noch kein Meshtastic-Hub geantwortet.\nTippe unten auf 'HUB SUCHEN' (NodeInfo)!");
    lv_obj_set_pos(sNodeEmpty, 16, 52);

    for (int i = 0; i < 3; i++) {
        lv_obj_t* r = box(nodeCard, CW - 20, 52);
        lv_obj_set_pos(r, 10, 8 + i * 54);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, cSurf2(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_60, 0);

        sNodeTitle[i] = label(r, F.small, lv_color_hex(0xBF5AF2), "");
        lv_obj_set_pos(sNodeTitle[i], 10, 4);

        sNodeMeta[i] = caption(r, "", cText2());
        lv_obj_set_pos(sNodeMeta[i], 10, 28);
        sNodeRows[i] = r;
    }

    sLastRxLbl = caption(nodeCard, "Letztes Paket: --", accent());
    accReg(sLastRxLbl, ACC_TEXT);
    lv_obj_set_pos(sLastRxLbl, 14, 176);

    // Action Buttons: HUB SUCHEN (sends NodeInfo want_response=true) | POS SEND | SOS
    lv_obj_t* discBtn = lv_button_create(sViewMesh);
    lv_obj_set_size(discBtn, (CW - 10) / 2, 62);
    lv_obj_set_pos(discBtn, 0, 216);
    lv_obj_set_style_radius(discBtn, R_CARD, 0);
    lv_obj_set_style_shadow_width(discBtn, 0, 0);
    lv_obj_set_style_bg_color(discBtn, cSurf2(), 0);
    lv_obj_set_style_border_width(discBtn, 2, 0);
    lv_obj_set_style_border_color(discBtn, accent(), 0);
    accReg(discBtn, ACC_BORDER);
    iconText(discBtn, ICO_PODCASTS, "HUB SUCHEN", accent());
    lv_obj_add_event_cb(discBtn, [](lv_event_t*) {
        bool ok = RadioLoRa::sendNodeInfoDiscovery();
        haptic(HAPTIC_CLICK);
        DisplayEngine::showNotification("Meshtastic Discovery",
                                        ok ? "NodeInfo-Anfrage (want_response) gesendet" : "LoRa nicht bereit",
                                        2500, ICO_PODCASTS);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* posBtn = lv_button_create(sViewMesh);
    lv_obj_set_size(posBtn, (CW - 10) / 2, 62);
    lv_obj_set_pos(posBtn, (CW - 10) / 2 + 10, 216);
    lv_obj_set_style_radius(posBtn, R_CARD, 0);
    lv_obj_set_style_shadow_width(posBtn, 0, 0);
    lv_obj_set_style_bg_color(posBtn, cSurf2(), 0);
    lv_obj_set_style_border_width(posBtn, 2, 0);
    lv_obj_set_style_border_color(posBtn, lv_color_hex(0xBF5AF2), 0);
    iconText(posBtn, ICO_MY_LOCATION, "POS SEND", lv_color_hex(0xBF5AF2));
    lv_obj_add_event_cb(posBtn, [](lv_event_t*) {
        bool ok = RadioLoRa::sendPositionBeacon();
        haptic(HAPTIC_CLICK);
        DisplayEngine::showNotification("Mesh Position",
                                        ok ? "GPS-Position ans Mesh gesendet" : "LoRa nicht bereit",
                                        2200, ICO_MY_LOCATION);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* sos = lv_button_create(sViewMesh);
    lv_obj_set_size(sos, CW, 62);
    lv_obj_set_pos(sos, 0, 286);
    lv_obj_set_style_radius(sos, R_CARD, 0);
    lv_obj_set_style_shadow_width(sos, 0, 0);
    lv_obj_set_style_bg_color(sos, cCrit(), 0);
    iconText(sos, ICO_SOS, "SOS NOTRUF (HALTEN)", cText());
    lv_obj_add_event_cb(sos, [](lv_event_t*) {
        RadioLoRa::sendSosBeacon();
        haptic(HAPTIC_ALERT_LONG);
        DisplayEngine::showNotification("SOS", "Notruf-Bake mit Position gesendet");
    }, LV_EVENT_LONG_PRESSED, NULL);

    // =========================================================================
    // View 1: 868 MHz Sub-GHz RF Spectrum Scanner (863.0 .. 870.0 MHz)
    // =========================================================================
    sViewSpec = card(p, CW, 344);
    lv_obj_set_pos(sViewSpec, M, 132);

    sSpecHdr = label(sViewSpec, F.small, accent(), "863.0–870.0 MHz · Live-RSSI Sweep");
    accReg(sSpecHdr, ACC_TEXT);
    lv_obj_set_pos(sSpecHdr, 14, 12);

    lv_obj_t* specGraph = box(sViewSpec, CW - 28, 176);
    lv_obj_set_pos(specGraph, 14, 44);
    lv_obj_set_style_radius(specGraph, 12, 0);
    lv_obj_set_style_bg_color(specGraph, cBg(), 0);
    lv_obj_set_style_bg_opa(specGraph, LV_OPA_COVER, 0);

    for (int i = 0; i < 24; i++) {
        lv_obj_t* b = box(specGraph, 11, 20);
        lv_obj_set_pos(b, 8 + i * 14, 138);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_bg_color(b, accent(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        sSpecBars[i] = b;
    }

    lv_obj_t* fLbl1 = caption(sViewSpec, "863 MHz", cText3());
    lv_obj_set_pos(fLbl1, 16, 228);
    lv_obj_t* fLbl2 = caption(sViewSpec, "866.5 MHz", cText3());
    lv_obj_set_pos(fLbl2, 146, 228);
    lv_obj_t* fLbl3 = caption(sViewSpec, "869.5 MHz (Mesh)", lv_color_hex(0xBF5AF2));
    lv_obj_set_pos(fLbl3, 250, 228);

    lv_obj_t* specInfo = caption(sViewSpec,
                                 "SX1262 Echtzeit-Spektrumanalyse im EU-ISM-Band.\n"
                                 "Lila markiert Slot 1 (869.525 MHz Meshtastic EU_868).",
                                 cText2());
    lv_obj_set_pos(specInfo, 16, 262);
    setHidden(sViewSpec, true);

    // =========================================================================
    // View 2: BLE / WiFi Anti-Tracker Threat Radar
    // =========================================================================
    sViewRadar = card(p, CW, 344);
    lv_obj_set_pos(sViewRadar, M, 132);

    sRadarHdr = label(sViewRadar, F.small, accent(), "Anti-Tracker & BLE Radar");
    accReg(sRadarHdr, ACC_TEXT);
    lv_obj_set_pos(sRadarHdr, 14, 12);

    sThreatEmpty = label(sViewRadar, F.small, cText2(), "Keine verdächtigen Tracker in Reichweite.");
    lv_obj_set_pos(sThreatEmpty, 16, 100);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* r = box(sViewRadar, CW - 24, 52);
        lv_obj_set_pos(r, 12, 42 + i * 56);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, cSurf2(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);

        sThreatName[i] = label(r, F.small, cText(), "");
        lv_obj_set_pos(sThreatName[i], 10, 4);

        sThreatMeta[i] = caption(r, "", cText2());
        lv_obj_set_pos(sThreatMeta[i], 10, 28);
        sThreatRows[i] = r;
    }

    lv_obj_t* scanBtn = box(sViewRadar, CW - 28, 52);
    lv_obj_set_pos(scanBtn, 14, 276);
    lv_obj_set_style_radius(scanBtn, 14, 0);
    lv_obj_set_style_bg_color(scanBtn, accent(), 0);
    lv_obj_set_style_bg_opa(scanBtn, LV_OPA_COVER, 0);
    accReg(scanBtn, ACC_BG);
    lv_obj_add_flag(scanBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scanBtn, [](lv_event_t*) {
        RadioLoRa::triggerThreatScan();
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(scanBtn, F.small, cBg(), "UMGEBUNG JETZT SCANNEN (AIRTAG / FLIPPER)"));
    setHidden(sViewRadar, true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 0;
    d.onShow = [] {
        RadioLoRa::sendNodeInfoDiscovery();
    };
    d.onHide = [] {
        RadioLoRa::setSpectrumScanActive(false);
    };
    nav::registerPage(nav::PG_LORA, d);
}

}  // namespace pages
