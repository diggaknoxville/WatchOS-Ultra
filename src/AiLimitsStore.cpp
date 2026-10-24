#include "AiLimitsStore.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

AiLimitsData AiLimitsStore::currentData = {
    // defaults: sample data until first BLE sync
    .claude_5h_pct = 78,
    .claude_5h_reset = 0,
    .claude_wk_pct = 42,
    .claude_wk_reset = 0,

    .ag_gemini_5h_pct = 85,
    .ag_gemini_5h_reset = 0,
    .ag_gemini_wk_pct = 60,
    .ag_gemini_wk_reset = 0,

    .ag_claude_5h_pct = 65,
    .ag_claude_5h_reset = 0,
    .ag_claude_wk_pct = 35,
    .ag_claude_wk_reset = 0,

    .gc_5h_pct = 92,
    .gc_5h_reset = 0,
    .gc_daily_pct = 88,
    .gc_daily_reset = 0,
    .gc_weekly_pct = 74,
    .gc_weekly_reset = 0,

    .argon_weekly_pct = 70,
    .argon_weekly_reset = 0,

    .last_sync_time = 0,
    .has_data = false
};

bool AiLimitsStore::bleConnected = false;
bool AiLimitsStore::showUsed = false;
static portMUX_TYPE sLimitsMux = portMUX_INITIALIZER_UNLOCKED;

void AiLimitsStore::init() {
    loadFromNvs();
}

AiLimitsData AiLimitsStore::get() {
    portENTER_CRITICAL(&sLimitsMux);
    AiLimitsData copy = currentData;
    portEXIT_CRITICAL(&sLimitsMux);
    return copy;
}

void AiLimitsStore::set(const AiLimitsData& data) {
    portENTER_CRITICAL(&sLimitsMux);
    currentData = data;
    currentData.has_data = true;
    portEXIT_CRITICAL(&sLimitsMux);
    saveToNvs();
}

bool AiLimitsStore::isBleConnected() {
    return bleConnected;
}

void AiLimitsStore::setBleConnected(bool connected) {
    bleConnected = connected;
}

bool AiLimitsStore::isShowUsed() {
    return showUsed;
}

void AiLimitsStore::setShowUsed(bool used) {
    showUsed = used;
    Preferences p;
    if (p.begin("ailimits", false)) {
        p.putBool("show_used", showUsed);
        p.end();
    }
}

void AiLimitsStore::toggleShowUsed() {
    setShowUsed(!showUsed);
}

bool AiLimitsStore::updateFromBleBinary(const uint8_t* payload, size_t len) {
    if (!payload || len < sizeof(AiLimitsBlePacket)) return false;
    const AiLimitsBlePacket* pkt = reinterpret_cast<const AiLimitsBlePacket*>(payload);
    if (pkt->magic != 0xA1) return false;

    AiLimitsData d;
    d.claude_5h_pct = pkt->claude_5h_pct;
    d.claude_5h_reset = pkt->claude_5h_reset;
    d.claude_wk_pct = pkt->claude_wk_pct;
    d.claude_wk_reset = pkt->claude_wk_reset;

    d.ag_gemini_5h_pct = pkt->ag_gemini_5h_pct;
    d.ag_gemini_5h_reset = pkt->ag_gemini_5h_reset;
    d.ag_gemini_wk_pct = pkt->ag_gemini_wk_pct;
    d.ag_gemini_wk_reset = pkt->ag_gemini_wk_reset;

    d.ag_claude_5h_pct = pkt->ag_claude_5h_pct;
    d.ag_claude_5h_reset = pkt->ag_claude_5h_reset;
    d.ag_claude_wk_pct = pkt->ag_claude_wk_pct;
    d.ag_claude_wk_reset = pkt->ag_claude_wk_reset;

    d.gc_5h_pct = pkt->gc_5h_pct;
    d.gc_5h_reset = pkt->gc_5h_reset;
    d.gc_daily_pct = pkt->gc_daily_pct;
    d.gc_daily_reset = pkt->gc_daily_reset;
    d.gc_weekly_pct = pkt->gc_weekly_pct;
    d.gc_weekly_reset = pkt->gc_weekly_reset;

    d.argon_weekly_pct = pkt->argon_weekly_pct;
    d.argon_weekly_reset = pkt->argon_weekly_reset;

    d.last_sync_time = pkt->sync_timestamp ? pkt->sync_timestamp : (uint32_t)time(nullptr);
    d.has_data = true;

    set(d);
    return true;
}

// Simple JSON parser helper for keys: "key": value
static uint32_t parseJsonUint(const char* json, const char* key, uint32_t defVal = 0) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    if (!p) {
        snprintf(search, sizeof(search), "\"%s\" :", key);
        p = strstr(json, search);
    }
    if (!p) return defVal;
    p = strchr(p, ':');
    if (!p) return defVal;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return (uint32_t)strtoul(p, nullptr, 10);
}

bool AiLimitsStore::updateFromJson(const char* jsonStr) {
    if (!jsonStr || jsonStr[0] != '{') return false;

    AiLimitsData d = get();
    d.claude_5h_pct        = (uint8_t)parseJsonUint(jsonStr, "c_5h_p", d.claude_5h_pct);
    d.claude_5h_reset      = parseJsonUint(jsonStr, "c_5h_r", d.claude_5h_reset);
    d.claude_wk_pct        = (uint8_t)parseJsonUint(jsonStr, "c_wk_p", d.claude_wk_pct);
    d.claude_wk_reset      = parseJsonUint(jsonStr, "c_wk_r", d.claude_wk_reset);

    d.ag_gemini_5h_pct     = (uint8_t)parseJsonUint(jsonStr, "ag_g_5h_p", d.ag_gemini_5h_pct);
    d.ag_gemini_5h_reset   = parseJsonUint(jsonStr, "ag_g_5h_r", d.ag_gemini_5h_reset);
    d.ag_gemini_wk_pct     = (uint8_t)parseJsonUint(jsonStr, "ag_g_wk_p", d.ag_gemini_wk_pct);
    d.ag_gemini_wk_reset   = parseJsonUint(jsonStr, "ag_g_wk_r", d.ag_gemini_wk_reset);

    d.ag_claude_5h_pct     = (uint8_t)parseJsonUint(jsonStr, "ag_c_5h_p", d.ag_claude_5h_pct);
    d.ag_claude_5h_reset   = parseJsonUint(jsonStr, "ag_c_5h_r", d.ag_claude_5h_reset);
    d.ag_claude_wk_pct     = (uint8_t)parseJsonUint(jsonStr, "ag_c_wk_p", d.ag_claude_wk_pct);
    d.ag_claude_wk_reset   = parseJsonUint(jsonStr, "ag_c_wk_r", d.ag_claude_wk_reset);

    d.gc_5h_pct            = (uint8_t)parseJsonUint(jsonStr, "gc_5h_p", d.gc_5h_pct);
    d.gc_5h_reset          = parseJsonUint(jsonStr, "gc_5h_r", d.gc_5h_reset);
    d.gc_daily_pct         = (uint8_t)parseJsonUint(jsonStr, "gc_d_p", d.gc_daily_pct);
    d.gc_daily_reset       = parseJsonUint(jsonStr, "gc_d_r", d.gc_daily_reset);
    d.gc_weekly_pct        = (uint8_t)parseJsonUint(jsonStr, "gc_wk_p", d.gc_weekly_pct);
    d.gc_weekly_reset      = parseJsonUint(jsonStr, "gc_wk_r", d.gc_weekly_reset);

    d.argon_weekly_pct     = (uint8_t)parseJsonUint(jsonStr, "arg_p", d.argon_weekly_pct);
    d.argon_weekly_reset   = parseJsonUint(jsonStr, "arg_r", d.argon_weekly_reset);

    uint32_t sync = parseJsonUint(jsonStr, "sync", 0);
    d.last_sync_time = sync ? sync : (uint32_t)time(nullptr);
    d.has_data = true;

    set(d);
    return true;
}

void AiLimitsStore::saveToNvs() {
    Preferences p;
    if (!p.begin("ailimits", false)) return;
    p.putBytes("data", &currentData, sizeof(AiLimitsData));
    p.end();
}

void AiLimitsStore::loadFromNvs() {
    Preferences p;
    if (!p.begin("ailimits", true)) return;
    if (p.isKey("data")) {
        AiLimitsData d;
        size_t read = p.getBytes("data", &d, sizeof(AiLimitsData));
        if (read == sizeof(AiLimitsData)) {
            portENTER_CRITICAL(&sLimitsMux);
            currentData = d;
            portEXIT_CRITICAL(&sLimitsMux);
        }
    }
    if (p.isKey("show_used")) {
        showUsed = p.getBool("show_used", false);
    }
    p.end();
}

void AiLimitsStore::formatCountdown(char* out, size_t outSize, uint32_t resetEpochSec) {
    if (!out || outSize == 0) return;
    if (resetEpochSec == 0) {
        snprintf(out, outSize, "--");
        return;
    }
    time_t now = time(nullptr);
    if (resetEpochSec <= (uint32_t)now) {
        snprintf(out, outSize, "READY");
        return;
    }
    uint32_t diff = resetEpochSec - (uint32_t)now;
    if (diff < 60) {
        snprintf(out, outSize, "<1m");
    } else if (diff < 3600) {
        snprintf(out, outSize, "%um", (unsigned)(diff / 60));
    } else if (diff < 86400) {
        unsigned h = diff / 3600;
        unsigned m = (diff % 3600) / 60;
        snprintf(out, outSize, "%uh %02um", h, m);
    } else {
        unsigned d = diff / 86400;
        unsigned h = (diff % 86400) / 3600;
        snprintf(out, outSize, "%ud %uh", d, h);
    }
}

void AiLimitsStore::formatCompactCountdown(char* out, size_t outSize, uint32_t resetEpochSec) {
    if (!out || outSize == 0) return;
    if (resetEpochSec == 0) {
        snprintf(out, outSize, "--");
        return;
    }
    time_t now = time(nullptr);
    if (resetEpochSec <= (uint32_t)now) {
        snprintf(out, outSize, "100%%");
        return;
    }
    uint32_t diff = resetEpochSec - (uint32_t)now;
    if (diff < 60) {
        snprintf(out, outSize, "<1m");
    } else if (diff < 3600) {
        snprintf(out, outSize, "%um", (unsigned)(diff / 60));
    } else if (diff < 86400) {
        unsigned h = diff / 3600;
        unsigned m = (diff % 3600) / 60;
        snprintf(out, outSize, "%u:%02u", h, m);
    } else {
        unsigned d = diff / 86400;
        snprintf(out, outSize, "%ud", d);
    }
}

void AiLimitsStore::formatSyncAgo(char* out, size_t outSize, uint32_t syncEpochSec) {
    if (!out || outSize == 0) return;
    if (syncEpochSec == 0) {
        snprintf(out, outSize, "NO SYNC");
        return;
    }
    time_t now = time(nullptr);
    if ((uint32_t)now < syncEpochSec) {
        snprintf(out, outSize, "JUST NOW");
        return;
    }
    uint32_t diff = (uint32_t)now - syncEpochSec;
    if (diff < 60) {
        snprintf(out, outSize, "JUST NOW");
    } else if (diff < 3600) {
        snprintf(out, outSize, "%um AGO", (unsigned)(diff / 60));
    } else if (diff < 86400) {
        snprintf(out, outSize, "%uh AGO", (unsigned)(diff / 3600));
    } else {
        snprintf(out, outSize, "%ud AGO", (unsigned)(diff / 86400));
    }
}

