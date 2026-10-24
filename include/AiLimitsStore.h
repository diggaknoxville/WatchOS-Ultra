#pragma once
#include <Arduino.h>
#include <time.h>

// AI Limits data structure holding percentages (0..100) and reset epoch timestamps (seconds).
struct AiLimitsData {
    // Sektion 1: Anthropic (Claude Pro)
    uint8_t  claude_5h_pct;
    uint32_t claude_5h_reset;
    uint8_t  claude_wk_pct;
    uint32_t claude_wk_reset;

    // Sektion 2: Antigravity IDE (Google AI Pro Abo)
    // 2A: Gemini Models
    uint8_t  ag_gemini_5h_pct;
    uint32_t ag_gemini_5h_reset;
    uint8_t  ag_gemini_wk_pct;
    uint32_t ag_gemini_wk_reset;
    // 2B: Claude & GPT Models
    uint8_t  ag_claude_5h_pct;
    uint32_t ag_claude_5h_reset;
    uint8_t  ag_claude_wk_pct;
    uint32_t ag_claude_wk_reset;

    // Sektion 3: Google Cloud
    uint8_t  gc_5h_pct;
    uint32_t gc_5h_reset;
    uint8_t  gc_daily_pct;
    uint32_t gc_daily_reset;
    uint8_t  gc_weekly_pct;
    uint32_t gc_weekly_reset;

    // Sektion 3: Bottom Argon Weekly Bar
    uint8_t  argon_weekly_pct;
    uint32_t argon_weekly_reset;

    // Sync metadata
    uint32_t last_sync_time;   // Unix epoch seconds
    bool     has_data;
};

// Packed binary BLE packet format (< 64 bytes)
struct __attribute__((packed)) AiLimitsBlePacket {
    uint8_t  magic;             // 0xA1
    uint8_t  version;           // 0x01
    uint8_t  claude_5h_pct;
    uint32_t claude_5h_reset;
    uint8_t  claude_wk_pct;
    uint32_t claude_wk_reset;
    uint8_t  ag_gemini_5h_pct;
    uint32_t ag_gemini_5h_reset;
    uint8_t  ag_gemini_wk_pct;
    uint32_t ag_gemini_wk_reset;
    uint8_t  ag_claude_5h_pct;
    uint32_t ag_claude_5h_reset;
    uint8_t  ag_claude_wk_pct;
    uint32_t ag_claude_wk_reset;
    uint8_t  gc_5h_pct;
    uint32_t gc_5h_reset;
    uint8_t  gc_daily_pct;
    uint32_t gc_daily_reset;
    uint8_t  gc_weekly_pct;
    uint32_t gc_weekly_reset;
    uint8_t  argon_weekly_pct;
    uint32_t argon_weekly_reset;
    uint32_t sync_timestamp;
};

class AiLimitsStore {
public:
    static void init();
    static AiLimitsData get();
    static void set(const AiLimitsData& data);
    static bool updateFromBleBinary(const uint8_t* payload, size_t len);
    static bool updateFromJson(const char* jsonStr);
    
    // Helper to format countdown relative to now (e.g. "1h 24m", "3d 12h", "READY")
    static void formatCountdown(char* out, size_t outSize, uint32_t resetEpochSec);
    // Compact format for ring center (e.g. "1:24", "3d")
    static void formatCompactCountdown(char* out, size_t outSize, uint32_t resetEpochSec);
    // Format last sync time string
    static void formatSyncAgo(char* out, size_t outSize, uint32_t syncEpochSec);

    static bool isBleConnected();
    static void setBleConnected(bool connected);

    static bool isShowUsed();
    static void setShowUsed(bool showUsed);
    static void toggleShowUsed();

private:
    static AiLimitsData currentData;
    static bool bleConnected;
    static bool showUsed;
    static void saveToNvs();
    static void loadFromNvs();
};

