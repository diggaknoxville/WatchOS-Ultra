#include "NotificationStore.h"
#include <Preferences.h>

namespace NotificationStore {

static const uint8_t CAP = 20;
static NotificationItem sItems[CAP];      // ring buffer, newest at sHead-1
static uint8_t sHead = 0, sCount = 0;
static volatile uint32_t sRev = 1;
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t sPersistLock = nullptr;

static void persist() {
    // copy under the spinlock, write flash outside of it
    static NotificationItem copy[CAP];
    uint8_t head, count;
    if (sPersistLock) xSemaphoreTake(sPersistLock, portMAX_DELAY);   // guards `copy` + flash
    portENTER_CRITICAL(&sMux);
    memcpy(copy, sItems, sizeof(copy));
    head = sHead;
    count = sCount;
    portEXIT_CRITICAL(&sMux);
    Preferences p;
    if (p.begin("kronos_ntf", false)) {
        p.putBytes("items", copy, sizeof(copy));
        p.putUChar("head", head);
        p.putUChar("count", count);
        p.end();
    }
    if (sPersistLock) xSemaphoreGive(sPersistLock);
}

void begin() {
    sPersistLock = xSemaphoreCreateMutex();
    Preferences p;
    if (p.begin("kronos_ntf", false)) {
        if (p.getBytesLength("items") == sizeof(sItems)) p.getBytes("items", sItems, sizeof(sItems));
        sHead = p.getUChar("head", 0) % CAP;
        sCount = min<uint8_t>(p.getUChar("count", 0), CAP);
        p.end();
    }
}

void add(const char* title, const char* body, const char* icon) {
    portENTER_CRITICAL(&sMux);
    NotificationItem& it = sItems[sHead];
    strncpy(it.title, title ? title : "", sizeof(it.title) - 1);
    it.title[sizeof(it.title) - 1] = 0;
    strncpy(it.body, body ? body : "", sizeof(it.body) - 1);
    it.body[sizeof(it.body) - 1] = 0;
    strncpy(it.icon, icon ? icon : "", sizeof(it.icon) - 1);
    it.icon[sizeof(it.icon) - 1] = 0;
    it.epoch = (uint32_t)time(NULL);
    it.unread = true;
    sHead = (sHead + 1) % CAP;
    if (sCount < CAP) sCount++;
    sRev++;
    portEXIT_CRITICAL(&sMux);
    persist();
}

uint8_t count() { return sCount; }

bool get(uint8_t i, NotificationItem& out) {
    bool ok = false;
    portENTER_CRITICAL(&sMux);
    if (i < sCount) {
        out = sItems[(sHead + CAP - 1 - i) % CAP];
        ok = true;
    }
    portEXIT_CRITICAL(&sMux);
    return ok;
}

uint8_t unread() {
    uint8_t n = 0;
    portENTER_CRITICAL(&sMux);
    for (uint8_t i = 0; i < sCount; i++) if (sItems[(sHead + CAP - 1 - i) % CAP].unread) n++;
    portEXIT_CRITICAL(&sMux);
    return n;
}

void markAllRead() {
    bool changed = false;
    portENTER_CRITICAL(&sMux);
    for (uint8_t i = 0; i < CAP; i++) if (sItems[i].unread) { sItems[i].unread = false; changed = true; }
    if (changed) sRev++;
    portEXIT_CRITICAL(&sMux);
    if (changed) persist();
}

void clear() {
    portENTER_CRITICAL(&sMux);
    sCount = 0;
    sHead = 0;
    sRev++;
    portEXIT_CRITICAL(&sMux);
    persist();
}

uint32_t revision() { return sRev; }

}  // namespace NotificationStore
