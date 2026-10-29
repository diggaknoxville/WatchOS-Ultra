#include "BleCompanion.h"
#include "AiLimitsStore.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "ui/Navigator.h"
#include "ui/Icons.h"
#include "DisplayEngine.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include <string.h>

#define BLE_DEVICE_NAME             "WatchOS Ultra"
#define SERVICE_AI_LIMITS_UUID      "0000a100-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_DATA_UUID    "0000a101-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_STATUS_UUID  "0000a102-0000-1000-8000-00805f9b34fb"
#define CHAR_COMMAND_UUID           "0000a103-0000-1000-8000-00805f9b34fb"

// Apple Notification Center Service (ANCS) UUIDs
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIF_SRC_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CTRL_PT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SRC_UUID("22EAC6E9-24D6-4BB5-B36ACE7C7BFB");

// Bluetooth SIG Alert Notification Service (ANS) UUIDs
static const NimBLEUUID ANS_SERVICE_UUID((uint16_t)0x1811);
static const NimBLEUUID ANS_CHAR_SUP_NEW_UUID((uint16_t)0x2A47);
static const NimBLEUUID ANS_CHAR_NEW_ALERT_UUID((uint16_t)0x2A46);
static const NimBLEUUID ANS_CHAR_SUP_UNREAD_UUID((uint16_t)0x2A48);
static const NimBLEUUID ANS_CHAR_UNREAD_STATUS_UUID((uint16_t)0x2A45);
static const NimBLEUUID ANS_CHAR_CTRL_PT_UUID((uint16_t)0x2A44);

static NimBLEServer* sServer = nullptr;
static NimBLECharacteristic* sDataChar = nullptr;
static NimBLECharacteristic* sStatusChar = nullptr;
static NimBLECharacteristic* sCmdChar = nullptr;
static NimBLECharacteristic* sNewAlertChar = nullptr;
static NimBLECharacteristic* sUnreadAlertChar = nullptr;

// ANCS State
static volatile bool sAncsDiscoveryPending = false;
static uint16_t sAncsConnHandle = BLE_HS_CONN_HANDLE_NONE;
static NimBLERemoteCharacteristic* sAncsCtrlPoint = nullptr;
static uint32_t sLastAncsUid = 0;
static const char* sLastAncsIcon = ICO_NOTIFICATIONS;

// Native BLE HID Consumer Control (Media Play/Pause, Next, Prev, Vol+, Vol-)
static NimBLEHIDDevice* sHid = nullptr;
static NimBLECharacteristic* sInputReport = nullptr;

// HID Report Descriptor for Consumer Control (Report ID 1, 1 Byte)
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x07,        //   Report Count (7)
    0x09, 0xCD,        //   Usage (Play/Pause)          - bit 0
    0x09, 0xB5,        //   Usage (Scan Next Track)     - bit 1
    0x09, 0xB6,        //   Usage (Scan Previous Track) - bit 2
    0x09, 0xB7,        //   Usage (Stop)                - bit 3
    0x09, 0xE9,        //   Usage (Volume Increment)    - bit 4
    0x09, 0xEA,        //   Usage (Volume Decrement)    - bit 5
    0x09, 0xE2,        //   Usage (Mute)                - bit 6
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const)               - padding bit 7
    0xC0               // End Collection
};
static bool sBleEnabled = true;
static bool sStackInited = false;
static volatile bool sConnected = false;
static volatile bool sCmdSubscribed = false;
static uint32_t sLastAdvRestart = 0;
static uint32_t sPasskey = 0;

// ---- power: advertising cadence, connection parameters, stack shutdown ----
// Fast advertising for 30 s after boot / disconnect (quick reconnect), then the
// slow 417.5..546.25 ms interval recommended for accessories. A connection
// starts with a 30..50 ms interval for service discovery and is relaxed after
// 15 s to 120..180 ms with peripheral latency 4 (<= 0.9 s, supervision 6 s).
static constexpr uint32_t ADV_FAST_MS = 30000;
static constexpr uint16_t ADV_FAST_MIN = 32, ADV_FAST_MAX = 48;      // 20 .. 30 ms (0.625 ms units)
static constexpr uint16_t ADV_SLOW_MIN = 668, ADV_SLOW_MAX = 874;    // 417.5 .. 546.25 ms
static constexpr uint32_t CONN_RELAX_AFTER_MS = 15000;
static volatile uint32_t sAdvFastSince = 0;       // 0 = slow cadence active
static volatile uint32_t sConnectedAt = 0;
static volatile bool sConnRelaxed = false;
static volatile bool sShutdownPending = false;    // setEnabled(false): release the stack on the bg task
static volatile bool sEnableRequest = false;      // setEnabled(true): build / restart on the bg task

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
//  Apple Notification Center Service (ANCS) & Alert Notification Service (ANS)
// ---------------------------------------------------------------------------
static void onAncsNotificationSourceNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 8) return;
    uint8_t eventId = pData[0]; // 0=Added, 1=Modified, 2=Removed
    uint8_t categoryId = pData[2];
    uint32_t uid = (uint32_t)pData[4] | ((uint32_t)pData[5] << 8) | ((uint32_t)pData[6] << 16) | ((uint32_t)pData[7] << 24);

    Serial.printf("[BLE] ANCS Source: event=%u cat=%u uid=%lu\n", (unsigned)eventId, (unsigned)categoryId, (unsigned long)uid);

    if (eventId == 0) { // New notification added
        const char* icon = ICO_NOTIFICATIONS;
        switch (categoryId) {
            case 1: icon = ICO_CALL; break; // Incoming call
            case 2: icon = ICO_CALL; break; // Missed call
            case 3: icon = ICO_CALL; break; // Voicemail
            case 4: icon = ICO_CHAT; break; // Social
            case 5: icon = ICO_CALENDAR_MONTH; break; // Schedule
            case 6: icon = ICO_MAIL; break; // Email
            case 7: icon = ICO_PODCASTS; break; // News
            case 8: icon = ICO_MONITOR_HEART; break; // Health
            default: icon = ICO_NOTIFICATIONS; break;
        }
        sLastAncsUid = uid;
        sLastAncsIcon = icon;

        if (sAncsCtrlPoint) {
            // Request Title (Attr 1, max 40 bytes)
            uint8_t reqTitle[8] = {0x00, pData[4], pData[5], pData[6], pData[7], 0x01, 0x28, 0x00};
            sAncsCtrlPoint->writeValue(reqTitle, sizeof(reqTitle), false);
            // Request Message (Attr 3, max 100 bytes)
            uint8_t reqMsg[8] = {0x00, pData[4], pData[5], pData[6], pData[7], 0x03, 0x64, 0x00};
            sAncsCtrlPoint->writeValue(reqMsg, sizeof(reqMsg), false);
        }
    }
}

static void onAncsDataSourceNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 8) return;
    uint8_t cmdId = pData[0];
    if (cmdId != 0x00) return; // CommandIDGetNotificationAttributes

    size_t idx = 5;
    char title[44] = {0};
    char message[116] = {0};
    while (idx + 3 <= length) {
        uint8_t attrId = pData[idx++];
        uint16_t attrLen = (uint16_t)pData[idx] | ((uint16_t)pData[idx + 1] << 8);
        idx += 2;
        if (idx + attrLen > length) attrLen = length - idx;
        if (attrId == 0x01) { // Title
            size_t copyLen = min<size_t>(attrLen, sizeof(title) - 1);
            memcpy(title, pData + idx, copyLen);
            title[copyLen] = 0;
        } else if (attrId == 0x03) { // Message
            size_t copyLen = min<size_t>(attrLen, sizeof(message) - 1);
            memcpy(message, pData + idx, copyLen);
            message[copyLen] = 0;
        }
        idx += attrLen;
    }
    if (title[0] || message[0]) {
        Serial.printf("[BLE] ANCS Notification: '%s' | '%s'\n", title, message);
        DisplayEngine::showNotification(title[0] ? title : "iOS Mitteilung", message, 4500, sLastAncsIcon);
    }
}

class AnsAlertCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pChar->getValue();
        size_t len = val.length();
        if (len < 2) return;
        uint8_t catId = val.data()[0];
        const char* icon = ICO_NOTIFICATIONS;
        const char* defaultTitle = "Mitteilung";
        switch (catId) {
            case 1: icon = ICO_MAIL; defaultTitle = "E-Mail"; break;
            case 2: icon = ICO_PODCASTS; defaultTitle = "News"; break;
            case 3: icon = ICO_CALL; defaultTitle = "Anruf"; break;
            case 4: icon = ICO_CALL; defaultTitle = "Entgangener Anruf"; break;
            case 5: icon = ICO_CHAT; defaultTitle = "SMS"; break;
            case 7: icon = ICO_CALENDAR_MONTH; defaultTitle = "Termin"; break;
            case 9: icon = ICO_CHAT; defaultTitle = "Nachricht"; break;
            default: break;
        }
        char text[128] = {0};
        if (len > 2) {
            size_t textLen = min<size_t>(len - 2, sizeof(text) - 1);
            memcpy(text, val.data() + 2, textLen);
            text[textLen] = 0;
        }
        char title[44] = {0};
        char body[112] = {0};
        char* colon = strchr(text, ':');
        char* nl = strchr(text, '\n');
        if (nl) {
            *nl = 0;
            strncpy(title, text, sizeof(title) - 1);
            strncpy(body, nl + 1, sizeof(body) - 1);
        } else if (colon && (size_t)(colon - text) < sizeof(title)) {
            *colon = 0;
            strncpy(title, text, sizeof(title) - 1);
            strncpy(body, colon + 1, sizeof(body) - 1);
            while (body[0] == ' ') memmove(body, body + 1, strlen(body));
        } else {
            strncpy(title, defaultTitle, sizeof(title) - 1);
            strncpy(body, text[0] ? text : "Neue Benachrichtigung", sizeof(body) - 1);
        }
        Serial.printf("[BLE] ANS Alert cat=%u: '%s' - '%s'\n", (unsigned)catId, title, body);
        DisplayEngine::showNotification(title, body, 4000, icon);
    }
};

class AnsCtrlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pChar->getValue();
        if (val.length() >= 1) {
            Serial.printf("[BLE] ANS Control Point cmd=0x%02X\n", (unsigned)val.data()[0]);
        }
    }
};

// ---------------------------------------------------------------------------
//  NimBLE callbacks: only copy / flag, never parse or write NVS here.
// ---------------------------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        sConnected = true;
        sCmdSubscribed = false;
        sAncsConnHandle = connInfo.getConnHandle();
        sAncsDiscoveryPending = true;
        AiLimitsStore::setBleConnected(true);
        Serial.printf("[BLE] Client connected (connId=%u)\n", connInfo.getConnHandle());
        pServer->updateConnParams(connInfo.getConnHandle(), 24, 40, 0, 400); // 30-50ms interval, 4s timeout
        sConnectedAt = millis() ? millis() : 1;
        sConnRelaxed = false;
        nav::markDirty(nav::PG_AI_LIMITS);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_AGENDA);
        nav::markDirty(nav::PG_APP_MEDIA);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        sConnected = false;
        sCmdSubscribed = false;
        sAncsDiscoveryPending = false;
        sAncsCtrlPoint = nullptr;
        AiLimitsStore::setBleConnected(false);
        Serial.printf("[BLE] Client disconnected (reason=%d)\n", reason);
        nav::markDirty(nav::PG_AI_LIMITS);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_AGENDA);
        sConnectedAt = 0;
        if (sBleEnabled && !sShutdownPending) {
            NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
            adv->setMinInterval(ADV_FAST_MIN);
            adv->setMaxInterval(ADV_FAST_MAX);
            sAdvFastSince = millis() ? millis() : 1;
            NimBLEDevice::startAdvertising();
        }
    }

    uint32_t onPassKeyDisplay() override {
        Serial.printf("[BLE] Passkey display: %06lu\n", (unsigned long)sPasskey);
        nav::markDirty(nav::PG_CONTROL);
        return sPasskey;
    }

    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        Serial.printf("[BLE] Auto-injecting passkey for entry: %06lu\n", (unsigned long)sPasskey);
        NimBLEDevice::injectPassKey(connInfo, sPasskey);
    }

    void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t passkey) override {
        Serial.printf("[BLE] Auto-confirming passkey: %06lu\n", (unsigned long)passkey);
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf("[BLE] Auth complete: encrypted=%d bonded=%d authenticated=%d\n",
                      (int)connInfo.isEncrypted(), (int)connInfo.isBonded(), (int)connInfo.isAuthenticated());
        if (connInfo.isEncrypted() || connInfo.isBonded()) {
            sAncsConnHandle = connInfo.getConnHandle();
            sAncsDiscoveryPending = true;
        }
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
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);      // wrist to phone range; +9 dBm only costs current

    // Standard BLE Security: Just-Works Bonding for seamless iOS & Android pairing without companion app
    NimBLEDevice::setSecurityPasskey(sPasskey);
    NimBLEDevice::setSecurityAuth(true /*bond*/, false /*mitm*/, true /*sc*/);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    sServer = NimBLEDevice::createServer();
    sServer->setCallbacks(new ServerCallbacks());

    // Native BLE HID Consumer Control (Media Play/Pause, Next, Prev, Vol+, Vol-)
    sHid = new NimBLEHIDDevice(sServer);
    sHid->setManufacturer("WatchOS");
    sHid->setPnp(0x02, 0xe502, 0xa111, 0x0110);
    sHid->setHidInfo(0x00, 0x01);
    sHid->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    sInputReport = sHid->getInputReport(1);
    sHid->setBatteryLevel(PowerManager::getBatteryPercent());

    NimBLEService* pService = sServer->createService(SERVICE_AI_LIMITS_UUID);
    if (!pService) {
        Serial.println("[BLE] Failed to create companion service!");
        return false;
    }

    sDataChar = pService->createCharacteristic(
        CHAR_AI_LIMITS_DATA_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    sDataChar->setCallbacks(new DataCallbacks());

    sStatusChar = pService->createCharacteristic(
        CHAR_AI_LIMITS_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
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

    // Standard Bluetooth SIG Alert Notification Service (ANS 0x1811) for Android & smart accessories
    NimBLEService* pAnsService = sServer->createService(ANS_SERVICE_UUID);
    if (pAnsService) {
        NimBLECharacteristic* pSupNew = pAnsService->createCharacteristic(
            ANS_CHAR_SUP_NEW_UUID,
            NIMBLE_PROPERTY::READ
        );
        uint8_t supNew[2] = {0xFF, 0x03};
        pSupNew->setValue(supNew, sizeof(supNew));

        NimBLECharacteristic* pSupUnread = pAnsService->createCharacteristic(
            ANS_CHAR_SUP_UNREAD_UUID,
            NIMBLE_PROPERTY::READ
        );
        uint8_t supUnread[2] = {0xFF, 0x03};
        pSupUnread->setValue(supUnread, sizeof(supUnread));

        sNewAlertChar = pAnsService->createCharacteristic(
            ANS_CHAR_NEW_ALERT_UUID,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
        );
        sNewAlertChar->setCallbacks(new AnsAlertCallbacks());

        sUnreadAlertChar = pAnsService->createCharacteristic(
            ANS_CHAR_UNREAD_STATUS_UUID,
            NIMBLE_PROPERTY::NOTIFY
        );
        uint8_t initialUnread[2] = {0x00, 0x00};
        sUnreadAlertChar->setValue(initialUnread, sizeof(initialUnread));

        NimBLECharacteristic* pAnsCtrl = pAnsService->createCharacteristic(
            ANS_CHAR_CTRL_PT_UUID,
            NIMBLE_PROPERTY::WRITE
        );
        pAnsCtrl->setCallbacks(new AnsCtrlCallbacks());
    }

    sServer->start();

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setName(BLE_DEVICE_NAME);
    advData.setAppearance(GENERIC_HID);
    advData.addServiceUUID(sHid->getHidService()->getUUID());
    pAdvertising->setAdvertisementData(advData);

    NimBLEAdvertisementData scanData;
    scanData.addServiceUUID(SERVICE_AI_LIMITS_UUID);
    scanData.addServiceUUID(ANS_SERVICE_UUID);
    pAdvertising->setScanResponseData(scanData);
    pAdvertising->enableScanResponse(true);
    pAdvertising->setMinInterval(ADV_FAST_MIN);
    pAdvertising->setMaxInterval(ADV_FAST_MAX);
    sAdvFastSince = millis() ? millis() : 1;
    sStackInited = true;
    return true;
}

static bool sPreloaded = false;

void BleCompanion::preload() {
    if (sPreloaded) return;
    sPreloaded = true;
    loadCalendarFromNvs();
}

// bg task: release the whole NimBLE host + controller so the CPU can light-sleep
static void shutdownStack() {
    if (!sStackInited) { sShutdownPending = false; return; }
    static uint32_t sDiscAt = 0;
    if (sConnected && sServer) {
        if (!sDiscAt) {
            sDiscAt = millis() ? millis() : 1;
            std::vector<uint16_t> peers = sServer->getPeerDevices();
            for (uint16_t connId : peers) sServer->disconnect(connId);
        }
        if (millis() - sDiscAt < 1500) return;          // give the link time to close
    }
    sDiscAt = 0;
    if (NimBLEDevice::getAdvertising()->isAdvertising()) NimBLEDevice::stopAdvertising();
    sConnected = false;
    sCmdSubscribed = false;
    sAncsDiscoveryPending = false;
    AiLimitsStore::setBleConnected(false);
    // deinit(true) deletes server, services, characteristics and clients
    bool ok = NimBLEDevice::deinit(true);
    sServer = nullptr;
    sDataChar = sStatusChar = sCmdChar = sNewAlertChar = sUnreadAlertChar = nullptr;
    sAncsCtrlPoint = nullptr;
    if (sHid) { delete sHid; sHid = nullptr; }   // only holds pointers into the deleted server
    sInputReport = nullptr;
    sStackInited = false;
    sShutdownPending = false;
    Serial.printf("[BLE] Stack released (%s)\n", ok ? "ok" : "deinit failed");
}

bool BleCompanion::isStackRunning() { return sStackInited; }

bool BleCompanion::init() {
    preload();
    sBleEnabled = SettingsStorage::get().bleEnabled;
    Preferences p;
    if (p.begin("ble_cfg", false)) {
        sPasskey = p.getUInt("passkey", 123456);
        p.putUInt("passkey", sPasskey);
        p.end();
    } else {
        sPasskey = 123456;
    }
    if (sBleEnabled) {
        if (ensureStack()) {
            NimBLEDevice::getAdvertising()->start();
            Serial.printf("[BLE] NimBLE advertising started as '%s' (passkey %06lu)\n", BLE_DEVICE_NAME, (unsigned long)sPasskey);
        }
    } else {
        Serial.println("[BLE] Disabled by user settings (stack deferred)");
    }
    return true;
}

void BleCompanion::resetBonds() {
    NimBLEDevice::deleteAllBonds();
    Serial.println("[BLE] All bonds cleared from NVS");
}

uint32_t BleCompanion::getPasskey() { return sPasskey; }

// ---------------------------------------------------------------------------
//  Payload processing (background task)
// ---------------------------------------------------------------------------
static void processPayload(const uint8_t* bytes, size_t len) {
    bool updatedLimits = false;

    if (len > 2 && bytes[0] == 'N') {
        char buf[RX_SLOT_BYTES + 1];
        memcpy(buf, bytes + 1, len - 1);
        buf[len - 1] = 0;
        char* sep = strchr(buf, '|');
        if (sep) {
            *sep = 0;
            DisplayEngine::showNotification(buf, sep + 1, 4000, ICO_NOTIFICATIONS);
        } else {
            DisplayEngine::showNotification("Mitteilung", buf, 4000, ICO_NOTIFICATIONS);
        }
        return;
    }

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
        } else if (findKey(json, end, "notify")) {
            char title[40] = {0};
            char body[112] = {0};
            parseJsonString(json, end, "title", title, sizeof(title));
            parseJsonString(json, end, "body", body, sizeof(body));
            DisplayEngine::showNotification(title[0] ? title : "Mitteilung", body, 4000, ICO_NOTIFICATIONS);
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
    if (!sConnected || !sStackInited) return false;

    // 1. Native HID Consumer Control (Media commands for Android, iOS, Windows, macOS)
    uint8_t hidMask = 0;
    if (strstr(cmd, "PLAY") || strstr(cmd, "PAUSE")) hidMask = 0x01;
    else if (strstr(cmd, "NEXT")) hidMask = 0x02;
    else if (strstr(cmd, "PREV")) hidMask = 0x04;
    else if (strstr(cmd, "STOP")) hidMask = 0x08;
    else if (strstr(cmd, "VOL_UP")) hidMask = 0x10;
    else if (strstr(cmd, "VOL_DOWN") || strstr(cmd, "DOWN")) hidMask = 0x20;

    if (hidMask != 0 && sInputReport) {
        uint8_t press[1] = {hidMask};
        sInputReport->setValue(press, 1);
        sInputReport->notify();
        vTaskDelay(pdMS_TO_TICKS(15));
        uint8_t release[1] = {0x00};
        sInputReport->setValue(release, 1);
        sInputReport->notify();
        Serial.printf("[BLE] HID Consumer key sent: 0x%02X (%s)\n", (unsigned)hidMask, cmd);
    }

    // 2. Also forward to custom companion app if subscribed
    if (sCmdChar && sCmdSubscribed) {
        sCmdChar->setValue((const uint8_t*)cmd, strlen(cmd));
        bool ok = sCmdChar->notify();
        Serial.printf("[BLE] Command %s: %s\n", ok ? "sent" : "dropped", cmd);
        return ok;
    }
    return true;
}

void BleCompanion::loop(unsigned long now) {
    if (sShutdownPending) { shutdownStack(); return; }
    if (sEnableRequest) {
        sEnableRequest = false;
        if (sBleEnabled && ensureStack() && !sConnected && !NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::getAdvertising()->setMinInterval(ADV_FAST_MIN);
            NimBLEDevice::getAdvertising()->setMaxInterval(ADV_FAST_MAX);
            sAdvFastSince = now ? now : 1;
            NimBLEDevice::startAdvertising();
            Serial.println("[BLE] Enabled, advertising");
        }
    }
    if (!sBleEnabled || !sStackInited) return;

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!sConnected && sAdvFastSince && now - sAdvFastSince >= ADV_FAST_MS) {
        sAdvFastSince = 0;                              // fast window over: slow cadence
        bool was = adv->isAdvertising();
        if (was) adv->stop();
        adv->setMinInterval(ADV_SLOW_MIN);
        adv->setMaxInterval(ADV_SLOW_MAX);
        if (was) adv->start();
    }
    if (!sConnected && (now - sLastAdvRestart >= 10000)) {
        sLastAdvRestart = now;
        if (!adv->isAdvertising()) {
            NimBLEDevice::startAdvertising();
        }
    }
    if (sConnected && !sConnRelaxed && sConnectedAt && now - sConnectedAt >= CONN_RELAX_AFTER_MS && sServer) {
        sConnRelaxed = true;
        std::vector<uint16_t> peers = sServer->getPeerDevices();
        for (uint16_t h : peers) sServer->updateConnParams(h, 96, 144, 4, 600);
        Serial.println("[BLE] Connection parameters relaxed (120-180 ms, latency 4)");
    }

    // Discover ANCS on connected iOS devices
    if (sAncsDiscoveryPending && sConnected && sServer) {
        sAncsDiscoveryPending = false;
        NimBLEClient* pClient = sServer->getClient(sAncsConnHandle);
        if (pClient && pClient->isConnected()) {
            Serial.println("[BLE] Probing for Apple ANCS service...");
            NimBLERemoteService* pAncs = pClient->getService(ANCS_SERVICE_UUID);
            if (pAncs) {
                Serial.println("[BLE] Found Apple ANCS Service!");
                auto pNotifSource = pAncs->getCharacteristic(ANCS_NOTIF_SRC_UUID);
                auto pCtrlPoint = pAncs->getCharacteristic(ANCS_CTRL_PT_UUID);
                auto pDataSource = pAncs->getCharacteristic(ANCS_DATA_SRC_UUID);
                if (pNotifSource && pCtrlPoint && pDataSource) {
                    sAncsCtrlPoint = pCtrlPoint;
                    pDataSource->subscribe(true, onAncsDataSourceNotify);
                    pNotifSource->subscribe(true, onAncsNotificationSourceNotify);
                    Serial.println("[BLE] Subscribed to Apple ANCS notifications!");
                }
            } else {
                Serial.println("[BLE] Peer does not host ANCS (Android, PC or unsubscribed)");
            }
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
        // Disconnect, stop advertising and release host + controller on the
        // background task (NimBLE calls must not run on the UI task).
        sShutdownPending = true;
        Serial.println("[BLE] Disabled (stack release pending)");
    } else {
        sShutdownPending = false;
        sEnableRequest = true;                          // stack is (re)built on the bg task
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
