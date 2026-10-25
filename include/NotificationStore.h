#pragma once
#include <Arduino.h>
#include <time.h>

// Persistent notification history (newest first), shown in the notification
// centre. Thread-safe: add()/markAllRead()/clear() may be called from any task
// (they only copy under a spinlock and mark slots dirty); loop() runs on the
// background task and writes the dirty slots to NVS at most every 3 s
// (namespace "kronos_ntf", keys "n0".."n19", "head", "count").
struct NotificationItem {
    char title[40];
    char body[112];
    char icon[4];        // ICO_* / LV_SYMBOL_* (UTF-8, 3 bytes) or empty
    uint32_t epoch;      // UTC
    bool unread;
};

namespace NotificationStore {
    void begin();                                      // load from NVS (migrates the legacy single blob)
    void add(const char* title, const char* body, const char* icon);   // non-blocking
    void loop(unsigned long now);                      // bg task: deferred persistence
    uint8_t count();
    bool get(uint8_t index, NotificationItem& out);   // 0 = newest
    uint8_t unread();
    void markAllRead();
    void clear();
    uint32_t revision();                               // bumps on every change
}
