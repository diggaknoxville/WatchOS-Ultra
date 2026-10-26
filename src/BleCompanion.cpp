#include "BleCompanion.h"
#include "AiLimitsStore.h"
#include "SettingsStorage.h"
#include "ui/Navigator.h"
#include "ui/Icons.h"
#include "DisplayEngine.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include <string.h>

#define BLE_DEVICE_NAME             "WatchOS Ultra"
#define SERVICE_AI_LIMITS_UUID      "0000a100-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_DATA_UUID    "0000a101-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_STATUS_UUID  "0000a102-0000-1000-8000-00805f9b34fb"
#define CHAR_COMMAND_UUID           "0000a103-0000-1000-8000-00805f9b34fb"

static NimBLEServer* sServer = nullptr;
static NimBLECharacteristic* sDataChar = nullptr;
static NimBLECharacteristic* sStatusChar = nullptr;
static NimBLECharacteristic* sCmdChar = nullptr;
static bool sBleEnabled = true;
static bool sStackInited = false;
static volatile bool sConnected = false;
static volatile bool sCmdSubscribed = false;
static uint32_t sLastAdvRestart = 0;
static uint32_t sPasskey = 0;

// ---- raw write queue: NimBLE host task -> BleCompanion::loop() -----------
static constexpr size_t RX_SLOT_BYTES = 512;
static constexpr int    RX_SLOTS = 2;
struct RxSlot {
    uint16_t len;
    uint8_t  data[RX_SLOT_BYTES];
};
static RxSlot sRxSlots[RX_SLOTS];
static volatile uint8_t sRxHead = 0, sRxCount = 0;
static portMUX_TYPE sRxMux = portMUX_INITIALIZER_UNLOCKED;

// ---- outgoing command queue (UI task -> loop) -----------------------------
static constexpr int CMD_SLOTS = 4;
static constexpr size_t CMD_BYTES = 32;
static char sCmdQueue[CMD_SLOTS][CMD_BYTES];
static volatile uint8_t sCmdHead = 0, sCmdCount = 0;
static portMUX_TYPE sCmdMux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool sStatusNotifyPending = false;
static volatile bool sCalSyncPending = false;
static uint32_t sLastCalSyncMs = 0;
static constexpr uint32_t CAL_SYNC_MIN_MS = 60000UL;

static constexpr int MAX_CAL_EVENTS = 12;
static CalendarEvent sCalendarEvents[MAX_CAL_EVENTS];
static uint8_t sNumCalEvents = 0;
static portMUX_TYPE sCalMux = portMUX_INITIALIZER_UNLOCKED;

static MediaState sMediaState = {false, "", "", 50};
static portMUX_TYPE sMediaMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
//  Tiny JSON helpers. Keys are matched only at key positions ("key" followed
//  by ':') and only inside [json, end).
// ---------------------------------------------------------------------------
static inline uint32_t rd_u32le(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Bounded substring search (no NUL terminator required inside [hay, end)).
static const char* findSub(const char* hay, const char* end, const char* needle, size_t nlen) {
    if (nlen == 0 || hay >= end) return nullptr;
    for (const char* p = hay; p + nlen <= end; p++) {
        if (*p == needle[0] && memcmp(p, needle, nlen) == 0) return p;
    }
    return nullptr;
}

static const char* findKey(const char* json, const char* end, const char* key) {
    char search[32];
    int sl = snprintf(search, sizeof(search), "\"%s\"", key);
    if (sl <= 0 || sl >= (int)sizeof(search)) return nullptr;
    const char* p = json;
    while (p + sl <= end) {
        const char* hit = findSub(p, end, search, (size_t)sl);
        if (!hit) return nullptr;
        const char* q = hit + sl;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            return q;
        }
        p = hit + sl;
    }
    return nullptr;
}

static uint32_t parseJsonUint(const char* json, const char* end, const char* key, uint32_t defVal = 0) {
    const char* v = findKey(json, end, key);
    if (!v || v >= end) return defVal;
    if (*v == 't') return 1;          // true
    if (*v == 'f') return 0;          // false
    if (*v == '"') v++;               // tolerate quoted numbers
    if (*v < '0' || *v > '9') return defVal;
    return (uint32_t)strtoul(v, nullptr, 10);
}

static bool parseJsonString(const char* json, const char* end, const char* key, char* out, size_t maxLen) {
    const char* v = findKey(json, end, key);
    if (!v || v >= end || *v != '"') return false;
    v++;
    size_t i = 0;
    while (v < end && *v != '"' && i + 1 < maxLen) {
        if (*v == '\\' && v + 1 < end) v++;
        out[i++] = *v++;
    }
    out[i] = 0;
    return true;
}

// Find the '}' that closes the object starting at `open` (which points at '{').
static const char* findObjectEnd(const char* open, const char* end) {
    int depth = 0;
    bool inStr = false;
    for (const char* p = open; p < end; p++) {
        char c = *p;
        if (inStr) {
            if (c == '\\' && p + 1 < end) p++;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return p; }
    }
    return nullptr;
}

static void loadCalendarFromNvs() {
    Preferences p;
    if (p.begin("kronos_cal", true)) {
        uint8_t n = min<uint8_t>(p.getUChar("cnt", 0), MAX_CAL_EVENTS);
        if (n > 0) {
            size_t got = p.getBytes("events", sCalendarEvents, n * sizeof(CalendarEvent));
            n = (uint8_t)(got / sizeof(CalendarEvent));
        }
        sNumCalEvents = n;
        p.end();
    }
}

static void saveCalendarToNvs() {
    CalendarEvent tmp[MAX_CAL_EVENTS];
    portENTER_CRITICAL(&sCalMux);
    uint8_t n = sNumCalEvents;
    memcpy(tmp, sCalendarEvents, n * sizeof(CalendarEvent));
    portEXIT_CRITICAL(&sCalMux);
    Preferences p;
    if (p.begin("kronos_cal", false)) {
        p.putUChar("cnt", n);
        if (n > 0) p.putBytes("events", tmp, n * sizeof(CalendarEvent));
        p.end();
    }
}

// Parse a JSON event array starting at `arr` (points at '[') into the store.
static uint8_t parseCalendarArray(const char* arr, const char* end, CalendarEvent* out, uint8_t maxOut) {
    uint8_t n = 0;
    const char* p = arr;
    while (n < maxOut) {
        const char* open = (const char*)memchr(p, '{', end - p);
        if (!open) break;
        const char* close = findObjectEnd(open, end);
        if (!close) break;
        CalendarEvent& ev = out[n];
        memset(&ev, 0, sizeof(ev));
        if (!parseJsonString(open, close, "t", ev.title, sizeof(ev.title)))
            parseJsonString(open, close, "title", ev.title, sizeof(ev.title));
        if (!parseJsonString(open, close, "l", ev.loc, sizeof(ev.loc)))
            parseJsonString(open, close, "location", ev.loc, sizeof(ev.loc));
        ev.startEpoch = parseJsonUint(open, close, "s", 0);
        if (ev.startEpoch == 0) ev.startEpoch = parseJsonUint(open, close, "start", 0);
        ev.endEpoch = parseJsonUint(open, close, "e", 0);
        if (ev.endEpoch == 0) ev.endEpoch = parseJsonUint(open, close, "end", 0);
        ev.allDay = parseJsonUint(open, close, "a", 0) != 0 || parseJsonUint(open, close, "allDay", 0) != 0;
        if (ev.title[0] || ev.startEpoch) n++;
        p = close + 1;
    }
    return n;
}

static void commitCalendar(const CalendarEvent* evs, uint8_t n, const char* src) {
    portENTER_CRITICAL(&sCalMux);
    sNumCalEvents = n;
    if (n) memcpy(sCalendarEvents, evs, n * sizeof(CalendarEvent));
    portEXIT_CRITICAL(&sCalMux);
    saveCalendarToNvs();
    nav::markDirty(nav::PG_AGENDA);
    DisplayEngine::showNotification("Kalender", "Termine synchronisiert", 2000, ICO_CALENDAR_MONTH);
    Serial.printf("[BLE] Calendar updated via %s (%u events)\n", src, (unsigned)n);
}

// ---------------------------------------------------------------------------
//  NimBLE callbacks: only copy / flag, never parse or write NVS here.
// ---------------------------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        sConnected = true;
        sCmdSubscribed = false;
        AiLimitsStore::setBleConnected(true);
        Serial.printf("[BLE] Client connected (connId=%u)\n", connInfo.getConnHandle());
        pServer->updateConnParams(connInfo.getConnHandle(), 24, 40, 0, 400); // 30-50ms interval, 4s timeout
        nav::markDirty(nav::PG_AI_LIMITS);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_AGENDA);
        nav::markDirty(nav::PG_APP_MEDIA);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        sConnected = false;
        sCmdSubscribed = false;
        AiLimitsStore::setBleConnected(false);
        Serial.printf("[BLE] Client disconnected (reason=%d)\n", reason);
        nav::markDirty(nav::PG_AI_LIMITS);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_AGENDA);
        if (sBleEnabled) {
            NimBLEDevice::startAdvertising();
        }
    }

    uint32_t onPassKeyDisplay() override {
        Serial.println("[BLE] Passkey requested by peer (shown on watch)");
        nav::markDirty(nav::PG_CONTROL);
        return sPasskey;
    }

    // No onConfirmPassKey override: with BLE_HS_IO_DISPLAY_ONLY the stack never
    // selects numeric comparison, the peer has to type the displayed passkey.

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            Serial.println("[BLE] Pairing failed / link not encrypted -> disconnect");
            NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
            return;
        }
        Serial.printf("[BLE] Link encrypted (bonded=%d, authenticated=%d)\n",
                      (int)connInfo.isBonded(), (int)connInfo.isAuthenticated());
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pCharacteristic->getValue();
        size_t len = val.length();
        if (len == 0) return;
        if (len > RX_SLOT_BYTES) len = RX_SLOT_BYTES;
        portENTER_CRITICAL(&sRxMux);
        if (sRxCount < RX_SLOTS) {
            RxSlot& s = sRxSlots[(sRxHead + sRxCount) % RX_SLOTS];
            s.len = (uint16_t)len;
            memcpy(s.data, val.data(), len);
            sRxCount = sRxCount + 1;
        }
        portEXIT_CRITICAL(&sRxMux);
    }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
        bool sub = (subValue & 0x0001) != 0;
        sCmdSubscribed = sub;
        if (sub) sCalSyncPending = true;   // phone is listening: ask for the calendar once
        Serial.printf("[BLE] Command channel %s\n", sub ? "subscribed" : "unsubscribed");
    }
};

static bool ensureStack() {
    if (sStackInited) return true;
    Serial.println("[BLE] Initializing NimBLE stack...");
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    if (sPasskey == 0) sPasskey = esp_random() % 1000000UL;
    NimBLEDevice::setSecurityAuth(true /*bond*/, true /*mitm*/, true /*sc*/);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setSecurityPasskey(sPasskey);

    sServer = NimBLEDevice::createServer();
    sServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = sServer->createService(SERVICE_AI_LIMITS_UUID);
    if (!pService) {
        Serial.println("[BLE] Failed to create service!");
        return false;
    }

    sDataChar = pService->createCharacteristic(
        CHAR_AI_LIMITS_DATA_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
    );
    sDataChar->setCallbacks(new DataCallbacks());

    sStatusChar = pService->createCharacteristic(
        CHAR_AI_LIMITS_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY
    );
    {
        uint8_t initial[2] = {0x01, 0};
        sStatusChar->setValue(initial, sizeof(initial));
    }

    sCmdChar = pService->createCharacteristic(
        CHAR_COMMAND_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    sCmdChar->setCallbacks(new CmdCallbacks());


    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName(BLE_DEVICE_NAME);
    pAdvertising->addServiceUUID(SERVICE_AI_LIMITS_UUID);
    pAdvertising->enableScanResponse(true);
    sStackInited = true;
    return true;
}

bool BleCompanion::init() {
    loadCalendarFromNvs();
    sBleEnabled = SettingsStorage::get().bleEnabled;
    if (sPasskey == 0) sPasskey = esp_random() % 1000000UL;
    if (sBleEnabled) {
        if (ensureStack()) {
            NimBLEDevice::getAdvertising()->start();
            Serial.println("[BLE] NimBLE advertising started as '" BLE_DEVICE_NAME "' (passkey pairing)");
        }
    } else {
        Serial.println("[BLE] Disabled by user settings (stack deferred)");
    }
    return true;
}

uint32_t BleCompanion::getPasskey() { return sPasskey; }

// ---------------------------------------------------------------------------
//  Payload processing (background task)
// ---------------------------------------------------------------------------
static void processPayload(const uint8_t* bytes, size_t len) {
    bool updatedLimits = false;

    if (bytes[0] == 0xA1) {
        updatedLimits = AiLimitsStore::updateFromBleBinary(bytes, len);
        Serial.printf("[BLE] Received Quota binary (%u bytes) -> %s\n", (unsigned)len, updatedLimits ? "OK" : "ERR");
    } else if (bytes[0] == 0xC1 && len >= 3) {
        // Binary calendar: [0xC1][ver][count] then per event:
        //   u32 start, u32 end, u8 allDay, char[32] title, char[15] loc  (= 56 bytes)
        uint8_t count = min<uint8_t>(bytes[2], MAX_CAL_EVENTS);
        CalendarEvent evs[MAX_CAL_EVENTS];
        uint8_t n = 0;
        size_t off = 3;
        for (uint8_t i = 0; i < count && off + 56 <= len; i++) {
            CalendarEvent& ev = evs[n++];
            memset(&ev, 0, sizeof(ev));
            ev.startEpoch = rd_u32le(bytes + off); off += 4;
            ev.endEpoch   = rd_u32le(bytes + off); off += 4;
            ev.allDay     = (bytes[off++] != 0);
            memcpy(ev.title, bytes + off, 32); ev.title[32] = 0; off += 32;
            memcpy(ev.loc, bytes + off, 15);   ev.loc[15] = 0;   off += 15;
        }
        commitCalendar(evs, n, "BLE binary");
    } else if (bytes[0] == '{') {
        const char* json = (const char*)bytes;
        const char* end = json + len;
        const char* calKey = findKey(json, end, "cal");
        if (!calKey) calKey = findKey(json, end, "events");
        if (calKey) {
            const char* arr = (const char*)memchr(calKey, '[', end - calKey);
            if (arr) {
                CalendarEvent evs[MAX_CAL_EVENTS];
                uint8_t n = parseCalendarArray(arr, end, evs, MAX_CAL_EVENTS);
                commitCalendar(evs, n, "BLE JSON");
            }
        } else if (findKey(json, end, "media")) {
            char title[40] = {0};
            char artist[28] = {0};
            parseJsonString(json, end, "title", title, sizeof(title));
            parseJsonString(json, end, "artist", artist, sizeof(artist));
            bool isPlay = (parseJsonUint(json, end, "play", 0) != 0);
            uint8_t vol = (uint8_t)min<uint32_t>(parseJsonUint(json, end, "vol", 50), 100);
            BleCompanion::setMediaState(isPlay, title, artist, vol);
            nav::markDirty(nav::PG_APP_MEDIA);
        } else {
            char tmp[RX_SLOT_BYTES + 1];
            memcpy(tmp, bytes, len);
            tmp[len] = 0;
            updatedLimits = AiLimitsStore::updateFromJson(tmp);
            Serial.printf("[BLE] Received JSON payload (%u bytes) -> %s\n", (unsigned)len, updatedLimits ? "OK" : "ERR");
        }
    }

    if (updatedLimits) {
        nav::markDirty(nav::PG_AI_LIMITS);
        sStatusNotifyPending = true;
    }
}

static bool sendCommandNow(const char* cmd) {
    if (!sConnected || !sCmdChar || !sStackInited) return false;
    sCmdChar->setValue((const uint8_t*)cmd, strlen(cmd));
    bool ok = sCmdChar->notify();
    Serial.printf("[BLE] Command %s: %s\n", ok ? "sent" : "dropped", cmd);
    return ok;
}

void BleCompanion::loop(unsigned long now) {
    if (!sBleEnabled || !sStackInited) return;

    if (!sConnected && (now - sLastAdvRestart >= 10000)) {
        sLastAdvRestart = now;
        if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::startAdvertising();
        }
    }

    // Drain the raw write queue (parsing + NVS happen here, not on the host task)
    for (int guard = 0; guard < RX_SLOTS; guard++) {
        static RxSlot work;   // 514 bytes, keep off the task stack
        bool have = false;
        portENTER_CRITICAL(&sRxMux);
        if (sRxCount > 0) {
            work = sRxSlots[sRxHead];
            sRxHead = (sRxHead + 1) % RX_SLOTS;
            sRxCount = sRxCount - 1;
            have = true;
        }
        portEXIT_CRITICAL(&sRxMux);
        if (!have) break;
        if (work.len > 0) processPayload(work.data, work.len);
    }

    if (sStatusNotifyPending) {
        sStatusNotifyPending = false;
        notifyStatus();
    }

    if (sCalSyncPending && sConnected && sCmdSubscribed) {
        sCalSyncPending = false;
        if (sendCommandNow("CMD:CAL_SYNC")) sLastCalSyncMs = now ? now : 1;
    }

    // Outgoing command queue
    for (int guard = 0; guard < CMD_SLOTS; guard++) {
        char cmd[CMD_BYTES];
        bool have = false;
        portENTER_CRITICAL(&sCmdMux);
        if (sCmdCount > 0) {
            memcpy(cmd, sCmdQueue[sCmdHead], CMD_BYTES);
            sCmdHead = (sCmdHead + 1) % CMD_SLOTS;
            sCmdCount = sCmdCount - 1;
            have = true;
        }
        portEXIT_CRITICAL(&sCmdMux);
        if (!have) break;
        cmd[CMD_BYTES - 1] = 0;
        sendCommandNow(cmd);
    }
}

bool BleCompanion::isConnected() {
    return sConnected && sBleEnabled && sStackInited;
}

void BleCompanion::setEnabled(bool en) {
    sBleEnabled = en;
    SettingsStorage::get().bleEnabled = en;
    SettingsStorage::save();
    if (!en) {
        if (sStackInited) {
            if (NimBLEDevice::getAdvertising()->isAdvertising()) {
                NimBLEDevice::stopAdvertising();
            }
            if (sConnected && sServer) {
                std::vector<uint16_t> peers = sServer->getPeerDevices();
                for (uint16_t connId : peers) {
                    sServer->disconnect(connId);
                }
                sConnected = false;
                sCmdSubscribed = false;
                AiLimitsStore::setBleConnected(false);
            }
        }
        Serial.println("[BLE] Disabled");
    } else {
        if (ensureStack() && !sConnected && !NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::startAdvertising();
            Serial.println("[BLE] Re-enabled advertising");
        }
    }
    nav::markDirty(nav::PG_AI_LIMITS);
    nav::markDirty(nav::PG_CONTROL);
    nav::markDirty(nav::PG_SYSTEM);
}

bool BleCompanion::isEnabled() {
    return sBleEnabled;
}

void BleCompanion::notifyStatus() {
    if (sConnected && sStatusChar) {
        AiLimitsData d = AiLimitsStore::get();
        uint8_t topPct = max(d.claude_5h_pct, max(d.ag_gemini_5h_pct, d.ag_claude_5h_pct));
        uint8_t payload[2] = {0x01, topPct};
        sStatusChar->setValue(payload, sizeof(payload));
        sStatusChar->notify();
    }
}

void BleCompanion::sendMediaCommand(const char* cmd) {
    if (!cmd || !cmd[0]) return;
    if (!isConnected()) {
        Serial.printf("[BLE] Command dropped (not connected): %s\n", cmd);
        DisplayEngine::showToast("Bluetooth", "Handy nicht verbunden", 2000, ICO_SMARTPHONE);
        return;
    }
    portENTER_CRITICAL(&sCmdMux);
    if (sCmdCount < CMD_SLOTS) {
        char* slot = sCmdQueue[(sCmdHead + sCmdCount) % CMD_SLOTS];
        strncpy(slot, cmd, CMD_BYTES - 1);
        slot[CMD_BYTES - 1] = 0;
        sCmdCount = sCmdCount + 1;
    }
    portEXIT_CRITICAL(&sCmdMux);
}

bool BleCompanion::triggerCalendarSync() {
    if (!isConnected()) return false;
    uint32_t now = millis();
    if (sLastCalSyncMs != 0 && (now - sLastCalSyncMs) < CAL_SYNC_MIN_MS) return false;
    sLastCalSyncMs = now ? now : 1;
    sCalSyncPending = true;
    return true;
}

uint32_t BleCompanion::lastCalendarSyncMs() { return sLastCalSyncMs; }

MediaState BleCompanion::getMediaState() {
    portENTER_CRITICAL(&sMediaMux);
    MediaState copy = sMediaState;
    portEXIT_CRITICAL(&sMediaMux);
    return copy;
}

void BleCompanion::setMediaState(bool isPlaying, const char* title, const char* artist, uint8_t volumePct) {
    // Copy arguments first: callers may pass pointers into a previous copy or
    // (legacy) into sMediaState itself.
    char t[sizeof(sMediaState.title)] = {0};
    char a[sizeof(sMediaState.artist)] = {0};
    bool haveT = title && title[0];
    bool haveA = artist && artist[0];
    if (haveT) { strncpy(t, title, sizeof(t) - 1); t[sizeof(t) - 1] = 0; }
    if (haveA) { strncpy(a, artist, sizeof(a) - 1); a[sizeof(a) - 1] = 0; }
    if (volumePct > 100) volumePct = 100;

    portENTER_CRITICAL(&sMediaMux);
    sMediaState.isPlaying = isPlaying;
    if (haveT) memcpy(sMediaState.title, t, sizeof(t));
    if (haveA) memcpy(sMediaState.artist, a, sizeof(a));
    sMediaState.volumePct = volumePct;
    portEXIT_CRITICAL(&sMediaMux);
    nav::markDirty(nav::PG_CONTROL);
}

void BleCompanion::setMediaPlaying(bool playing) {
    portENTER_CRITICAL(&sMediaMux);
    sMediaState.isPlaying = playing;
    portEXIT_CRITICAL(&sMediaMux);
    nav::markDirty(nav::PG_CONTROL);
}

void BleCompanion::setMediaVolume(uint8_t pct) {
    if (pct > 100) pct = 100;
    portENTER_CRITICAL(&sMediaMux);
    sMediaState.volumePct = pct;
    portEXIT_CRITICAL(&sMediaMux);
    nav::markDirty(nav::PG_CONTROL);
}

uint8_t BleCompanion::getUpcomingEvents(CalendarEvent* out, uint8_t maxOut) {
    if (!out || maxOut == 0) return 0;
    portENTER_CRITICAL(&sCalMux);
    uint8_t n = min(maxOut, sNumCalEvents);
    for (uint8_t i = 0; i < n; i++) {
        out[i] = sCalendarEvents[i];
    }
    portEXIT_CRITICAL(&sCalMux);
    return n;
}

bool BleCompanion::hasCalendarEvents() {
    return sNumCalEvents > 0;
}

void BleCompanion::updateCalendarFromJson(const char* json) {
    if (!json || !json[0]) return;
    const char* end = json + strlen(json);
    const char* p = findKey(json, end, "cal");
    if (!p) p = findKey(json, end, "events");
    if (!p) p = json;
    const char* arr = (const char*)memchr(p, '[', end - p);
    if (!arr) return;
    CalendarEvent evs[MAX_CAL_EVENTS];
    uint8_t n = parseCalendarArray(arr, end, evs, MAX_CAL_EVENTS);
    commitCalendar(evs, n, "HTTP JSON");
}
