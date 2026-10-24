#pragma once
#include <Arduino.h>

// ==============================================================================
//  KRONOS OS - LilyGo T-Watch Ultra
//  All pin assignments come from the board variant (lilygo_twatch_ultra
//  pins_arduino.h) and are driven through LilyGoLib's `instance`. Do not
//  redefine hardware pins here.
// ==============================================================================

// System Constants
#define OS_NAME             "KRONOS OS"
#define OS_VERSION          "KRONOS 4.0"
#define OS_BUILD            "2026.10"
#define AP_SSID_PREFIX      "VORTEX-WATCH"
#define AP_PASSWORD         "vortex2026"
#define HTTP_PORT           80

// Vibrant Cyberpunk Color Palette (RGB565)
#define COLOR_BG            0x0000   // Pitch Black for OLED-like power saving
#define COLOR_SURFACE       0x10A2   // Dark Charcoal Surface
#define COLOR_BORDER        0x2965   // Muted Tech Grey
#define COLOR_TEXT_MUTED    0x6B6D   // Subdued Silver
#define COLOR_TEXT_WHITE    0xFFFF   // Crisp White

#define THEME_CYBER_CYAN    0x07FF   // Neon Cyber Cyan
#define THEME_ELECTRIC_LIME 0x07E0   // Toxic Matrix Green
#define THEME_NEON_AMBER    0xFD20   // Sunset Amber
#define THEME_MAGENTA_PULSE 0xF81F   // Hot Magenta
#define THEME_CRIMSON_WAR   0xF800   // Tactical Red
#define THEME_STEALTH_BLUE  0x3BFF   // Stealth Plasma Blue

enum WatchFaceId {
    FACE_MODULAR = 0,     // digital hero time + 3 complications
    FACE_CHRONO  = 1,     // analog chronometer dial (lv_scale)
    FACE_MINIMAL = 2,     // stacked oversized numerals
    FACE_COUNT   = 3
};

enum AppId {
    APP_WATCHFACE = 0,
    APP_QUICK_SETTINGS = 1,
    APP_FITNESS = 2,
    APP_LORA_MESH = 3,
    APP_AUDIO_SYNTH = 4,
    APP_STOPWATCH = 5,
    APP_SENSORS = 6,
    APP_PHONE_SYNC = 7,
    APP_COUNT = 8
};

enum PowerState {
    POWER_ACTIVE = 0,   // Full 240MHz, 60fps, high brightness
    POWER_DIM = 1,      // 80MHz, dimmed brightness
    POWER_AOD = 2,      // Low-power ambient clock face
    POWER_SLEEP = 3     // Display off, CPU light sleep, wrist wake
};

struct SystemSettings {
    uint8_t watchfaceIndex;
    uint16_t accentColor;
    uint8_t brightness;         // 10 - 255
    uint16_t autoSleepSec;      // 10, 15, 30, 60
    bool wristWakeEnabled;
    bool hapticEnabled;
    bool aodEnabled;
    bool soundEnabled;
    bool wifiApEnabled;
    bool loraBeaconEnabled;
    int32_t timezoneOffsetSec;  // e.g. 7200 for UTC+2 (informational / fallback)
    char tzPosix[48];           // POSIX TZ rule incl. DST, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
    uint32_t dailyStepGoal;
    char customTitle[32];
};

