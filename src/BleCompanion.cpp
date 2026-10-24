#include "BleCompanion.h"
#include "AiLimitsStore.h"
#include "ui/Navigator.h"
#include <NimBLEDevice.h>

#define BLE_DEVICE_NAME             "WatchOS Ultra"
#define SERVICE_AI_LIMITS_UUID      "0000a100-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_DATA_UUID    "0000a101-0000-1000-8000-00805f9b34fb"
#define CHAR_AI_LIMITS_STATUS_UUID  "0000a102-0000-1000-8000-00805f9b34fb"

static NimBLEServer* sServer = nullptr;
static NimBLECharacteristic* sDataChar = nullptr;
static NimBLECharacteristic* sStatusChar = nullptr;
static bool sBleEnabled = true;
static bool sConnected = false;
static uint32_t sLastAdvRestart = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        sConnected = true;
        AiLimitsStore::setBleConnected(true);
        Serial.printf("[BLE] Client connected (connId=%u)\n", connInfo.getConnHandle());
        nav::markDirty(nav::PG_AI_LIMITS);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        sConnected = false;
        AiLimitsStore::setBleConnected(false);
        Serial.printf("[BLE] Client disconnected (reason=%d), restarting advertising...\n", reason);
        nav::markDirty(nav::PG_AI_LIMITS);
        NimBLEDevice::startAdvertising();
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue val = pCharacteristic->getValue();
        size_t len = val.length();
        if (len == 0) return;

        const uint8_t* bytes = val.data();
        bool updated = false;

        if (bytes[0] == 0xA1) {
            updated = AiLimitsStore::updateFromBleBinary(bytes, len);
            Serial.printf("[BLE] Received binary packet (%u bytes) -> %s\n", (unsigned)len, updated ? "OK" : "ERR");
        } else if (bytes[0] == '{') {
            String jsonStr((const char*)bytes, len);
            updated = AiLimitsStore::updateFromJson(jsonStr.c_str());
            Serial.printf("[BLE] Received JSON payload (%u bytes) -> %s\n", (unsigned)len, updated ? "OK" : "ERR");
        }

        if (updated) {
            nav::markDirty(nav::PG_AI_LIMITS);
            BleCompanion::notifyStatus();
        }
    }
};

bool BleCompanion::init() {
    Serial.println("[BLE] Initializing NimBLE stack...");
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

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
    pAdvertising->start();

    Serial.println("[BLE] NimBLE advertising started as 'WatchOS Ultra'");
    return true;
}

void BleCompanion::loop(unsigned long now) {
    if (!sBleEnabled) return;

    if (!sConnected && (now - sLastAdvRestart >= 10000)) {
        sLastAdvRestart = now;
        if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
            NimBLEDevice::startAdvertising();
        }
    }
}

bool BleCompanion::isConnected() {
    return sConnected;
}

void BleCompanion::setEnabled(bool en) {
    sBleEnabled = en;
    if (!en && NimBLEDevice::getAdvertising()->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
    } else if (en && !sConnected && !NimBLEDevice::getAdvertising()->isAdvertising()) {
        NimBLEDevice::startAdvertising();
    }
}

bool BleCompanion::isEnabled() {
    return sBleEnabled;
}

void BleCompanion::notifyStatus() {
    if (sConnected && sStatusChar) {
        char statusMsg[32];
        snprintf(statusMsg, sizeof(statusMsg), "ACK:%lu", (unsigned long)time(nullptr));
        sStatusChar->setValue(statusMsg);
        sStatusChar->notify();
    }
}

