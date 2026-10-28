#pragma once
#include <Arduino.h>

// ==============================================================================
//  WatchOS Ultra - LilyGo T-Watch Ultra
//  All pin assignments come from the board variant (lilygo_twatch_ultra
//  pins_arduino.h) and are driven through LilyGoLib's `instance`. Do not
//  redefine hardware pins here.
//  Internal identifiers (Kronos* classes, kronos_* NVS namespaces) are kept
//  unchanged so stored user data survives the rename.
// ==============================================================================

// System Constants
#define OS_NAME             "WatchOS Ultra"
#define OS_VERSION          "1.4.1"
#define OS_BUILD            "2026.10c"
#define AP_SSID_PREFIX      "WATCHOS-ULTRA"
#define HTTP_PORT           80
// The WiFi AP password is per-device (CompanionServer::getApPassword()).

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
    FACE_MODULAR             = 0,  // digital hero time + 3 complications
    FACE_CHRONO              = 1,  // analog chronometer dial (lv_scale)
    FACE_MINIMAL             = 2,  // stacked oversized numerals
    FACE_ULTRA_WAYFINDER     = 3,  // Apple Watch Ultra Wayfinder compass & adventure dial
    FACE_SKELETON_TOURBILLON = 4,  // Haute Horlogerie skeleton dial with animated tourbillon
    FACE_CASIO               = 5,  // Retro Casio DW-5600 Illuminator LCD
    FACE_BAUHAUS             = 6,  // Max Bill minimalist analog dial
    FACE_CYBERPUNK           = 7,  // Cyberpunk HUD matrix terminal
    FACE_WORDCLOCK           = 8,  // German typographic word clock ("ES IST...")
    FACE_NEON_HORIZON        = 9,  // Synthwave 80s sunset perspective grid
    FACE_PILOT_FLIEGER       = 10, // B-Uhr Aviator cockpit dial
    FACE_DIVER_PRO           = 11, // Submariner 300M Diver rotating bezel
    FACE_RADAR_TACTICAL      = 12, // Tactical military sonar radar
    FACE_LIFETIME            = 13, // Lifetime / Year / Month / Day progress
    FACE_COUNT               = 14
};

enum PowerState {
    POWER_ACTIVE = 0,   // Full brightness, UI at full frame rate
    POWER_DIM = 1,      // Dimmed brightness
    POWER_AOD = 2,      // Low-power ambient clock face
    POWER_SLEEP = 3     // Display off, UI task throttled, wrist wake
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
    bool bleEnabled;
    bool gpsEnabled;
    bool loraEnabled;
    bool loraBeaconEnabled;
    int32_t timezoneOffsetSec;  // e.g. 7200 for UTC+2 (informational / fallback)
    char tzPosix[48];           // POSIX TZ rule incl. DST, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
    uint32_t dailyStepGoal;
    char customTitle[32];
    bool timeFormat12h;         // false = 24h (default), true = 12h (AM/PM)
    // Energiesparmodus: radios + AOD off, light sleep while the display is off.
    // saverRestore remembers what the mode switched off (bit0 BLE, bit1 LoRa,
    // bit2 AOD) so turning it off restores the previous state.
    bool powerSaver;
    uint8_t saverRestore;
};
