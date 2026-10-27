#include "AiLimitsStore.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

// NVS layout (namespace "ailimits"):
//   "data_v2"  : AiLimitsNvsBlob { uint8_t version = 2; AiLimitsData data; }
//   "data"     : legacy v1 blob (raw AiLimitsData), migrated once to data_v2
//   "show_used": bool
static constexpr uint8_t NVS_BLOB_VERSION = 2;

struct __attribute__((packed)) AiLimitsNvsBlob {
    uint8_t version;
    AiLimitsData data;
};

AiLimitsData AiLimitsStore::currentData = {};   // all zero, has_data = false
bool AiLimitsStore::bleConnected = false;
bool AiLimitsStore::showUsed = false;
static portMUX_TYPE sLimitsMux = portMUX_INITIALIZER_UNLOCKED;

static inline uint8_t clampPct(uint32_t v) { return v > 100 ? 100 : (uint8_t)v; }

static void clampData(AiLimitsData& d) {
    d.claude_5h_pct     = clampPct(d.claude_5h_pct);
    d.claude_wk_pct     = clampPct(d.claude_wk_pct);
    d.ag_gemini_5h_pct  = clampPct(d.ag_gemini_5h_pct);
    d.ag_gemini_wk_pct  = clampPct(d.ag_gemini_wk_pct);
    d.ag_claude_5h_pct  = clampPct(d.ag_claude_5h_pct);
    d.ag_claude_wk_pct  = clampPct(d.ag_claude_wk_pct);
    d.gc_5h_pct         = clampPct(d.gc_5h_pct);
    d.gc_daily_pct      = clampPct(d.gc_daily_pct);
    d.gc_weekly_pct     = clampPct(d.gc_weekly_pct);
    d.argon_weekly_pct  = clampPct(d.argon_weekly_pct);
}

void AiLimitsStore::init() {
    loadFromNvs();
}

AiLimitsData AiLimitsStore::get() {
    portENTER_CRITICAL(&sLimitsMux);
    time_t now = time(nullptr);
    // Dynamic rollover: if reset timestamp has elapsed, quota is refreshed to 100%
    if (currentData.has_data && now > 1700000000) {
        if (currentData.claude_5h_reset && (uint32_t)now >= currentData.claude_5h_reset) currentData.claude_5h_pct = 100;
        if (currentData.claude_wk_reset && (uint32_t)now >= currentData.claude_wk_reset) currentData.claude_wk_pct = 100;
        if (currentData.ag_gemini_5h_reset && (uint32_t)now >= currentData.ag_gemini_5h_reset) currentData.ag_gemini_5h_pct = 100;
        if (currentData.ag_gemini_wk_reset && (uint32_t)now >= currentData.ag_gemini_wk_reset) currentData.ag_gemini_wk_pct = 100;
        if (currentData.ag_claude_5h_reset && (uint32_t)now >= currentData.ag_claude_5h_reset) currentData.ag_claude_5h_pct = 100;
        if (currentData.ag_claude_wk_reset && (uint32_t)now >= currentData.ag_claude_wk_reset) currentData.ag_claude_wk_pct = 100;
        if (currentData.gc_5h_reset && (uint32_t)now >= currentData.gc_5h_reset) currentData.gc_5h_pct = 100;
        if (currentData.gc_daily_reset && (uint32_t)now >= currentData.gc_daily_reset) currentData.gc_daily_pct = 100;
        if (currentData.gc_weekly_reset && (uint32_t)now >= currentData.gc_weekly_reset) currentData.gc_weekly_pct = 100;
        if (currentData.argon_weekly_reset && (uint32_t)now >= currentData.argon_weekly_reset) currentData.argon_weekly_pct = 100;
    }
    AiLimitsData copy = currentData;
    portEXIT_CRITICAL(&sLimitsMux);
    return copy;
}

void AiLimitsStore::set(const AiLimitsData& data) {
    AiLimitsData d = data;
    clampData(d);
    d.has_data = true;
    portENTER_CRITICAL(&sLimitsMux);
    currentData = d;
    portEXIT_CRITICAL(&sLimitsMux);
    saveToNvs();   // caller runs on the background task (BLE loop / web server)
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

static inline uint32_t rd_u32le(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;   // ESP32 is little-endian
}

bool AiLimitsStore::updateFromBleBinary(const uint8_t* payload, size_t len) {
    if (!payload || len < sizeof(AiLimitsBlePacket)) return false;
    if (payload[0] != 0xA1) return false;
    if (payload[1] != 0x01) {
        Serial.printf("[AILIMITS] unsupported packet version %u\n", (unsigned)payload[1]);
        return false;
    }
    // The packet is packed (unaligned): read field by field via memcpy.
    const uint8_t* p = payload + 2;
    AiLimitsData d = {};
    d.claude_5h_pct       = p[0];  d.claude_5h_reset     = rd_u32le(p + 1);
    d.claude_wk_pct       = p[5];  d.claude_wk_reset     = rd_u32le(p + 6);
    d.ag_gemini_5h_pct    = p[10]; d.ag_gemini_5h_reset  = rd_u32le(p + 11);
    d.ag_gemini_wk_pct    = p[15]; d.ag_gemini_wk_reset  = rd_u32le(p + 16);
    d.ag_claude_5h_pct    = p[20]; d.ag_claude_5h_reset  = rd_u32le(p + 21);
    d.ag_claude_wk_pct    = p[25]; d.ag_claude_wk_reset  = rd_u32le(p + 26);
    d.gc_5h_pct           = p[30]; d.gc_5h_reset         = rd_u32le(p + 31);
    d.gc_daily_pct        = p[35]; d.gc_daily_reset      = rd_u32le(p + 36);
    d.gc_weekly_pct       = p[40]; d.gc_weekly_reset     = rd_u32le(p + 41);
    d.argon_weekly_pct    = p[45]; d.argon_weekly_reset  = rd_u32le(p + 46);
    uint32_t syncTs       = rd_u32le(p + 50);

    d.last_sync_time = syncTs ? syncTs : (uint32_t)time(nullptr);
    d.has_data = true;
    set(d);
    return true;
}

// Minimal JSON lookup: matches "key" only at a key position (followed by ':').
static uint32_t parseJsonUint(const char* json, const char* key, uint32_t defVal = 0) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    size_t slen = strlen(search);
    const char* p = json;
    while ((p = strstr(p, search)) != nullptr) {
        const char* q = p + slen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            return (uint32_t)strtoul(q, nullptr, 10);
        }
        p += slen;
    }
    return defVal;
}

bool AiLimitsStore::updateFromJson(const char* jsonStr) {
    if (!jsonStr || jsonStr[0] != '{') return false;

    AiLimitsData d = get();
    d.claude_5h_pct        = clampPct(parseJsonUint(jsonStr, "c_5h_p", parseJsonUint(jsonStr, "claude_5h_pct", d.claude_5h_pct)));
    d.claude_5h_reset      = parseJsonUint(jsonStr, "c_5h_r", parseJsonUint(jsonStr, "claude_5h_reset", d.claude_5h_reset));
    d.claude_wk_pct        = clampPct(parseJsonUint(jsonStr, "c_wk_p", parseJsonUint(jsonStr, "claude_wk_pct", d.claude_wk_pct)));
    d.claude_wk_reset      = parseJsonUint(jsonStr, "c_wk_r", parseJsonUint(jsonStr, "claude_wk_reset", d.claude_wk_reset));

    d.ag_gemini_5h_pct     = clampPct(parseJsonUint(jsonStr, "ag_g_5h_p", parseJsonUint(jsonStr, "ag_gemini_5h_pct", d.ag_gemini_5h_pct)));
    d.ag_gemini_5h_reset   = parseJsonUint(jsonStr, "ag_g_5h_r", parseJsonUint(jsonStr, "ag_gemini_5h_reset", d.ag_gemini_5h_reset));
    d.ag_gemini_wk_pct     = clampPct(parseJsonUint(jsonStr, "ag_g_wk_p", parseJsonUint(jsonStr, "ag_gemini_wk_pct", d.ag_gemini_wk_pct)));
    d.ag_gemini_wk_reset   = parseJsonUint(jsonStr, "ag_g_wk_r", parseJsonUint(jsonStr, "ag_gemini_wk_reset", d.ag_gemini_wk_reset));

    d.ag_claude_5h_pct     = clampPct(parseJsonUint(jsonStr, "ag_c_5h_p", parseJsonUint(jsonStr, "ag_claude_5h_pct", d.ag_claude_5h_pct)));
    d.ag_claude_5h_reset   = parseJsonUint(jsonStr, "ag_c_5h_r", parseJsonUint(jsonStr, "ag_claude_5h_reset", d.ag_claude_5h_reset));
    d.ag_claude_wk_pct     = clampPct(parseJsonUint(jsonStr, "ag_c_wk_p", parseJsonUint(jsonStr, "ag_claude_wk_pct", d.ag_claude_wk_pct)));
    d.ag_claude_wk_reset   = parseJsonUint(jsonStr, "ag_c_wk_r", parseJsonUint(jsonStr, "ag_claude_wk_reset", d.ag_claude_wk_reset));

    d.gc_5h_pct            = clampPct(parseJsonUint(jsonStr, "gc_5h_p", parseJsonUint(jsonStr, "gc_5h_pct", d.gc_5h_pct)));
    d.gc_5h_reset          = parseJsonUint(jsonStr, "gc_5h_r", parseJsonUint(jsonStr, "gc_5h_reset", d.gc_5h_reset));
    d.gc_daily_pct         = clampPct(parseJsonUint(jsonStr, "gc_d_p", parseJsonUint(jsonStr, "gc_daily_pct", d.gc_daily_pct)));
    d.gc_daily_reset       = parseJsonUint(jsonStr, "gc_d_r", parseJsonUint(jsonStr, "gc_daily_reset", d.gc_daily_reset));
    d.gc_weekly_pct        = clampPct(parseJsonUint(jsonStr, "gc_wk_p", parseJsonUint(jsonStr, "gc_weekly_pct", d.gc_weekly_pct)));
    d.gc_weekly_reset      = parseJsonUint(jsonStr, "gc_wk_r", parseJsonUint(jsonStr, "gc_weekly_reset", d.gc_weekly_reset));

    d.argon_weekly_pct     = clampPct(parseJsonUint(jsonStr, "arg_p", parseJsonUint(jsonStr, "argon_weekly_pct", d.argon_weekly_pct)));
    d.argon_weekly_reset   = parseJsonUint(jsonStr, "arg_r", parseJsonUint(jsonStr, "argon_weekly_reset", d.argon_weekly_reset));

    uint32_t sync = parseJsonUint(jsonStr, "sync", parseJsonUint(jsonStr, "sync_timestamp", 0));
    d.last_sync_time = sync ? sync : (uint32_t)time(nullptr);
    d.has_data = true;

    set(d);
    return true;
}

void AiLimitsStore::saveToNvs() {
    AiLimitsNvsBlob blob;
    blob.version = NVS_BLOB_VERSION;
    blob.data = get();
    Preferences p;
    if (!p.begin("ailimits", false)) return;
    p.putBytes("data_v2", &blob, sizeof(blob));
    p.end();
}

void AiLimitsStore::loadFromNvs() {
    Preferences p;
    if (!p.begin("ailimits", false)) return;
    bool loaded = false;
    if (p.isKey("data_v2")) {
        AiLimitsNvsBlob blob;
        size_t read = p.getBytes("data_v2", &blob, sizeof(blob));
        if (read == sizeof(blob) && blob.version == NVS_BLOB_VERSION) {
            AiLimitsData d;                       // copy out of the packed blob first
            memcpy(&d, &blob.data, sizeof(d));
            clampData(d);
            portENTER_CRITICAL(&sLimitsMux);
            currentData = d;
            portEXIT_CRITICAL(&sLimitsMux);
            loaded = true;
        }
    } else if (p.isKey("data")) {
        // Legacy v1 blob (raw struct). Only accept it when the size matches,
        // then rewrite as v2 and drop the old key.
        AiLimitsData d;
        size_t read = p.getBytes("data", &d, sizeof(AiLimitsData));
        if (read == sizeof(AiLimitsData) && d.has_data) {
            clampData(d);
            portENTER_CRITICAL(&sLimitsMux);
            currentData = d;
            portEXIT_CRITICAL(&sLimitsMux);
            loaded = true;
            AiLimitsNvsBlob blob;
            blob.version = NVS_BLOB_VERSION;
            blob.data = d;
            p.putBytes("data_v2", &blob, sizeof(blob));
        }
        p.remove("data");
    }
    if (p.isKey("show_used")) {
        showUsed = p.getBool("show_used", false);
    }
    p.end();

    if (!loaded) {
        time_t now = time(nullptr);
        uint32_t baseTime = (now > 1700000000) ? (uint32_t)now : 1730000000;
        AiLimitsData d = {};
        d.claude_5h_pct = 100;
        d.claude_5h_reset = baseTime + 5 * 3600;
        d.claude_wk_pct = 100;
        d.claude_wk_reset = baseTime + 7 * 86400;

        d.ag_gemini_5h_pct = 100;
        d.ag_gemini_5h_reset = baseTime + 5 * 3600;
        d.ag_gemini_wk_pct = 100;
        d.ag_gemini_wk_reset = baseTime + 7 * 86400;

        d.ag_claude_5h_pct = 100;
        d.ag_claude_5h_reset = baseTime + 5 * 3600;
        d.ag_claude_wk_pct = 100;
        d.ag_claude_wk_reset = baseTime + 7 * 86400;

        d.gc_5h_pct = 100;
        d.gc_5h_reset = baseTime + 5 * 3600;
        d.gc_daily_pct = 100;
        d.gc_daily_reset = baseTime + 86400;
        d.gc_weekly_pct = 100;
        d.gc_weekly_reset = baseTime + 7 * 86400;

        d.argon_weekly_pct = 100;
        d.argon_weekly_reset = baseTime + 7 * 86400;

        d.last_sync_time = baseTime;
        d.has_data = true;

        portENTER_CRITICAL(&sLimitsMux);
        currentData = d;
        portEXIT_CRITICAL(&sLimitsMux);
    }

    Serial.printf("[AILIMITS] %s\n", loaded ? "cached limits loaded" : "default starting limits loaded");
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
