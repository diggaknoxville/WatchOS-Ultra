#include "BleCompanion.h"
#include "AiLimitsStore.h"
#include "SettingsStorage.h"
#include "ui/Navigator.h"
#include "DisplayEngine.h"
#include <NimBLEDevice.h>
#include <Preferences.h>

#define BLE_DEVICE_NAME             "WatchOS Ultra"
#define SERVICE_AI_LIMITS_UUID      "0000a100-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_DATA_UUID    "0000a101-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_STATUS_UUID  "0000a102-0000-1000-8000-00805f9b34fb"

static NimBLEServer* sServer = nullptr;
static NimBLECharacteristic* sDataChar = nullptr;
static NimBLECharacteristic* sStatusChar = nullptr;
static bool sBleEnabled = true;
static bool sStackInited = false;
static bool sConnected = false;
static uint32_t sLastAdvRestart = 0;

static constexpr int MAX_CAL_EVENTS = 12;
static CalendarEvent sCalendarEvents[MAX_CAL_EVENTS];
static uint8_t sNumCalEvents = 0;
static portMUX_TYPE sCalMux = portMUX_INITIALIZER_UNLOCKED;

static MediaState sMediaState = {false, "YouTube Music", "Bereit", 50};

static uint32_t parseJsonUint(const char* json, const char* key, uint32_t defVal = 0) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return defVal;
    p = strchr(p, ':');
    if (!p) return defVal;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return (uint32_t)strtoul(p, nullptr, 10);
}

static bool parseJsonString(const char* json, const char* key, char* out, size_t maxLen) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < maxLen) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return true;
}

static void loadCalendarFromNvs() {
    Preferences p;
    if (p.begin("kronos_cal", true)) {
        sNumCalEvents = min<uint8_t>(p.getUChar("cnt", 0), MAX_CAL_EVENTS);
        if (sNumCalEvents > 0) {
            p.getBytes("events", sCalendarEvents, sNumCalEvents * sizeof(CalendarEvent));
        }
        p.end();
    }
}

static void saveCalendarToNvs() {
    Preferences p;
    if (p.begin("kronos_cal", false)) {
        p.putUChar("cnt", sNumCalEvents);
        if (sNumCalEvents > 0) {
            p.putBytes("events", sCalendarEvents, sNumCalEvents * sizeof(CalendarEvent));
        }
        p.end();
    }
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        sConnected = true;
        AiLimitsStore::setBleConnected(true);
        Serial.printf("[BLE] Client connected (connId=%u)\n", connInfo.getConnHandle());
        pServer->updateConnParams(connInfo.getConnHandle(), 24, 40, 0, 400); // 30-50ms interval, 4s timeout
        nav::markDirty(nav::PG_AI_LIMITS);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_AGENDA);
        nav::markDirty(nav::PG_APP_MEDIA);

        BleCompanion::triggerCalendarSync();
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        sConnected = false;
        AiLimitsStore::setBleConnected(false);
        Serial.printf("[BLE] Client disconnected (reason=%d)\n", reason);
        nav::markDirty(nav::PG_AI_LIMITS);
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_AGENDA);
        if (sBleEnabled) {
            NimBLEDevice::startAdvertising();
        }
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pCharacteristic->getValue();
        size_t len = val.length();
        if (len == 0) return;

        const uint8_t* bytes = val.data();
        bool updatedLimits = false;

        if (bytes[0] == 0xA1) {
            updatedLimits = AiLimitsStore::updateFromBleBinary(bytes, len);
            Serial.printf("[BLE] Received Quota binary (%u bytes) -> %s\n", (unsigned)len, updatedLimits ? "OK" : "ERR");
        } else if (bytes[0] == 0xC1 && len >= 3) {
            uint8_t count = min<uint8_t>(bytes[2], MAX_CAL_EVENTS);
            portENTER_CRITICAL(&sCalMux);
            sNumCalEvents = 0;
            size_t off = 3;
            for (uint8_t i = 0; i < count && off + 56 <= len; i++) {
                CalendarEvent& ev = sCalendarEvents[sNumCalEvents++];
                ev.startEpoch = *(const uint32_t*)(bytes + off); off += 4;
                ev.endEpoch   = *(const uint32_t*)(bytes + off); off += 4;
                ev.allDay     = (bytes[off++] != 0);
                memcpy(ev.title, bytes + off, 32); ev.title[32] = 0; off += 32;
                memcpy(ev.loc, bytes + off, 15);   ev.loc[15] = 0;   off += 15;
            }
            portEXIT_CRITICAL(&sCalMux);
            saveCalendarToNvs();
            nav::markDirty(nav::PG_AGENDA);
            DisplayEngine::showNotification("Google Kalender", "Termine synchronisiert", 2000, "calendar_month");
            Serial.printf("[BLE] Received binary calendar (%u events)\n", sNumCalEvents);
        } else if (bytes[0] == '{') {
            String jsonStr((const char*)bytes, len);
            if (jsonStr.indexOf("\"cal\"") >= 0) {
                const char* p = strstr(jsonStr.c_str(), "\"cal\"");
                if (p) p = strchr(p, '[');
                if (p) {
                    portENTER_CRITICAL(&sCalMux);
                    sNumCalEvents = 0;
                    while ((p = strchr(p, '{')) != nullptr && sNumCalEvents < MAX_CAL_EVENTS) {
                        const char* endObj = strchr(p, '}');
                        if (!endObj) break;
                        CalendarEvent& ev = sCalendarEvents[sNumCalEvents++];
                        ev.title[0] = 0;
                        ev.loc[0] = 0;
                        parseJsonString(p, "t", ev.title, sizeof(ev.title));
                        parseJsonString(p, "l", ev.loc, sizeof(ev.loc));
                        ev.startEpoch = parseJsonUint(p, "s", 0);
                        ev.endEpoch   = parseJsonUint(p, "e", 0);
                        ev.allDay     = (parseJsonUint(p, "a", 0) != 0);
                        p = endObj + 1;
                    }
                    portEXIT_CRITICAL(&sCalMux);
                    saveCalendarToNvs();
                    nav::markDirty(nav::PG_AGENDA);
                    DisplayEngine::showNotification("Google Kalender", "Termine synchronisiert", 2000, "calendar_month");
                    Serial.printf("[BLE] Received JSON calendar (%u events)\n", sNumCalEvents);
                }
            } else if (jsonStr.indexOf("\"media\"") >= 0) {
                char title[40] = {0};
                char artist[28] = {0};
                parseJsonString(jsonStr.c_str(), "title", title, sizeof(title));
                parseJsonString(jsonStr.c_str(), "artist", artist, sizeof(artist));
                bool isPlay = (parseJsonUint(jsonStr.c_str(), "play", 0) != 0);
                uint8_t vol = (uint8_t)parseJsonUint(jsonStr.c_str(), "vol", 50);
                BleCompanion::setMediaState(isPlay, title[0] ? title : "YouTube Music", artist, vol);
            } else {
                updatedLimits = AiLimitsStore::updateFromJson(jsonStr.c_str());
                Serial.printf("[BLE] Received JSON payload (%u bytes) -> %s\n", (unsigned)len, updatedLimits ? "OK" : "ERR");
            }
        }

        if (updatedLimits) {
            nav::markDirty(nav::PG_AI_LIMITS);
            BleCompanion::notifyStatus();
        }
    }
};

static bool ensureStack() {
    if (sStackInited) return true;
    Serial.println("[BLE] Initializing NimBLE stack...");
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    sServer = NimBLEDevice::createServer();
    sServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = sServer->createService(SERVICE_AI_LIMITS_UUID);
    if (!pService) {
        Serial.println("[BLE] Failed to create service!");
        return false;
    }

    sDataChar = pService->createCharacteristic(
        CHAR_AI_LIMITS_DATA_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
    );
    sDataChar->setCallbacks(new DataCallbacks());

    sStatusChar = pService->createCharacteristic(
        CHAR_AI_LIMITS_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pService->start();

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
    if (sBleEnabled) {
        if (ensureStack()) {
            NimBLEDevice::getAdvertising()->start();
            Serial.println("[BLE] NimBLE advertising started as 'WatchOS Ultra'");
        }
    } else {
        Serial.println("[BLE] Disabled by user settings (stack deferred)");
    }
    return true;
}

void BleCompanion::loop(unsigned long now) {
    if (!sBleEnabled || !sStackInited) return;

    if (!sConnected && (now - sLastAdvRestart >= 10000)) {
        sLastAdvRestart = now;
        if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::startAdvertising();
        }
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
    if (sConnected && sStatusChar) {
        sStatusChar->setValue((const uint8_t*)cmd, strlen(cmd));
        sStatusChar->notify();
        Serial.printf("[BLE] Media command sent: %s\n", cmd);
    } else {
        Serial.printf("[BLE] Media command buffered/dropped (not connected): %s\n", cmd);
        DisplayEngine::showNotification("Bluetooth", "Handy nicht per BLE verbunden", 2000, "phone_android");
    }
}

void BleCompanion::triggerCalendarSync() {
    sendMediaCommand("CMD:CAL_SYNC");
}

const MediaState& BleCompanion::getMediaState() {
    return sMediaState;
}

void BleCompanion::setMediaState(bool isPlaying, const char* title, const char* artist, uint8_t volumePct) {
    sMediaState.isPlaying = isPlaying;
    if (title && title[0]) {
        strncpy(sMediaState.title, title, sizeof(sMediaState.title) - 1);
        sMediaState.title[sizeof(sMediaState.title) - 1] = 0;
    }
    if (artist && artist[0]) {
        strncpy(sMediaState.artist, artist, sizeof(sMediaState.artist) - 1);
        sMediaState.artist[sizeof(sMediaState.artist) - 1] = 0;
    }
    sMediaState.volumePct = volumePct;
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
    const char* p = strstr(json, "\"cal\"");
    if (!p) p = strstr(json, "\"events\"");
    if (!p) p = json;
    p = strchr(p, '[');
    if (!p) return;

    portENTER_CRITICAL(&sCalMux);
    sNumCalEvents = 0;
    while ((p = strchr(p, '{')) != nullptr && sNumCalEvents < MAX_CAL_EVENTS) {
        const char* endObj = strchr(p, '}');
        if (!endObj) break;
        CalendarEvent& ev = sCalendarEvents[sNumCalEvents++];
        ev.title[0] = 0;
        ev.loc[0] = 0;
        parseJsonString(p, "t", ev.title, sizeof(ev.title));
        if (!ev.title[0]) parseJsonString(p, "title", ev.title, sizeof(ev.title));
        parseJsonString(p, "l", ev.loc, sizeof(ev.loc));
        if (!ev.loc[0]) parseJsonString(p, "location", ev.loc, sizeof(ev.loc));
        ev.startEpoch = parseJsonUint(p, "s", 0);
        if (ev.startEpoch == 0) ev.startEpoch = parseJsonUint(p, "start", 0);
        ev.endEpoch   = parseJsonUint(p, "e", 0);
        if (ev.endEpoch == 0) ev.endEpoch = parseJsonUint(p, "end", 0);
        ev.allDay     = (parseJsonUint(p, "a", 0) != 0);
        p = endObj + 1;
    }
    portEXIT_CRITICAL(&sCalMux);
    saveCalendarToNvs();
    nav::markDirty(nav::PG_AGENDA);
    DisplayEngine::showNotification("Kalender Sync", "Termine erfolgreich synchronisiert!", 2500, "calendar_month");
    Serial.printf("[CALENDAR] Synced %u events via JSON\n", sNumCalEvents);
}
