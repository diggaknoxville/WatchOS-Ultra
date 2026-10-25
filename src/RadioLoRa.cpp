#include "RadioLoRa.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "GpsManager.h"
#include "DisplayEngine.h"
#include "AudioEngine.h"
#include "CompanionServer.h"
#include "ui/Navigator.h"
#include <RadioLib.h>
#include <LilyGoLib.h>   // provides the global `radio` (SX1262), begun by instance
#include <Preferences.h>

#define INBOX_LEN 8
#define BEACON_MS 60000UL

static bool ready = false;
static bool sSleeping = false;
static uint32_t sentCount = 0, recvCount = 0;
static float lastRssi = 0, lastSnr = 0;

static LoraMessage inbox[INBOX_LEN];      // ring, newest at inboxHead-1
static uint8_t inboxHead = 0, inboxCount = 0;
static portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t txQueue = nullptr;
struct TxReq { char text[96]; };

static volatile bool rxFlag = false;
static unsigned long lastBeacon = 0;

static void IRAM_ATTR onDio1() { rxFlag = true; }

static void persistInbox() {
    Preferences p;
    if (!p.begin("kronos_lora", false)) return;
    p.putBytes("inbox", inbox, sizeof(inbox));
    p.putUChar("head", inboxHead);
    p.putUChar("count", inboxCount);
    p.putUInt("rx", recvCount);
    p.putUInt("tx", sentCount);
    p.end();
}

bool RadioLoRa::init(float freq) {
    {
        Preferences p;
        if (p.begin("kronos_lora", false)) {
            p.getBytes("inbox", inbox, sizeof(inbox));
            inboxHead = p.getUChar("head", 0) % INBOX_LEN;
            inboxCount = min<uint8_t>(p.getUChar("count", 0), INBOX_LEN);
            recvCount = p.getUInt("rx", 0);
            sentCount = p.getUInt("tx", 0);
            p.end();
        }
    }
    txQueue = xQueueCreate(6, sizeof(TxReq));

    if (!(instance.getDeviceProbe() & HW_RADIO_ONLINE)) {
        Serial.println("[LORA] SX1262 not detected");
        return false;
    }
    int state = radio.setFrequency(freq);
    radio.setBandwidth(125.0f);
    radio.setSpreadingFactor(9);
    radio.setCodingRate(7);
    radio.setSyncWord(0x12);
    radio.setOutputPower(14);
    radio.setPreambleLength(8);
    radio.setCRC(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] configuration failed (%d)\n", state);
        return false;
    }
    radio.setPacketReceivedAction(onDio1);
    ready = true;
    if (SettingsStorage::get().loraEnabled) {
        radio.startReceive();
        sSleeping = false;
        Serial.printf("[LORA] SX1262 listening @ %.1f MHz SF9/BW125 (%u stored messages)\n", freq, inboxCount);
    } else {
        radio.sleep();
        sSleeping = true;
        Serial.println("[LORA] SX1262 initialized in sleep mode (disabled by user)");
    }
    return true;
}

static void storeMessage(const String& s, float rssi, float snr) {
    portENTER_CRITICAL(&inboxMux);
    LoraMessage& m = inbox[inboxHead];
    strncpy(m.text, s.c_str(), sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
    m.rssi = rssi; m.snr = snr; m.epoch = (uint32_t)time(NULL);
    inboxHead = (inboxHead + 1) % INBOX_LEN;
    if (inboxCount < INBOX_LEN) inboxCount++;
    portEXIT_CRITICAL(&inboxMux);
}

static void doTransmit(const char* text) {
    int st = radio.transmit(text);
    rxFlag = false;                          // TX-done also raises DIO1
    radio.startReceive();
    if (st == RADIOLIB_ERR_NONE) {
        sentCount++;
        Serial.printf("[LORA] TX '%s'\n", text);
    } else {
        Serial.printf("[LORA] TX failed (%d)\n", st);
        DisplayEngine::showNotification("LoRa", "Transmit failed");
    }
}

void RadioLoRa::loop(unsigned long now) {
    if (!ready) return;

    bool wantEn = SettingsStorage::get().loraEnabled;
    if (!wantEn) {
        if (!sSleeping) {
            rxFlag = false;
            radio.sleep();
            sSleeping = true;
            Serial.println("[LORA] SX1262 sleep (disabled)");
        }
        return;
    } else if (sSleeping) {
        sSleeping = false;
        radio.standby();
        radio.startReceive();
        Serial.println("[LORA] SX1262 wake (receiving)");
    }

    if (rxFlag) {
        rxFlag = false;
        String s;
        int st = radio.readData(s);
        if (st == RADIOLIB_ERR_NONE && s.length()) {
            lastRssi = radio.getRSSI();
            lastSnr = radio.getSNR();
            recvCount++;
            storeMessage(s, lastRssi, lastSnr);
            persistInbox();
            Serial.printf("[LORA] RX '%s' RSSI %.0f SNR %.1f\n", s.c_str(), lastRssi, lastSnr);
            char t[40];
            snprintf(t, sizeof(t), "LoRa  %.0f dBm", lastRssi);
            DisplayEngine::showNotification(t, s, 6000);
            AudioEngine::playSoundFx(s.startsWith("SOS") ? SFX_SOS : SFX_NOTIFY);
        }
        radio.startReceive();
    }

    TxReq req;
    while (xQueueReceive(txQueue, &req, 0) == pdTRUE) {
        doTransmit(req.text);
        persistInbox();
    }

    if (SettingsStorage::get().loraBeaconEnabled && now - lastBeacon > BEACON_MS) {
        lastBeacon = now;
        char b[96];
        if (GpsManager::hasLastFix())
            snprintf(b, sizeof(b), "BCN|%s|%d%%|%.5f,%.5f", CompanionServer::getApName(),
                     PowerManager::getBatteryPercent(), GpsManager::latitude(), GpsManager::longitude());
        else
            snprintf(b, sizeof(b), "BCN|%s|%d%%", CompanionServer::getApName(), PowerManager::getBatteryPercent());
        doTransmit(b);
    }
}

bool RadioLoRa::sendPacket(const String& msg) {
    if (!ready || !txQueue) return false;
    if (!SettingsStorage::get().loraEnabled) {
        RadioLoRa::setEnabled(true);
    }
    TxReq r;
    strncpy(r.text, msg.c_str(), sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    return xQueueSend(txQueue, &r, 0) == pdTRUE;
}

bool RadioLoRa::sendSosBeacon() {
    char b[96];
    if (GpsManager::hasLastFix())
        snprintf(b, sizeof(b), "SOS|%s|%.5f,%.5f|%d%%", CompanionServer::getApName(),
                 GpsManager::latitude(), GpsManager::longitude(), PowerManager::getBatteryPercent());
    else
        snprintf(b, sizeof(b), "SOS|%s|NOFIX|%d%%", CompanionServer::getApName(), PowerManager::getBatteryPercent());
    return sendPacket(b);
}

bool RadioLoRa::pollPacket(String&, float&, float&) { return false; }
bool RadioLoRa::isReady() { return ready && SettingsStorage::get().loraEnabled; }
void RadioLoRa::setEnabled(bool en) {
    SettingsStorage::get().loraEnabled = en;
    SettingsStorage::save();
    nav::markDirty(nav::PG_LORA);
    nav::markDirty(nav::PG_CONTROL);
    nav::markDirty(nav::PG_SYSTEM);
}
bool RadioLoRa::isEnabled() { return SettingsStorage::get().loraEnabled; }
uint32_t RadioLoRa::getSentCount() { return sentCount; }
uint32_t RadioLoRa::getRecvCount() { return recvCount; }
float RadioLoRa::getLastRssi() { return lastRssi; }
float RadioLoRa::getLastSnr() { return lastSnr; }

String RadioLoRa::getLastReceivedMessage() {
    LoraMessage m;
    return getInbox(&m, 1) ? String(m.text) : String("No packets yet");
}

uint8_t RadioLoRa::getInbox(LoraMessage* out, uint8_t max) {
    portENTER_CRITICAL(&inboxMux);
    uint8_t n = min(max, inboxCount);
    for (uint8_t i = 0; i < n; i++) {
        out[i] = inbox[(inboxHead + INBOX_LEN - 1 - i) % INBOX_LEN];
    }
    portEXIT_CRITICAL(&inboxMux);
    return n;
}
