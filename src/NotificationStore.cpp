#include "NotificationStore.h"
#include <Preferences.h>

namespace NotificationStore {

static const uint8_t CAP = 20;
static const unsigned long PERSIST_INTERVAL_MS = 3000;
static const char* NVS_NS = "kronos_ntf";

static NotificationItem sItems[CAP];      // ring buffer, newest at sHead-1
static uint8_t sHead = 0, sCount = 0;
static volatile uint32_t sRev = 1;
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;

// dirty tracking (protected by sMux)
static uint32_t sDirtySlots = 0;          // bit i -> sItems[i] must be written
static bool sMetaDirty = false;           // head/count must be written
static unsigned long sLastPersist = 0;
static bool sWriteFailLogged = false;

static void slotKey(char* buf, size_t n, uint8_t i) { snprintf(buf, n, "n%u", (unsigned)i); }

static void sanitize(NotificationItem& it) {
    it.title[sizeof(it.title) - 1] = 0;
    it.body[sizeof(it.body) - 1] = 0;
    it.icon[sizeof(it.icon) - 1] = 0;
}

void begin() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        Serial.println("[NTF] NVS namespace unavailable, history not persisted");
        return;
    }
    // Migration: older builds stored the whole ring as one 3.3 KB blob
    // ("items"; "ring" in even older builds). Load it once, delete it and
    // re-persist slot by slot.
    bool migrated = false;
    static const char* LEGACY_KEYS[] = {"items", "ring"};
    for (const char* lk : LEGACY_KEYS) {
        if (!p.isKey(lk)) continue;
        if (!migrated && p.getBytesLength(lk) == sizeof(sItems)) {
            p.getBytes(lk, sItems, sizeof(sItems));
            migrated = true;
        }
        p.remove(lk);
    }
    sHead = p.getUChar("head", 0) % CAP;
    sCount = min<uint8_t>(p.getUChar("count", 0), CAP);

    if (migrated) {
        sDirtySlots = (CAP >= 32) ? 0xFFFFFFFFu : ((1u << CAP) - 1u);
        sMetaDirty = true;
        Serial.printf("[NTF] migrated legacy blob (%u entries) to per-slot keys\n", (unsigned)sCount);
    } else {
        char key[8];
        for (uint8_t i = 0; i < CAP; i++) {
            slotKey(key, sizeof(key), i);
            if (p.getBytesLength(key) == sizeof(NotificationItem)) {
                p.getBytes(key, &sItems[i], sizeof(NotificationItem));
            } else {
                memset(&sItems[i], 0, sizeof(NotificationItem));
            }
        }
    }
    p.end();
    for (uint8_t i = 0; i < CAP; i++) sanitize(sItems[i]);
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
    sDirtySlots |= (1u << sHead);
    sMetaDirty = true;
    sHead = (sHead + 1) % CAP;
    if (sCount < CAP) sCount++;
    sRev = sRev + 1;
    portEXIT_CRITICAL(&sMux);
}

void loop(unsigned long now) {
    if (!sDirtySlots && !sMetaDirty) return;
    if (now - sLastPersist < PERSIST_INTERVAL_MS) return;
    sLastPersist = now;

    // Snapshot the dirty slots under the spinlock, write flash outside of it.
    static NotificationItem copy[CAP];        // bg task only
    uint32_t slots;
    bool meta;
    uint8_t head, count;
    portENTER_CRITICAL(&sMux);
    slots = sDirtySlots;
    meta = sMetaDirty;
    sDirtySlots = 0;
    sMetaDirty = false;
    head = sHead;
    count = sCount;
    for (uint8_t i = 0; i < CAP; i++) {
        if (slots & (1u << i)) copy[i] = sItems[i];
    }
    portEXIT_CRITICAL(&sMux);

    uint32_t failedSlots = 0;
    bool failedMeta = false;
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        failedSlots = slots;
        failedMeta = meta;
    } else {
        char key[8];
        for (uint8_t i = 0; i < CAP; i++) {
            if (!(slots & (1u << i))) continue;
            slotKey(key, sizeof(key), i);
            if (p.putBytes(key, &copy[i], sizeof(NotificationItem)) != sizeof(NotificationItem)) {
                failedSlots |= (1u << i);
            }
        }
        if (meta) {
            if (p.putUChar("head", head) == 0 || p.putUChar("count", count) == 0) failedMeta = true;
        }
        p.end();
    }

    if (failedSlots || failedMeta) {
        // keep them dirty, retried on the next interval
        portENTER_CRITICAL(&sMux);
        sDirtySlots |= failedSlots;
        sMetaDirty = sMetaDirty || failedMeta;
        portEXIT_CRITICAL(&sMux);
        if (!sWriteFailLogged) {
            sWriteFailLogged = true;
            Serial.printf("[NTF] NVS write failed (slots 0x%05lx, meta %d) - will retry\n",
                          (unsigned long)failedSlots, (int)failedMeta);
        }
    }
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
    portENTER_CRITICAL(&sMux);
    bool changed = false;
    for (uint8_t i = 0; i < CAP; i++) {
        if (sItems[i].unread) {
            sItems[i].unread = false;
            sDirtySlots |= (1u << i);
            changed = true;
        }
    }
    if (changed) sRev = sRev + 1;
    portEXIT_CRITICAL(&sMux);
}

void clear() {
    portENTER_CRITICAL(&sMux);
    sCount = 0;
    sHead = 0;
    sMetaDirty = true;       // slots become unreachable, no need to rewrite them
    sRev = sRev + 1;
    portEXIT_CRITICAL(&sMux);
}

uint32_t revision() { return sRev; }

}  // namespace NotificationStore
