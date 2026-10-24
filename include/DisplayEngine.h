#pragma once
#include "Config.h"
#include <lvgl.h>

// ============================================================================
//  DisplayEngine - facade of the KRONOS UI (LVGL 9 + compositor + navigator).
//  Builds all pages, owns the global overlays (notification banner, always-on
//  display, torch) and wires hardware events into the navigator.
//  Only the thread-safe entry points may be called from other tasks.
// ============================================================================

class DisplayEngine {
public:
    static bool init();                 // UI task
    static void update(unsigned long now);

    // thread-safe
    static void showNotification(const String& title, const String& msg,
                                 uint32_t durationMs = 4500, const char* icon = nullptr);

    // UI task
    static void setTorch(bool on);
    static bool torchOn();

    // compatibility with older callers
    static void setApp(AppId app);
    static AppId getCurrentApp();
    static void nextApp() {}
    static void prevApp() {}
    static void nextWatchface();
};
