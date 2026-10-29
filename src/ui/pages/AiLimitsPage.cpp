// ============================================================================
//  AiLimitsPage: Real-time Quota, Reset & Usage Dashboard for Anthropic,
//  Antigravity IDE & Google Cloud via Bluetooth Low Energy (BLE).
// ============================================================================
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "AiLimitsStore.h"
#include "BleCompanion.h"
#include "CompanionServer.h"
#include "DisplayEngine.h"
#include "SettingsStorage.h"
#include <time.h>

using namespace ui;

namespace pages {

// Theme colors
static const lv_color_t COL_ANTH_OUT = lv_color_hex(0xE58C45); // Anthropic Amber
static const lv_color_t COL_ANTH_IN  = lv_color_hex(0xF5A623); // Anthropic Warm Light
static const lv_color_t COL_AG_GEM   = lv_color_hex(0x00E5FF); // Neon Cyan
static const lv_color_t COL_AG_GEM2  = lv_color_hex(0x00B0FF);
static const lv_color_t COL_AG_CLD   = lv_color_hex(0xD946EF); // Neon Magenta/Purple
static const lv_color_t COL_AG_CLD2  = lv_color_hex(0x9333EA);
static const lv_color_t COL_GC_PRO   = lv_color_hex(0x4285F4); // Google Blue
static const lv_color_t COL_GC_FLS   = lv_color_hex(0x34A853); // Google Green
static const lv_color_t COL_GC_DAY   = lv_color_hex(0xFBBC05); // Google Yellow
static const lv_color_t COL_ARGON    = lv_color_hex(0x7C4DFF); // Deep Violet

// Widgets: Header & Status
static lv_obj_t *sBleDot, *sSyncTxt;
static lv_obj_t *sModeBtn, *sModeTxt;
static lv_obj_t *sCards[3], *sEmpty;
static lv_obj_t *sStaleBanner, *sStaleTxt;

// Sektion 1: Anthropic
static lv_obj_t *sAnthRing5h, *sAnthRingWk;
static lv_obj_t *sAnthTxt5h, *sAnthTxtWk;

// Sektion 2: Antigravity IDE
// 2A: Gemini Models
static lv_obj_t *sAgGemRing5h, *sAgGemRingWk;
static lv_obj_t *sAgGemCenter;
static lv_obj_t *sAgGemSub1, *sAgGemSub2;

// 2B: Claude & GPT Models
static lv_obj_t *sAgCldRing5h, *sAgCldRingWk;
static lv_obj_t *sAgCldCenter;
static lv_obj_t *sAgCldSub1, *sAgCldSub2;

// Sektion 3: Google Cloud
static lv_obj_t *sGcRingPro, *sGcRingFls, *sGcRingDay;
static lv_obj_t *sGcCenterPro, *sGcCenterFls, *sGcCenterDay;
static lv_obj_t *sGcSubPro, *sGcSubFls, *sGcSubDay;
// Bottom: Argon Weekly bar
static lv_obj_t *sArgonBar, *sArgonTxt;

static void refresh(bool) {
    AiLimitsData d = AiLimitsStore::get();
    bool ble = BleCompanion::isConnected();
    bool used = AiLimitsStore::isShowUsed();
    char buf[64], cdown[32];

    auto calcPct = [used](uint8_t raw) -> uint8_t {
        if (raw > 100) raw = 100;
        return used ? (100 - raw) : raw;
    };

    // Header Mode Toggle Pill & Sync Status
    if (used) {
        setText(sModeTxt, "VERBRAUCHT");
        setBgColorIf(sModeBtn, lv_color_hex(0x5A2000));
        setBorderWidthIf(sModeBtn, 1);
        setTextColorIf(sModeTxt, lv_color_hex(0xFF9800));
    } else {
        setText(sModeTxt, "REST");
        setBgColorIf(sModeBtn, lv_color_hex(0x0E3A20));
        setBorderWidthIf(sModeBtn, 1);
        setTextColorIf(sModeTxt, cOk());
    }

    setBgColorIf(sBleDot, ble ? cOk() : cStroke());
    time_t now = time(nullptr);
    uint32_t syncAge = (now > (time_t)d.last_sync_time) ? (uint32_t)(now - d.last_sync_time) : 0;
    if (d.last_sync_time) {
        char a[24];
        ago(a, sizeof(a), (time_t)d.last_sync_time);
        snprintf(buf, sizeof(buf), "%s", a);
    } else {
        snprintf(buf, sizeof(buf), "OFFLINE");
    }
    setText(sSyncTxt, buf);

    bool isStale = (syncAge > 12 * 3600);
    setHidden(sStaleBanner, !isStale);
    if (isStale) {
        char sbuf[64];
        if (syncAge < 86400) snprintf(sbuf, sizeof(sbuf), "Veraltet (%u h) • Tippen für Sync", (unsigned)(syncAge / 3600));
        else snprintf(sbuf, sizeof(sbuf), "Veraltet (%u d) • Tippen für Sync", (unsigned)(syncAge / 86400));
        setText(sStaleTxt, sbuf);
        if (sCards[0]) lv_obj_set_y(sCards[0], 96);
        if (sCards[1]) lv_obj_set_y(sCards[1], 196);
        if (sCards[2]) lv_obj_set_y(sCards[2], 322);
    } else {
        if (sCards[0]) lv_obj_set_y(sCards[0], 56);
        if (sCards[1]) lv_obj_set_y(sCards[1], 156);
        if (sCards[2]) lv_obj_set_y(sCards[2], 282);
    }

    // honest empty state: nothing has ever been received from the companion app
    setHidden(sEmpty, d.has_data);
    for (int i = 0; i < 3; i++) setHidden(sCards[i], !d.has_data);
    setHidden(sModeBtn, !d.has_data);
    if (!d.has_data) return;

    // Sektion 1: Anthropic
    uint8_t c5h = calcPct(d.claude_5h_pct);
    uint8_t cWk = calcPct(d.claude_wk_pct);
    setArc(sAnthRing5h, (float)c5h / 100.0f);
    setArc(sAnthRingWk, (float)cWk / 100.0f);

    AiLimitsStore::formatCountdown(cdown, sizeof(cdown), d.claude_5h_reset);
    snprintf(buf, sizeof(buf), "5h: %u%%  (%s)", c5h, cdown);
    setText(sAnthTxt5h, buf);

    AiLimitsStore::formatCountdown(cdown, sizeof(cdown), d.claude_wk_reset);
    snprintf(buf, sizeof(buf), "Wo: %u%%  (%s)", cWk, cdown);
    setText(sAnthTxtWk, buf);

    // Sektion 2: Antigravity IDE
    // 2A: Gemini
    uint8_t agG5h = calcPct(d.ag_gemini_5h_pct);
    uint8_t agGWk = calcPct(d.ag_gemini_wk_pct);
    setArc(sAgGemRing5h, (float)agG5h / 100.0f);
    setArc(sAgGemRingWk, (float)agGWk / 100.0f);
    snprintf(buf, sizeof(buf), "%u%%", agG5h);
    setText(sAgGemCenter, buf);

    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.ag_gemini_5h_reset);
    snprintf(buf, sizeof(buf), "5h: %s", cdown);
    setText(sAgGemSub1, buf);

    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.ag_gemini_wk_reset);
    snprintf(buf, sizeof(buf), "Wo: %u%% (%s)", agGWk, cdown);
    setText(sAgGemSub2, buf);

    // 2B: Claude / GPT
    uint8_t agC5h = calcPct(d.ag_claude_5h_pct);
    uint8_t agCWk = calcPct(d.ag_claude_wk_pct);
    setArc(sAgCldRing5h, (float)agC5h / 100.0f);
    setArc(sAgCldRingWk, (float)agCWk / 100.0f);
    snprintf(buf, sizeof(buf), "%u%%", agC5h);
    setText(sAgCldCenter, buf);

    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.ag_claude_5h_reset);
    snprintf(buf, sizeof(buf), "5h: %s", cdown);
    setText(sAgCldSub1, buf);

    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.ag_claude_wk_reset);
    snprintf(buf, sizeof(buf), "Wo: %u%% (%s)", agCWk, cdown);
    setText(sAgCldSub2, buf);

    // Sektion 3: Google Cloud
    uint8_t gcPro = calcPct(d.gc_5h_pct);
    uint8_t gcFls = calcPct(d.gc_weekly_pct);
    uint8_t gcDay = calcPct(d.gc_daily_pct);
    setArc(sGcRingPro, (float)gcPro / 100.0f);
    setArc(sGcRingFls, (float)gcFls / 100.0f);
    setArc(sGcRingDay, (float)gcDay / 100.0f);

    snprintf(buf, sizeof(buf), "%u%%", gcPro); setText(sGcCenterPro, buf);
    snprintf(buf, sizeof(buf), "%u%%", gcFls); setText(sGcCenterFls, buf);
    snprintf(buf, sizeof(buf), "%u%%", gcDay); setText(sGcCenterDay, buf);

    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.gc_5h_reset);
    setText(sGcSubPro, cdown);
    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.gc_weekly_reset);
    setText(sGcSubFls, cdown);
    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.gc_daily_reset);
    setText(sGcSubDay, cdown);

    // Sektion 3: Argon Weekly Bar
    uint8_t argVal = calcPct(d.argon_weekly_pct);
    setBar(sArgonBar, argVal);
    AiLimitsStore::formatCountdown(cdown, sizeof(cdown), d.argon_weekly_reset);
    snprintf(buf, sizeof(buf), "ARGON WOCHE  %u%% · %s", argVal, cdown);
    setText(sArgonTxt, buf);
}

void buildAiLimits(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);

    // Top Title & Status
    title(p, "LIMITS");

    // Mode Toggle Pill: REST % vs VERBRAUCHT %
    sModeBtn = card(p, 84, 26);
    lv_obj_align(sModeBtn, LV_ALIGN_TOP_RIGHT, -(M_HEAD + 120), 26);
    lv_obj_set_style_radius(sModeBtn, 13, 0);
    lv_obj_set_style_pad_all(sModeBtn, 0, 0);
    lv_obj_add_flag(sModeBtn, LV_OBJ_FLAG_CLICKABLE);
    sModeTxt = label(sModeBtn, F.cap, cText(), "REST");
    pressable(sModeBtn);
    lv_obj_center(sModeTxt);
    lv_obj_add_event_cb(sModeBtn, [](lv_event_t*) {
        AiLimitsStore::toggleShowUsed();
        haptic(HAPTIC_CLICK);
        refresh(true);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* statRow = box(p, LV_SIZE_CONTENT, 24);
    lv_obj_align(statRow, LV_ALIGN_TOP_RIGHT, -M_HEAD, 26);
    lv_obj_set_flex_flow(statRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statRow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(statRow, 8, 0);

    sBleDot = box(statRow, 8, 8);
    lv_obj_set_style_radius(sBleDot, 4, 0);
    lv_obj_set_style_bg_opa(sBleDot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sBleDot, cStroke(), 0);

    sSyncTxt = caption(statRow, "OFFLINE", cText3());

    sEmpty = emptyState(p, ICO_SYNC, "Noch nicht synchronisiert", "TIPPT ZUM STARTEN DES HOTSPOTS");
    lv_obj_align(sEmpty, LV_ALIGN_CENTER, 0, 20);
    pressable(sEmpty);
    lv_obj_add_event_cb(sEmpty, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.wifiApEnabled = true;
        SettingsStorage::save();
        CompanionServer::requestAp(true);
        DisplayEngine::showToast("Hotspot aktiv", "Handy per WLAN verbinden", 3000, ICO_WIFI);
        haptic(HAPTIC_CLICK);
        nav::markDirty(nav::PG_APP_CONNECT);
        nav::markDirty(nav::PG_CONTROL);
    }, LV_EVENT_CLICKED, NULL);

    // Stale-Alert Banner directly under header
    sStaleBanner = lv_obj_create(p);
    lv_obj_set_size(sStaleBanner, CW, 36);
    lv_obj_set_pos(sStaleBanner, M, 54);
    lv_obj_set_style_bg_color(sStaleBanner, lv_color_hex(0x281700), 0);
    lv_obj_set_style_bg_opa(sStaleBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sStaleBanner, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_border_width(sStaleBanner, 2, 0);
    lv_obj_set_style_radius(sStaleBanner, 10, 0);
    lv_obj_set_flex_flow(sStaleBanner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sStaleBanner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sStaleBanner, 8, 0);
    pressable(sStaleBanner);

    label(sStaleBanner, F.icoS, lv_color_hex(0xF59E0B), ICO_INFO);
    sStaleTxt = label(sStaleBanner, F.cap, lv_color_hex(0xFDE68A), "Veraltet • Tippen für Sync");
    lv_obj_add_event_cb(sStaleBanner, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.wifiApEnabled = true;
        SettingsStorage::save();
        CompanionServer::requestAp(true);
        haptic(HAPTIC_CLICK);
        nav::markDirty(nav::PG_APP_CONNECT);
        nav::markDirty(nav::PG_CONTROL);
        nav::openApp(nav::PG_APP_CONNECT);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(sStaleBanner, LV_OBJ_FLAG_HIDDEN);

    // ========================================================================
    // SEKTION 1: ANTHROPIC (CLAUDE PRO)
    // ========================================================================
    lv_obj_t* card1 = card(p, CW, 94);
    lv_obj_set_pos(card1, M, 56);
    sCards[0] = card1;

    lv_obj_t* cap1 = caption(card1, "ANTHROPIC  •  CLAUDE", COL_ANTH_OUT);
    lv_obj_set_pos(cap1, 16, 8);

    const int R1_SZ = 58, R1_W = 5;
    const int R1_IN_SZ = 42;
    const int R1_X = 16, R1_Y = 24;

    sAnthRing5h = ring(card1, R1_SZ, R1_W, COL_ANTH_OUT);
    lv_obj_set_pos(sAnthRing5h, R1_X, R1_Y);

    sAnthRingWk = ring(card1, R1_IN_SZ, R1_W, COL_ANTH_IN);
    lv_obj_set_pos(sAnthRingWk, R1_X + (R1_SZ - R1_IN_SZ) / 2, R1_Y + (R1_SZ - R1_IN_SZ) / 2);

    const int TXT1_X = R1_X + R1_SZ + 16;
    sAnthTxt5h = label(card1, F.body, cText(), "5h: --");
    lv_obj_set_pos(sAnthTxt5h, TXT1_X, 28);

    sAnthTxtWk = label(card1, F.body, cText2(), "Wo: --");
    lv_obj_set_pos(sAnthTxtWk, TXT1_X, 56);

    // ========================================================================
    // SEKTION 2: ANTIGRAVITY IDE (GEMINI + CLAUDE)
    // ========================================================================
    lv_obj_t* card2 = card(p, CW, 120);
    lv_obj_set_pos(card2, M, 156);
    sCards[1] = card2;

    lv_obj_t* cap2 = caption(card2, "ANTIGRAVITY IDE", COL_AG_GEM);
    lv_obj_set_pos(cap2, 16, 6);

    const int R2_SZ = 50, R2_IN_SZ = 36, R2_W = 4;
    const int COL_W = (CW - 32) / 2; // ~165px
    const int G2_Y = 24;

    // --- Sub-group 2A: Gemini Models ---
    const int G2A_X = 14;
    sAgGemRing5h = ring(card2, R2_SZ, R2_W, COL_AG_GEM);
    lv_obj_set_pos(sAgGemRing5h, G2A_X, G2_Y + 12);

    sAgGemRingWk = ring(card2, R2_IN_SZ, R2_W, COL_AG_GEM2);
    lv_obj_set_pos(sAgGemRingWk, G2A_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + 12 + (R2_SZ - R2_IN_SZ) / 2);

    sAgGemCenter = label(card2, F.cap, cText(), "--");
    lv_obj_set_size(sAgGemCenter, R2_IN_SZ, 18);
    lv_obj_set_style_text_align(sAgGemCenter, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sAgGemCenter, G2A_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + 12 + (R2_SZ - R2_IN_SZ) / 2 + 8);

    lv_obj_t* lblGem = caption(card2, "GEMINI", COL_AG_GEM);
    lv_obj_set_pos(lblGem, G2A_X, G2_Y - 4);
    sAgGemSub1 = caption(card2, "5h: --", cText());
    lv_obj_set_pos(sAgGemSub1, G2A_X + R2_SZ + 8, G2_Y + 16);
    sAgGemSub2 = caption(card2, "Wo: --", cText2());
    lv_obj_set_pos(sAgGemSub2, G2A_X + R2_SZ + 8, G2_Y + 38);

    // --- Sub-group 2B: Claude & GPT Models ---
    const int G2B_X = 14 + COL_W;
    sAgCldRing5h = ring(card2, R2_SZ, R2_W, COL_AG_CLD);
    lv_obj_set_pos(sAgCldRing5h, G2B_X, G2_Y + 12);

    sAgCldRingWk = ring(card2, R2_IN_SZ, R2_W, COL_AG_CLD2);
    lv_obj_set_pos(sAgCldRingWk, G2B_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + 12 + (R2_SZ - R2_IN_SZ) / 2);

    sAgCldCenter = label(card2, F.cap, cText(), "--");
    lv_obj_set_size(sAgCldCenter, R2_IN_SZ, 18);
    lv_obj_set_style_text_align(sAgCldCenter, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sAgCldCenter, G2B_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + 12 + (R2_SZ - R2_IN_SZ) / 2 + 8);

    lv_obj_t* lblCld = caption(card2, "CLAUDE", COL_AG_CLD);
    lv_obj_set_pos(lblCld, G2B_X, G2_Y - 4);
    sAgCldSub1 = caption(card2, "5h: --", cText());
    lv_obj_set_pos(sAgCldSub1, G2B_X + R2_SZ + 8, G2_Y + 16);
    sAgCldSub2 = caption(card2, "Wo: --", cText2());
    lv_obj_set_pos(sAgCldSub2, G2B_X + R2_SZ + 8, G2_Y + 38);

    // ========================================================================
    // SEKTION 3: GOOGLE CLOUD & GEMINI 4 ARGON
    // ========================================================================
    lv_obj_t* card3 = card(p, CW, 146);
    lv_obj_set_pos(card3, M, 282);
    sCards[2] = card3;

    lv_obj_t* cap3 = caption(card3, "GOOGLE CLOUD", COL_GC_PRO);
    lv_obj_set_pos(cap3, 16, 6);

    const int R3_SZ = 44, R3_W = 4;
    const int COL3_W = (CW - 32) / 3; // ~110px
    const int G3_Y = 22;

    // Ring 1: Pro (5h)
    int r1_x = 16 + (COL3_W - R3_SZ) / 2;
    sGcRingPro = ring(card3, R3_SZ, R3_W, COL_GC_PRO);
    lv_obj_set_pos(sGcRingPro, r1_x, G3_Y);
    sGcCenterPro = label(card3, F.cap, cText(), "--");
    lv_obj_set_size(sGcCenterPro, R3_SZ, 18);
    lv_obj_set_style_text_align(sGcCenterPro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sGcCenterPro, r1_x, G3_Y + 13);

    lv_obj_t* lblPro = caption(card3, "PRO 5h", COL_GC_PRO);
    lv_obj_set_width(lblPro, COL3_W);
    lv_obj_set_style_text_align(lblPro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblPro, 16, G3_Y + R3_SZ + 2);
    sGcSubPro = caption(card3, "--", cText3());
    lv_obj_set_width(sGcSubPro, COL3_W);
    lv_obj_set_style_text_align(sGcSubPro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sGcSubPro, 16, G3_Y + R3_SZ + 18);

    // Ring 2: Flash (Weekly)
    int r2_x = 16 + COL3_W + (COL3_W - R3_SZ) / 2;
    sGcRingFls = ring(card3, R3_SZ, R3_W, COL_GC_FLS);
    lv_obj_set_pos(sGcRingFls, r2_x, G3_Y);
    sGcCenterFls = label(card3, F.cap, cText(), "--");
    lv_obj_set_size(sGcCenterFls, R3_SZ, 18);
    lv_obj_set_style_text_align(sGcCenterFls, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sGcCenterFls, r2_x, G3_Y + 13);

    lv_obj_t* lblFls = caption(card3, "FLASH Wo", COL_GC_FLS);
    lv_obj_set_width(lblFls, COL3_W);
    lv_obj_set_style_text_align(lblFls, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblFls, 16 + COL3_W, G3_Y + R3_SZ + 2);
    sGcSubFls = caption(card3, "--", cText3());
    lv_obj_set_width(sGcSubFls, COL3_W);
    lv_obj_set_style_text_align(sGcSubFls, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sGcSubFls, 16 + COL3_W, G3_Y + R3_SZ + 18);

    // Ring 3: Daily Quota
    int r3_x = 16 + 2 * COL3_W + (COL3_W - R3_SZ) / 2;
    sGcRingDay = ring(card3, R3_SZ, R3_W, COL_GC_DAY);
    lv_obj_set_pos(sGcRingDay, r3_x, G3_Y);
    sGcCenterDay = label(card3, F.cap, cText(), "--");
    lv_obj_set_size(sGcCenterDay, R3_SZ, 18);
    lv_obj_set_style_text_align(sGcCenterDay, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sGcCenterDay, r3_x, G3_Y + 13);

    lv_obj_t* lblDay = caption(card3, "TAG", COL_GC_DAY);
    lv_obj_set_width(lblDay, COL3_W);
    lv_obj_set_style_text_align(lblDay, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblDay, 16 + 2 * COL3_W, G3_Y + R3_SZ + 2);
    sGcSubDay = caption(card3, "--", cText3());
    lv_obj_set_width(sGcSubDay, COL3_W);
    lv_obj_set_style_text_align(sGcSubDay, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sGcSubDay, 16 + 2 * COL3_W, G3_Y + R3_SZ + 18);

    // --- Bottom Bar: Gemini 4 Argon Weekly ---
    const int BAR_Y = 106;
    sArgonTxt = caption(card3, "ARGON WOCHE  --% · --", COL_ARGON);
    lv_obj_set_pos(sArgonTxt, 16, BAR_Y);

    sArgonBar = lv_bar_create(card3);
    lv_obj_set_size(sArgonBar, CW - 32, 6);
    lv_obj_set_pos(sArgonBar, 16, BAR_Y + 18);
    lv_bar_set_range(sArgonBar, 0, 100);
    lv_obj_set_style_bg_color(sArgonBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sArgonBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sArgonBar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sArgonBar, COL_ARGON, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sArgonBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sArgonBar, 3, LV_PART_INDICATOR);

    // Register with navigator
    refresh(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_AI_LIMITS, d);
}

} // namespace pages

