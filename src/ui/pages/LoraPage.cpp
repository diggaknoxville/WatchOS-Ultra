// LoRa / Meshtastic: peer list + discovery, 868 MHz spectrum sweep, BLE scan.
// No radio side effects in refresh(): discovery only via button, spectrum
// sweep only while the SPEKTRUM tab is visible, BLE scan only via button.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"

using namespace ui;

namespace pages {

static lv_obj_t *lModePill, *lStatePill, *lCounts, *lTabs;
static int sLoraTab = 0;                     // 0 = MESH, 1 = SPEKTRUM, 2 = BLE-SCAN
static lv_obj_t *sViewMesh, *sViewSpec, *sViewRadar;

// tab 0
static const int N_PEER_ROWS = 4;
static lv_obj_t *sNodeRows[N_PEER_ROWS], *sNodeIcon[N_PEER_ROWS], *sNodeName[N_PEER_ROWS];
static lv_obj_t *sNodeMeta[N_PEER_ROWS], *sNodeAge[N_PEER_ROWS], *sNodeRssi[N_PEER_ROWS];
static lv_obj_t *sNodeEmpty, *sLastRxLbl, *sDiscBtn, *sPosBtn, *sSosBtn;
static uint32_t sDiscMs = 0;                 // millis() of our last accepted discovery
static const uint32_t DISC_COOLDOWN_MS = 120000UL;

// tab 1
static lv_obj_t *sSpecHdr, *sSpecBars[LORA_SPECTRUM_BINS];

// tab 2
static lv_obj_t *sRadarHdr, *sThreatRows[4], *sThreatName[4], *sThreatMeta[4], *sThreatEmpty, *sScanBtn;
static bool sScanRequested = false;

// spectrum graph geometry: bins span 863.0 .. 870.0 MHz; the Meshtastic slot (869.525) is the last bin
static const int SPEC_BAR_W = (CW - 28 - 16) / LORA_SPECTRUM_BINS;
static const int MESH_BIN = LORA_SPECTRUM_BINS - 1;

static void applyTab(int tab) {
    sLoraTab = tab;
    setTabActive(lTabs, tab);
    setHidden(sViewMesh, tab != 0);
    setHidden(sViewSpec, tab != 1);
    setHidden(sViewRadar, tab != 2);
    RadioLoRa::setSpectrumScanActive(tab == 1 && nav::current() == nav::PG_LORA);
}

static void refresh(bool) {
    char b[96];
    bool en = RadioLoRa::isEnabled();
    bool on = en && RadioLoRa::isReady();
    MeshPreset pr = RadioLoRa::getPreset();
    bool mesh = (pr != MESH_PRESET_KRONOS_DIR);
    const char* pShort = (pr == MESH_PRESET_LONG_FAST)  ? "LONGFAST" :
                         (pr == MESH_PRESET_MED_FAST)   ? "MEDFAST" :
                         (pr == MESH_PRESET_SHORT_FAST) ? "SHORTFAST" : "DIREKT";
    setPill(lModePill, pShort, mesh ? cMesh() : accent());
    setPill(lStatePill, !en ? "AUS" : on ? "AKTIV" : "OFFLINE", !en ? cText2() : on ? cOk() : cCrit());

    snprintf(b, sizeof(b), "%s · %u KNOTEN · TX %lu · RX %lu", RadioLoRa::getPresetLabel(),
             (unsigned)RadioLoRa::getPeerCount(), (unsigned long)RadioLoRa::getSentCount(),
             (unsigned long)RadioLoRa::getRecvCount());
    setText(lCounts, b);

    if (sLoraTab == 0) {
        static LoraPeerNode peers[N_PEER_ROWS];
        uint8_t np = RadioLoRa::getPeers(peers, N_PEER_ROWS);
        setHidden(sNodeEmpty, np != 0);
        for (int i = 0; i < N_PEER_ROWS; i++) {
            bool has = (i < np);
            setHidden(sNodeRows[i], !has);
            if (!has) continue;
            const LoraPeerNode& pn = peers[i];
            if (pn.hasName && pn.name[0]) {
                if (pn.shortName[0]) snprintf(b, sizeof(b), "%.14s [%s]", pn.name, pn.shortName);
                else snprintf(b, sizeof(b), "%.18s", pn.name);
            } else {
                snprintf(b, sizeof(b), "!%08lx", (unsigned long)pn.nodeId);
            }
            setText(sNodeName[i], b);
            setTextColorIf(sNodeName[i], pn.isSos ? cCrit() : cText());
            setText(sNodeIcon[i], pn.isSos ? ICO_SOS : (pn.hasPos ? ICO_LOCATION_ON : ICO_CELL_TOWER));
            setTextColorIf(sNodeIcon[i], pn.isSos ? cCrit() : (pn.hasPos ? cOk() : cText3()));
            char a[24];
            ago(a, sizeof(a), (time_t)pn.epoch);
            setText(sNodeAge[i], a);
            if (pn.batPct > 0 && pn.hasPos) {
                snprintf(b, sizeof(b), "%u%% · %.0f dBm · %.3f, %.3f", pn.batPct, pn.rssi, pn.lat, pn.lon);
            } else if (pn.batPct > 0) {
                snprintf(b, sizeof(b), "%u%% · %.0f dBm · SNR %.1f", pn.batPct, pn.rssi, pn.snr);
            } else if (pn.hasPos) {
                snprintf(b, sizeof(b), "%.0f dBm · SNR %.1f · %.3f, %.3f", pn.rssi, pn.snr, pn.lat, pn.lon);
            } else {
                snprintf(b, sizeof(b), "%.0f dBm · SNR %.1f · keine Position", pn.rssi, pn.snr);
            }
            setText(sNodeMeta[i], b);
            int w = (int)((pn.rssi + 130.0f) * 100.0f / 90.0f);        // -130 .. -40 dBm -> 0..100 px
            if (w < 4) w = 4;
            if (w > 100) w = 100;
            if (lv_obj_get_width(sNodeRssi[i]) != w) lv_obj_set_width(sNodeRssi[i], w);
            setBgColorIf(sNodeRssi[i], pn.rssi > -80 ? cOk() : pn.rssi > -105 ? cWarn() : cCrit());
        }

        LoraMessage lastMsg;
        if (RadioLoRa::getInbox(&lastMsg, 1) > 0) {
            snprintf(b, sizeof(b), "Letztes Paket: %s (%.0f dBm)", lastMsg.text, lastMsg.rssi);
        } else {
            snprintf(b, sizeof(b), en ? "Letztes Paket: noch keins empfangen" : "LoRa ist ausgeschaltet");
        }
        setText(sLastRxLbl, b);

        uint32_t since = millis() - sDiscMs;
        bool cooling = sDiscMs && since < DISC_COOLDOWN_MS;
        if (cooling) snprintf(b, sizeof(b), "Discovery (%lu s)", (unsigned long)((DISC_COOLDOWN_MS - since) / 1000));
        else snprintf(b, sizeof(b), "Discovery");
        setButtonText(sDiscBtn, b);
        setButtonEnabled(sDiscBtn, en && !cooling);
        setButtonEnabled(sPosBtn, en);
        setButtonEnabled(sSosBtn, en);
    } else if (sLoraTab == 1) {
        int8_t bins[LORA_SPECTRUM_BINS];
        float peakF = 869.525f;
        int8_t peakD = -115;
        RadioLoRa::getSpectrumBins(bins, LORA_SPECTRUM_BINS, peakF, peakD);
        if (!en) snprintf(b, sizeof(b), "LoRa ist aus - kein Sweep");
        else snprintf(b, sizeof(b), "863-870 MHz · Peak %.3f MHz (%d dBm)", peakF, (int)peakD);
        setText(sSpecHdr, b);
        for (int i = 0; i < LORA_SPECTRUM_BINS; i++) {
            int h = constrain((bins[i] + 122) * 2, 6, 126);
            if (lv_obj_get_height(sSpecBars[i]) != h) {
                lv_obj_set_height(sSpecBars[i], h);
                lv_obj_set_y(sSpecBars[i], 158 - h);
            }
            lv_color_t col = (bins[i] > -75) ? cCrit() : (bins[i] > -95) ? cWarn() : (i == MESH_BIN) ? cMesh() : accent();
            setBgColorIf(sSpecBars[i], col);
        }
    } else {
        static BleThreatInfo threats[4];
        uint8_t nt = RadioLoRa::getThreats(threats, 4);
        bool scanning = RadioLoRa::isThreatScanRunning();
        uint8_t trackers = 0;
        for (uint8_t i = 0; i < nt; i++) if (threats[i].isTracker) trackers++;
        if (scanning) snprintf(b, sizeof(b), "Scanne Bluetooth-Umgebung...");
        else if (!sScanRequested && nt == 0) snprintf(b, sizeof(b), "Noch nicht gescannt");
        else snprintf(b, sizeof(b), "%u Geräte · %u mögliche Tracker", (unsigned)nt, (unsigned)trackers);
        setText(sRadarHdr, b);
        setHidden(sThreatEmpty, nt != 0 || scanning);
        setButtonEnabled(sScanBtn, !scanning);
        for (int i = 0; i < 4; i++) {
            bool has = (i < nt);
            setHidden(sThreatRows[i], !has);
            if (!has) continue;
            snprintf(b, sizeof(b), "%s%s", threats[i].name[0] ? threats[i].name : "(ohne Namen)",
                     threats[i].isTracker ? "  ·  möglicher Tracker" : "");
            setText(sThreatName[i], b);
            setTextColorIf(sThreatName[i], threats[i].isTracker ? cWarn() : cText());
            const char* dist = (threats[i].rssi > -55) ? "sehr nah" : (threats[i].rssi > -75) ? "nah" : "entfernt";
            snprintf(b, sizeof(b), "%s · %d dBm · %s", threats[i].typeTag, (int)threats[i].rssi, dist);
            setText(sThreatMeta[i], b);
        }
    }
}

static void onSosConfirm(bool ok, void*) {
    if (!ok) return;
    bool sent = RadioLoRa::sendSosBeacon();
    haptic(sent ? HAPTIC_ALERT_LONG : HAPTIC_TICK);
    if (sent) DisplayEngine::showNotification("SOS", "Notruf-Bake ins Mesh gesendet", 4000, ICO_SOS);
    else DisplayEngine::showToast("SOS", RadioLoRa::isEnabled() ? "Senden fehlgeschlagen" : "LoRa ist aus", 2500, ICO_SOS);
}

void buildLora(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "LORA");

    lModePill = pill(p, "LONGFAST", cMesh());
    lv_obj_align(lModePill, LV_ALIGN_TOP_RIGHT, -(M_HEAD + 78), 26);
    pressable(lModePill);
    lv_obj_add_event_cb(lModePill, [](lv_event_t*) {
        RadioLoRa::cyclePreset();
        haptic(HAPTIC_TICK);
        DisplayEngine::showToast("LoRa-Preset", RadioLoRa::getPresetLabel(), 2000, ICO_PODCASTS);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    lStatePill = pill(p, "AUS", cText2());
    lv_obj_align(lStatePill, LV_ALIGN_TOP_RIGHT, -M_HEAD, 26);
    pressable(lStatePill);
    lv_obj_add_event_cb(lStatePill, [](lv_event_t*) {
        bool en = !RadioLoRa::isEnabled();
        RadioLoRa::setEnabled(en);
        haptic(HAPTIC_TICK);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_APP_SETTINGS);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    static const char* TABS[3] = {"MESH", "SPEKTRUM", "BLE-SCAN"};
    lTabs = tabBar(p, M, CONTENT_Y, CW, TABS, 3, 0, [](lv_event_t* e) {
        applyTab((int)(intptr_t)lv_event_get_user_data(e));
        haptic(HAPTIC_TICK);
        refresh(true);
    });
    lCounts = caption(p, "", cText3());
    lv_obj_set_pos(lCounts, M, CONTENT_Y + 40);

    const int VIEW_Y = CONTENT_Y + 64;      // 144
    const int VIEW_H = SCR_H - VIEW_Y - 8;

    // ===================================================== tab 0: MESH
    sViewMesh = box(p, CW, VIEW_H);
    lv_obj_set_pos(sViewMesh, M, VIEW_Y);

    lv_obj_t* nodeCard = card(sViewMesh, CW, 220);
    lv_obj_set_pos(nodeCard, 0, 0);
    sNodeEmpty = emptyState(nodeCard, ICO_CELL_TOWER, "Keine Knoten bekannt", "Discovery sendet eine NodeInfo-Anfrage");
    lv_obj_align(sNodeEmpty, LV_ALIGN_TOP_MID, 0, 16);
    for (int i = 0; i < N_PEER_ROWS; i++) {
        lv_obj_t* r = box(nodeCard, CW - 20, 44);
        lv_obj_set_pos(r, 10, 8 + i * 46);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, cSurf2(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_60, 0);
        sNodeIcon[i] = label(r, F.icoS, cText3(), ICO_CELL_TOWER);
        lv_obj_align(sNodeIcon[i], LV_ALIGN_LEFT_MID, 8, 0);
        sNodeName[i] = label(r, F.small, cText(), "");
        lv_label_set_long_mode(sNodeName[i], LV_LABEL_LONG_DOT);
        lv_obj_set_size(sNodeName[i], 190, 18);
        lv_obj_set_pos(sNodeName[i], 40, 2);
        sNodeMeta[i] = caption(r, "", cText2());
        lv_obj_set_pos(sNodeMeta[i], 40, 24);
        sNodeAge[i] = caption(r, "", cText3());
        lv_obj_align(sNodeAge[i], LV_ALIGN_TOP_RIGHT, -8, 4);
        lv_obj_t* track = box(r, 100, 4);
        lv_obj_set_style_radius(track, 2, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(track, cStroke(), 0);
        lv_obj_align(track, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
        sNodeRssi[i] = box(track, 4, 4);
        lv_obj_set_style_radius(sNodeRssi[i], 2, 0);
        lv_obj_set_style_bg_opa(sNodeRssi[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sNodeRssi[i], cOk(), 0);
        lv_obj_set_pos(sNodeRssi[i], 0, 0);
        sNodeRows[i] = r;
    }
    sLastRxLbl = caption(nodeCard, "", accent());
    accReg(sLastRxLbl, ACC_TEXT);
    lv_label_set_long_mode(sLastRxLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sLastRxLbl, CW - 28);
    lv_obj_set_pos(sLastRxLbl, 14, 194);

    const int bw = (CW - 10) / 2;
    sDiscBtn = secondaryButton(sViewMesh, bw, 52, ICO_PODCASTS, "Discovery");
    lv_obj_set_pos(sDiscBtn, 0, 230);
    lv_obj_add_event_cb(sDiscBtn, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        bool ok = RadioLoRa::sendNodeInfoDiscovery();
        if (ok) {
            sDiscMs = millis();
            DisplayEngine::showToast("Mesh Discovery", "NodeInfo-Anfrage gesendet", 2000, ICO_PODCASTS);
        } else {
            DisplayEngine::showToast("Mesh Discovery",
                                     RadioLoRa::isEnabled() ? "Bereits gesendet - bitte warten" : "LoRa ist aus",
                                     2200, ICO_PODCASTS);
            if (RadioLoRa::isEnabled() && !sDiscMs) sDiscMs = millis();   // rate-limited elsewhere: show cooldown
        }
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    sPosBtn = secondaryButton(sViewMesh, bw, 52, ICO_MY_LOCATION, "Position senden");
    lv_obj_set_pos(sPosBtn, bw + 10, 230);
    lv_obj_add_event_cb(sPosBtn, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        bool ok = RadioLoRa::sendPositionBeacon();
        DisplayEngine::showToast("Mesh Position",
                                 ok ? "Position ins Mesh gesendet"
                                    : (RadioLoRa::isEnabled() ? "Keine aktuelle Position (GPS-Fix nötig)" : "LoRa ist aus"),
                                 2500, ICO_MY_LOCATION);
    }, LV_EVENT_CLICKED, NULL);

    sSosBtn = dangerButton(sViewMesh, CW_BOT, 50, ICO_SOS, "SOS-Notruf senden");
    lv_obj_set_pos(sSosBtn, (CW - CW_BOT) / 2, 292);
    lv_obj_add_event_cb(sSosBtn, [](lv_event_t*) {
        haptic(HAPTIC_CLICK);
        confirmDialog("SOS senden?", "Sendet eine Notruf-Bake mit deiner Position an alle Mesh-Knoten.",
                      "SOS senden", "Abbrechen", onSosConfirm, nullptr, true);
    }, LV_EVENT_CLICKED, NULL);

    // ===================================================== tab 1: SPEKTRUM
    sViewSpec = card(p, CW, VIEW_H);
    lv_obj_set_pos(sViewSpec, M, VIEW_Y);
    sSpecHdr = label(sViewSpec, F.small, accent(), "863-870 MHz");
    accReg(sSpecHdr, ACC_TEXT);
    lv_obj_set_pos(sSpecHdr, 14, 12);
    lv_obj_t* specGraph = box(sViewSpec, CW - 28, 176);
    lv_obj_set_pos(specGraph, 14, 44);
    lv_obj_set_style_radius(specGraph, 12, 0);
    lv_obj_set_style_bg_color(specGraph, cBg(), 0);
    lv_obj_set_style_bg_opa(specGraph, LV_OPA_COVER, 0);
    for (int i = 0; i < LORA_SPECTRUM_BINS; i++) {
        lv_obj_t* bar = box(specGraph, SPEC_BAR_W - 4, 20);
        lv_obj_set_pos(bar, 8 + i * SPEC_BAR_W, 138);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, accent(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        sSpecBars[i] = bar;
    }
    lv_obj_t* fLbl1 = caption(sViewSpec, "863", cText3());
    lv_obj_set_pos(fLbl1, 16, 228);
    lv_obj_t* fLbl2 = caption(sViewSpec, "866.5", cText3());
    lv_obj_set_pos(fLbl2, 160, 228);
    lv_obj_t* fLbl3 = caption(sViewSpec, "869.5 MESH", cMesh());
    lv_obj_align(fLbl3, LV_ALIGN_TOP_RIGHT, -16, 228);
    lv_obj_t* specInfo = caption(sViewSpec, "RSSI-Sweep des SX1262 im EU-ISM-Band.\nLila = Meshtastic-Slot 869.525 MHz.", cText2());
    lv_obj_set_pos(specInfo, 16, 262);
    setHidden(sViewSpec, true);

    // ===================================================== tab 2: BLE-SCAN
    sViewRadar = card(p, CW, VIEW_H);
    lv_obj_set_pos(sViewRadar, M, VIEW_Y);
    sRadarHdr = label(sViewRadar, F.small, accent(), "Noch nicht gescannt");
    accReg(sRadarHdr, ACC_TEXT);
    lv_obj_set_pos(sRadarHdr, 14, 12);
    sThreatEmpty = emptyState(sViewRadar, ICO_RADAR, "Keine Geräte", "Scan startet eine 1,5 s BLE-Suche");
    lv_obj_align(sThreatEmpty, LV_ALIGN_TOP_MID, 0, 50);
    for (int i = 0; i < 4; i++) {
        lv_obj_t* r = box(sViewRadar, CW - 24, 50);
        lv_obj_set_pos(r, 12, 42 + i * 54);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, cSurf2(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_70, 0);
        sThreatName[i] = label(r, F.small, cText(), "");
        lv_label_set_long_mode(sThreatName[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(sThreatName[i], CW - 44);
        lv_obj_set_pos(sThreatName[i], 10, 3);
        sThreatMeta[i] = caption(r, "", cText2());
        lv_obj_set_pos(sThreatMeta[i], 10, 27);
        sThreatRows[i] = r;
    }
    sScanBtn = primaryButton(sViewRadar, CW - 28, 48, ICO_RADAR, "Umgebung scannen");
    lv_obj_set_pos(sScanBtn, 14, VIEW_H - 62);
    lv_obj_add_event_cb(sScanBtn, [](lv_event_t*) {
        RadioLoRa::triggerThreatScan();
        sScanRequested = true;
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);
    setHidden(sViewRadar, true);

    refresh(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 30000;
    d.onShow = [] { RadioLoRa::setSpectrumScanActive(sLoraTab == 1); };
    d.onHide = [] { RadioLoRa::setSpectrumScanActive(false); };
    nav::registerPage(nav::PG_LORA, d);
}

}  // namespace pages
