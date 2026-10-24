#pragma once
#include <Arduino.h>
#include <time.h>

// Persistent notification history (newest first), shown in the notification
// centre. Thread-safe: producers run on the background task, the UI reads.
struct NotificationItem {
    char title[40];
    char body[112];
    char icon[4];        // LV_SYMBOL_* (UTF-8, 3 bytes) or empty
    uint32_t epoch;      // UTC
    bool unread;
};

namespace NotificationStore {
    void begin();
    void add(const char* title, const char* body, const char* icon);
    uint8_t count();
    bool get(uint8_t index, NotificationItem& out);   // 0 = newest
    uint8_t unread();
    void markAllRead();
    void clear();
    uint32_t revision();                               // bumps on every change
}
