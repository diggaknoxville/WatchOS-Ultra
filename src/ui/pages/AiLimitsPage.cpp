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
static lv_obj_t *sAgGemCap, *sAgCldCap, *sGcCapPro, *sGcCapFls, *sGcCapDay;
static lv_obj_t *sStaleBanner, *sStaleTxt;

// Sektion 1: Anthropic
static lv_obj_t *sAnthRing5h, *sAnthRingWk;
static lv_obj_t *sAnthTxt5h, *sAnthTxtWk;

// Sektion 2: Antigravity IDE
// 2A: Gemini Models
static lv_obj_t *sAgGemRing5h, *sAgGemRingWk;
static lv_obj_t *sAgGemCenter5h, *sAgGemCenterWk;
// 2B: Claude & GPT Models
static lv_obj_t *sAgCldRing5h, *sAgCldRingWk;
static lv_obj_t *sAgCldCenter5h, *sAgCldCenterWk;

// Sektion 3: Google Cloud
static lv_obj_t *sGcRingPro, *sGcRingFls, *sGcRingDay;
static lv_obj_t *sGcCenterPro, *sGcCenterFls, *sGcCenterDay;
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
        if (syncAge < 86400) snprintf(sbuf, sizeof(sbuf), "Veraltet (vor %u h) • Hotspot für Sync", (unsigned)(syncAge / 3600));
        else snprintf(sbuf, sizeof(sbuf), "Veraltet (vor %u d) • Hotspot für Sync", (unsigned)(syncAge / 86400));
        setText(sStaleTxt, sbuf);
        lv_obj_set_y(sCards[0], 96);
        lv_obj_set_y(sCards[1], 198);
        lv_obj_set_y(sCards[2], 328);
    } else {
        lv_obj_set_y(sCards[0], 60);
        lv_obj_set_y(sCards[1], 162);
        lv_obj_set_y(sCards[2], 292);
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
    snprintf(buf, sizeof(buf), "5h %u%%", agG5h);
    setText(sAgGemCenter5h, buf);
    snprintf(buf, sizeof(buf), "Wo %u%%", agGWk);
    setText(sAgGemCenterWk, buf);
    {
        char c2[16];
        AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.ag_gemini_5h_reset);
        AiLimitsStore::formatCompactCountdown(c2, sizeof(c2), d.ag_gemini_wk_reset);
        snprintf(buf, sizeof(buf), "GEMINI · %s / %s", cdown, c2);
        setText(sAgGemCap, buf);
    }

    // 2B: Claude / GPT
    uint8_t agC5h = calcPct(d.ag_claude_5h_pct);
    uint8_t agCWk = calcPct(d.ag_claude_wk_pct);
    setArc(sAgCldRing5h, (float)agC5h / 100.0f);
    setArc(sAgCldRingWk, (float)agCWk / 100.0f);
    snprintf(buf, sizeof(buf), "5h %u%%", agC5h);
    setText(sAgCldCenter5h, buf);
    snprintf(buf, sizeof(buf), "Wo %u%%", agCWk);
    setText(sAgCldCenterWk, buf);
    {
        char c2[16];
        AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.ag_claude_5h_reset);
        AiLimitsStore::formatCompactCountdown(c2, sizeof(c2), d.ag_claude_wk_reset);
        snprintf(buf, sizeof(buf), "CLAUDE / GPT · %s / %s", cdown, c2);
        setText(sAgCldCap, buf);
    }

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
    snprintf(buf, sizeof(buf), "PRO 5h · %s", cdown); setText(sGcCapPro, buf);
    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.gc_weekly_reset);
    snprintf(buf, sizeof(buf), "FLASH Wo · %s", cdown); setText(sGcCapFls, buf);
    AiLimitsStore::formatCompactCountdown(cdown, sizeof(cdown), d.gc_daily_reset);
    snprintf(buf, sizeof(buf), "TAG · %s", cdown); setText(sGcCapDay, buf);

    // Sektion 3: Argon Weekly Bar
    uint8_t argVal = calcPct(d.argon_weekly_pct);
    if (lv_bar_get_value(sArgonBar) != argVal) lv_bar_set_value(sArgonBar, argVal, LV_ANIM_OFF);
    AiLimitsStore::formatCountdown(cdown, sizeof(cdown), d.argon_weekly_reset);
    snprintf(buf, sizeof(buf), "ARGON WOCHE  %u%% • %s", argVal, cdown);
    setText(sArgonTxt, buf);
}

void buildAiLimits(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);

    // Top Title & BLE Sync Status
    title(p, "LIMITS");

    // Mode Toggle Pill: REST % vs VERBRAUCHT %
    sModeBtn = card(p, 78, 26);
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

    sEmpty = emptyState(p, ICO_SYNC, "Noch nicht synchronisiert", "COMPANION-APP PER BLUETOOTH VERBINDEN");
    lv_obj_align(sEmpty, LV_ALIGN_CENTER, 0, 20);

    // ========================================================================
    // SEKTION 1: ANTHROPIC (CLAUDE PRO)
    // ========================================================================
    const int S1_Y = 60;
    lv_obj_t* card1 = card(p, CW, 94);
    lv_obj_set_pos(card1, M, S1_Y);
    sCards[0] = card1;

    lv_obj_t* cap1 = caption(card1, "ANTHROPIC  •  CLAUDE", COL_ANTH_OUT);
    lv_obj_set_pos(cap1, 16, 8);

    // Dual concentric rings (left)
    const int R1_SZ = 64, R1_W = 5;
    const int R1_IN_SZ = 48;
    const int R1_X = 16, R1_Y = 24;

    sAnthRing5h = ring(card1, R1_SZ, R1_W, COL_ANTH_OUT);
    lv_obj_set_pos(sAnthRing5h, R1_X, R1_Y);

    sAnthRingWk = ring(card1, R1_IN_SZ, R1_W, COL_ANTH_IN);
    lv_obj_set_pos(sAnthRingWk, R1_X + (R1_SZ - R1_IN_SZ) / 2, R1_Y + (R1_SZ - R1_IN_SZ) / 2);

    // Countdowns & percentages beside the rings (right)
    const int TXT1_X = R1_X + R1_SZ + 16;
    sAnthTxt5h = label(card1, F.body, cText(), "5h: --");
    lv_obj_set_pos(sAnthTxt5h, TXT1_X, 30);

    sAnthTxtWk = label(card1, F.body, cText2(), "Wk: --");
    lv_obj_set_pos(sAnthTxtWk, TXT1_X, 58);

    // ========================================================================
    // SEKTION 2: ANTIGRAVITY IDE (GOOGLE AI PRO)
    // ========================================================================
    const int S2_Y = 162;
    lv_obj_t* card2 = card(p, CW, 122);
    lv_obj_set_pos(card2, M, S2_Y);
    sCards[1] = card2;

    lv_obj_t* cap2 = caption(card2, "ANTIGRAVITY IDE", COL_AG_GEM);
    lv_obj_set_pos(cap2, 16, 8);

    const int R2_SZ = 62, R2_IN_SZ = 46, R2_W = 4;
    const int COL_W = (CW - 32) / 2; // ~165px

    // --- Sub-group 2A: Gemini Models ---
    const int G2A_X = 16 + (COL_W - R2_SZ) / 2;
    const int G2_Y = 26;

    sAgGemRing5h = ring(card2, R2_SZ, R2_W, COL_AG_GEM);
    lv_obj_set_pos(sAgGemRing5h, G2A_X, G2_Y);

    sAgGemRingWk = ring(card2, R2_IN_SZ, R2_W, COL_AG_GEM2);
    lv_obj_set_pos(sAgGemRingWk, G2A_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + (R2_SZ - R2_IN_SZ) / 2);

    // Center container with 2 compact lines
    lv_obj_t* cBoxA = box(card2, R2_IN_SZ, R2_IN_SZ);
    lv_obj_set_pos(cBoxA, G2A_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + (R2_SZ - R2_IN_SZ) / 2);
    lv_obj_set_flex_flow(cBoxA, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cBoxA, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sAgGemCenter5h = label(cBoxA, F.cap, cText(), "5h --");
    sAgGemCenterWk = label(cBoxA, F.cap, cText3(), "Wo --");

    lv_obj_t* lblGem = caption(card2, "GEMINI", COL_AG_GEM);
    sAgGemCap = lblGem;
    lv_obj_set_width(lblGem, COL_W);
    lv_obj_set_style_text_align(lblGem, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblGem, 16, G2_Y + R2_SZ + 2);

    // --- Sub-group 2B: Claude & GPT Models ---
    const int G2B_X = 16 + COL_W + (COL_W - R2_SZ) / 2;

    sAgCldRing5h = ring(card2, R2_SZ, R2_W, COL_AG_CLD);
    lv_obj_set_pos(sAgCldRing5h, G2B_X, G2_Y);

    sAgCldRingWk = ring(card2, R2_IN_SZ, R2_W, COL_AG_CLD2);
    lv_obj_set_pos(sAgCldRingWk, G2B_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + (R2_SZ - R2_IN_SZ) / 2);

    // Center container with 2 compact lines
    lv_obj_t* cBoxB = box(card2, R2_IN_SZ, R2_IN_SZ);
    lv_obj_set_pos(cBoxB, G2B_X + (R2_SZ - R2_IN_SZ) / 2, G2_Y + (R2_SZ - R2_IN_SZ) / 2);
    lv_obj_set_flex_flow(cBoxB, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cBoxB, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sAgCldCenter5h = label(cBoxB, F.cap, cText(), "5h --");
    sAgCldCenterWk = label(cBoxB, F.cap, cText3(), "Wo --");

    lv_obj_t* lblCld = caption(card2, "CLAUDE / GPT", COL_AG_CLD);
    sAgCldCap = lblCld;
    lv_obj_set_width(lblCld, COL_W);
    lv_obj_set_style_text_align(lblCld, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblCld, 16 + COL_W, G2_Y + R2_SZ + 2);

    // ========================================================================
    // SEKTION 3: GOOGLE CLOUD & GEMINI 4 ARGON
    // ========================================================================
    const int S3_Y = 292;
    lv_obj_t* card3 = card(p, CW, 136);
    lv_obj_set_pos(card3, M, S3_Y);
    sCards[2] = card3;

    lv_obj_t* cap3 = caption(card3, "GOOGLE CLOUD", COL_GC_PRO);
    lv_obj_set_pos(cap3, 16, 6);

    // 3 rings side-by-side for Gemini 1.5 Pro, Flash and Daily Quota
    const int R3_SZ = 48, R3_W = 4;
    const int COL3_W = (CW - 32) / 3; // ~110px
    const int G3_Y = 22;

    // Ring 1: Pro (5h)
    int r1_x = 16 + (COL3_W - R3_SZ) / 2;
    sGcRingPro = ring(card3, R3_SZ, R3_W, COL_GC_PRO);
    lv_obj_set_pos(sGcRingPro, r1_x, G3_Y);
    lv_obj_t* cBox3A = box(card3, R3_SZ, R3_SZ);
    lv_obj_set_pos(cBox3A, r1_x, G3_Y);
    lv_obj_set_flex_flow(cBox3A, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cBox3A, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sGcCenterPro = label(cBox3A, F.cap, cText(), "--");
    lv_obj_t* lblPro = caption(card3, "PRO 5h", cText3());
    sGcCapPro = lblPro;
    lv_obj_set_width(lblPro, COL3_W);
    lv_obj_set_style_text_align(lblPro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblPro, 16, G3_Y + R3_SZ + 2);

    // Ring 2: Flash (Weekly)
    int r2_x = 16 + COL3_W + (COL3_W - R3_SZ) / 2;
    sGcRingFls = ring(card3, R3_SZ, R3_W, COL_GC_FLS);
    lv_obj_set_pos(sGcRingFls, r2_x, G3_Y);
    lv_obj_t* cBox3B = box(card3, R3_SZ, R3_SZ);
    lv_obj_set_pos(cBox3B, r2_x, G3_Y);
    lv_obj_set_flex_flow(cBox3B, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cBox3B, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sGcCenterFls = label(cBox3B, F.cap, cText(), "--");
    lv_obj_t* lblFls = caption(card3, "FLASH Wo", cText3());
    sGcCapFls = lblFls;
    lv_obj_set_width(lblFls, COL3_W);
    lv_obj_set_style_text_align(lblFls, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblFls, 16 + COL3_W, G3_Y + R3_SZ + 2);

    // Ring 3: Daily Quota
    int r3_x = 16 + 2 * COL3_W + (COL3_W - R3_SZ) / 2;
    sGcRingDay = ring(card3, R3_SZ, R3_W, COL_GC_DAY);
    lv_obj_set_pos(sGcRingDay, r3_x, G3_Y);
    lv_obj_t* cBox3C = box(card3, R3_SZ, R3_SZ);
    lv_obj_set_pos(cBox3C, r3_x, G3_Y);
    lv_obj_set_flex_flow(cBox3C, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cBox3C, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sGcCenterDay = label(cBox3C, F.cap, cText(), "--");
    lv_obj_t* lblDay = caption(card3, "TAG", cText3());
    sGcCapDay = lblDay;
    lv_obj_set_width(lblDay, COL3_W);
    lv_obj_set_style_text_align(lblDay, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lblDay, 16 + 2 * COL3_W, G3_Y + R3_SZ + 2);

    // --- Bottom Bar: Gemini 4 Argon Weekly ---
    const int BAR_Y = 98;
    sArgonTxt = caption(card3, "ARGON WOCHE  --% • --", COL_ARGON);
    lv_obj_set_pos(sArgonTxt, 16, BAR_Y);

    sArgonBar = lv_bar_create(card3);
    lv_obj_set_size(sArgonBar, CW - 32, 6);
    lv_obj_set_pos(sArgonBar, 16, BAR_Y + 16);
    lv_bar_set_range(sArgonBar, 0, 100);
    lv_obj_set_style_bg_color(sArgonBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sArgonBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sArgonBar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sArgonBar, COL_ARGON, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sArgonBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sArgonBar, 3, LV_PART_INDICATOR);

    // Prominenter Stale-Alert Banner direkt unter dem Header
    sStaleBanner = lv_obj_create(p);
    lv_obj_set_size(sStaleBanner, CW, 34);
    lv_obj_set_pos(sStaleBanner, M, 56);
    lv_obj_set_style_bg_color(sStaleBanner, lv_color_hex(0x2E1A00), 0);
    lv_obj_set_style_bg_opa(sStaleBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sStaleBanner, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_border_width(sStaleBanner, 2, 0);
    lv_obj_set_style_radius(sStaleBanner, 12, 0);
    lv_obj_set_flex_flow(sStaleBanner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sStaleBanner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sStaleBanner, 8, 0);
    pressable(sStaleBanner);

    lv_obj_t* stIco = label(sStaleBanner, F.icoS, lv_color_hex(0xF59E0B), ICO_INFO);
    sStaleTxt = label(sStaleBanner, F.cap, lv_color_hex(0xFDE68A), "Veraltet • Hotspot für Sync");
    lv_obj_add_event_cb(sStaleBanner, [](lv_event_t*) {
        SystemSettings& s = SettingsStorage::get();
        s.wifiApEnabled = true;
        SettingsStorage::save();
        CompanionServer::requestAp(true);
        DisplayEngine::showToast("Hotspot aktiv", "Handy per WLAN verbinden", 3000, ICO_WIFI);
        haptic(HAPTIC_CLICK);
        nav::markDirty(nav::PG_APP_CONNECT);
        nav::markDirty(nav::PG_CONTROL);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(sStaleBanner, LV_OBJ_FLAG_HIDDEN);

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

