#pragma once
#include "Config.h"
#include <lvgl.h>

// ============================================================================
//  DisplayEngine - facade of the WatchOS Ultra UI (LVGL 9 + compositor +
//  navigator). Builds all pages, owns the global overlays (banner, always-on
//  display, torch, OTA confirm dialog) and wires hardware events into the
//  navigator. Only the thread-safe entry points may be called from other tasks.
// ============================================================================

class DisplayEngine {
public:
    static bool init();                 // UI task
    static void update(unsigned long now);
    static bool isReady();

    // thread-safe: banner + stored in the notification history
    static void showNotification(const String& title, const String& msg,
                                 uint32_t durationMs = 4500, const char* icon = nullptr);
    // thread-safe: banner only, NOT stored. For UI confirmations (toggles, face change ...)
    static void showToast(const char* title, const char* msg,
                          uint32_t durationMs = 2000, const char* icon = nullptr);

    // UI task
    static void setTorch(bool on);
    static bool torchOn();
    static bool isOverlayBlockingMirror();
    static void nextWatchface();

    // "Nicht stören": suppresses notification banners and their haptics.
    // Runtime state only (no SystemSettings field yet); toasts still show.
    static void setDoNotDisturb(bool on);
    static bool doNotDisturb();
};
