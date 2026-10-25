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
#include <mbedtls/aes.h>

#define INBOX_LEN 8
#define MAX_PEERS 16
#define BEACON_MS 60000UL

static bool ready = false;
static bool sSleeping = false;
static bool sMeshMode = true;    // Default to Meshtastic EU_868 + KRONOS hybrid support
static volatile bool sReconfigRadio = false;
static uint32_t sentCount = 0, recvCount = 0;
static float lastRssi = 0, lastSnr = 0;

static LoraMessage inbox[INBOX_LEN];      // ring, newest at inboxHead-1
static uint8_t inboxHead = 0, inboxCount = 0;
static portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;

static LoraPeerNode sPeers[MAX_PEERS];
static uint8_t sPeerCount = 0;

static QueueHandle_t txQueue = nullptr;
struct TxReq {
    char text[96];
    bool isMeshPos;
    double lat;
    double lon;
};

static volatile bool rxFlag = false;
static unsigned long lastBeacon = 0;

// Default Meshtastic LongFast PSK ("AQ==" expanded per Meshtastic spec)
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static void IRAM_ATTR onDio1() { rxFlag = true; }

static uint32_t ourNodeId() {
    uint64_t mac = ESP.getEfuseMac();
    return (uint32_t)((mac >> 16) ^ (mac & 0xFFFFFFFFULL));
}

static void updatePeer(uint32_t nodeId, const char* name, bool hasPos, double lat, double lon,
                       float rssi, float snr, bool isSos, bool isMesh) {
    portENTER_CRITICAL(&inboxMux);
    int idx = -1;
    for (int i = 0; i < sPeerCount; i++) {
        if (sPeers[i].nodeId == nodeId || (name && name[0] && strcmp(sPeers[i].name, name) == 0)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (sPeerCount < MAX_PEERS) {
            idx = sPeerCount++;
        } else {
            // Replace oldest
            idx = 0;
            for (int i = 1; i < MAX_PEERS; i++) {
                if (sPeers[i].epoch < sPeers[idx].epoch) idx = i;
            }
        }
        memset(&sPeers[idx], 0, sizeof(LoraPeerNode));
        sPeers[idx].nodeId = nodeId;
    }
    if (name && name[0]) {
        strncpy(sPeers[idx].name, name, sizeof(sPeers[idx].name) - 1);
        sPeers[idx].name[sizeof(sPeers[idx].name) - 1] = 0;
    } else if (!sPeers[idx].name[0]) {
        snprintf(sPeers[idx].name, sizeof(sPeers[idx].name), "!%04x", (unsigned)(nodeId & 0xFFFF));
    }
    if (hasPos && fabs(lat) > 0.001 && fabs(lon) > 0.001) {
        sPeers[idx].lat = lat;
        sPeers[idx].lon = lon;
        sPeers[idx].hasPos = true;
    }
    sPeers[idx].rssi = rssi;
    sPeers[idx].snr = snr;
    sPeers[idx].epoch = (uint32_t)time(NULL);
    sPeers[idx].isSos = isSos;
    sPeers[idx].isMeshtastic = isMesh;
    portEXIT_CRITICAL(&inboxMux);
    nav::markDirty(nav::PG_POSITION);
    nav::markDirty(nav::PG_LORA);
}

static void persistInbox() {
    Preferences p;
    if (!p.begin("kronos_lora", false)) return;
    p.putBytes("inbox", inbox, sizeof(inbox));
    p.putUChar("head", inboxHead);
    p.putUChar("count", inboxCount);
    p.putUInt("rx", recvCount);
    p.putUInt("tx", sentCount);
    p.putBool("mesh", sMeshMode);
    p.end();
}

static void configureRadioParams() {
    if (sMeshMode) {
        // Meshtastic EU_868 LongFast: 869.525 MHz, BW 250 kHz, SF11, CR 4/5, SyncWord 0x2B, Preamble 16
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(11);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(17);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else {
        // KRONOS Direct: 868.0 MHz, BW 125 kHz, SF9, CR 4/7, SyncWord 0x12, Preamble 8
        radio.setFrequency(868.0f);
        radio.setBandwidth(125.0f);
        radio.setSpreadingFactor(9);
        radio.setCodingRate(7);
        radio.setSyncWord(0x12);
        radio.setOutputPower(14);
        radio.setPreambleLength(8);
        radio.setCRC(true);
    }
}

bool RadioLoRa::init(float freq) {
    (void)freq;
    {
        Preferences p;
        if (p.begin("kronos_lora", false)) {
            p.getBytes("inbox", inbox, sizeof(inbox));
            inboxHead = p.getUChar("head", 0) % INBOX_LEN;
            inboxCount = min<uint8_t>(p.getUChar("count", 0), INBOX_LEN);
            recvCount = p.getUInt("rx", 0);
            sentCount = p.getUInt("tx", 0);
            sMeshMode = p.getBool("mesh", true);
            p.end();
        }
    }
    txQueue = xQueueCreate(6, sizeof(TxReq));

    if (!(instance.getDeviceProbe() & HW_RADIO_ONLINE)) {
        Serial.println("[LORA] SX1262 not detected");
        return false;
    }
    configureRadioParams();
    radio.setPacketReceivedAction(onDio1);
    ready = true;
    if (SettingsStorage::get().loraEnabled) {
        radio.startReceive();
        sSleeping = false;
        Serial.printf("[LORA] SX1262 listening in %s mode (%u stored messages)\n",
                      sMeshMode ? "Meshtastic EU_868 (869.525 MHz)" : "KRONOS (868.0 MHz)", inboxCount);
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

// Parse KRONOS ASCII packet:
//  BCN|<name>|<bat>%|<lat>,<lon>
//  SOS|<name>|<lat>,<lon>|<bat>%
//  NAV|<name>|<lat>,<lon>
static void parseAsciiPacket(const String& s, float rssi, float snr) {
    if (s.startsWith("BCN|") || s.startsWith("SOS|") || s.startsWith("NAV|")) {
        bool isSos = s.startsWith("SOS|");
        bool isNav = s.startsWith("NAV|");
        int p1 = s.indexOf('|');
        int p2 = s.indexOf('|', p1 + 1);
        if (p1 > 0 && p2 > p1) {
            String name = s.substring(p1 + 1, p2);
            String rest = s.substring(p2 + 1);
            double lat = 0, lon = 0;
            bool hasCoord = false;
            int comma = rest.indexOf(',');
            if (comma > 0) {
                int start = rest.lastIndexOf('|', comma);
                String latStr = rest.substring(start >= 0 ? start + 1 : 0, comma);
                int end = rest.indexOf('|', comma);
                String lonStr = end >= 0 ? rest.substring(comma + 1, end) : rest.substring(comma + 1);
                lat = latStr.toDouble();
                lon = lonStr.toDouble();
                hasCoord = (fabs(lat) > 0.01 && fabs(lon) > 0.01);
            }
            uint32_t hash = 0x811C9DC5u;
            for (unsigned int i = 0; i < name.length(); i++) hash = (hash ^ (uint8_t)name[i]) * 0x01000193u;
            updatePeer(hash, name.c_str(), hasCoord, lat, lon, rssi, snr, isSos, false);
            if (isNav && hasCoord) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Ziel %s (%.4f, %.4f)", name.c_str(), lat, lon);
                DisplayEngine::showNotification("LoRa Wegpunkt", msg, 5000, "location");
            }
        }
    }
}

// Minimal protobuf varint reader for Meshtastic Data payload
static uint64_t pbVarint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

// Decode Meshtastic OTA frame (16-byte header + AES-128 CTR encrypted Data protobuf)
static bool tryDecodeMeshtastic(const uint8_t* pkt, size_t len, float rssi, float snr) {
    if (len < 18 || len > 255) return false;
    uint32_t destNode = *(const uint32_t*)(pkt + 0);
    uint32_t fromNode = *(const uint32_t*)(pkt + 4);
    uint32_t packetId = *(const uint32_t*)(pkt + 8);
    (void)destNode;
    if (fromNode == 0 || fromNode == 0xFFFFFFFFUL) return false;

    // AES-128 CTR nonce: [packetId (8 bytes LE), fromNode (8 bytes LE)]
    uint8_t nonce[16] = {0};
    memcpy(nonce + 0, &packetId, 4);
    memcpy(nonce + 8, &fromNode, 4);

    uint8_t plain[240];
    size_t payLen = len - 16;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, MESH_DEFAULT_PSK, 128);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    mbedtls_aes_crypt_ctr(&aes, payLen, &nc_off, nonce, stream_block, pkt + 16, plain);
    mbedtls_aes_free(&aes);

    // Parse Data protobuf: field 1 = portnum (varint), field 2 = payload (bytes)
    const uint8_t *p = plain, *end = plain + payLen;
    uint32_t portnum = 0;
    const uint8_t* dataPtr = nullptr;
    size_t dataLen = 0;
    while (p < end) {
        uint64_t tag = pbVarint(p, end);
        uint32_t field = tag >> 3, wt = tag & 7;
        if (field == 0 || field > 20) return false;
        if (field == 1 && wt == 0) {
            portnum = (uint32_t)pbVarint(p, end);
        } else if (field == 2 && wt == 2) {
            uint64_t l = pbVarint(p, end);
            if (p + l > end) return false;
            dataPtr = p;
            dataLen = (size_t)l;
            p += l;
        } else if (wt == 0) pbVarint(p, end);
        else if (wt == 2) { uint64_t l = pbVarint(p, end); if (p + l > end) return false; p += l; }
        else if (wt == 5) { if (p + 4 > end) return false; p += 4; }
        else if (wt == 1) { if (p + 8 > end) return false; p += 8; }
        else return false;
    }

    if (!dataPtr || portnum == 0) return false;

    if (portnum == 1) { // TEXT_MESSAGE_APP
        char txt[96];
        size_t clen = min(dataLen, sizeof(txt) - 16);
        snprintf(txt, sizeof(txt), "[!%04x] ", (unsigned)(fromNode & 0xFFFF));
        size_t prefixLen = strlen(txt);
        memcpy(txt + prefixLen, dataPtr, clen);
        txt[prefixLen + clen] = 0;
        updatePeer(fromNode, nullptr, false, 0, 0, rssi, snr, false, true);
        storeMessage(String(txt), rssi, snr);
        persistInbox();
        DisplayEngine::showNotification("Meshtastic", txt, 5000);
        return true;
    } else if (portnum == 3) { // POSITION_APP
        const uint8_t *pp = dataPtr, *pend = dataPtr + dataLen;
        int32_t latI = 0, lonI = 0;
        while (pp < pend) {
            uint64_t tag = pbVarint(pp, pend);
            uint32_t field = tag >> 3, wt = tag & 7;
            if (field == 1 && wt == 5 && pp + 4 <= pend) { memcpy(&latI, pp, 4); pp += 4; }
            else if (field == 2 && wt == 5 && pp + 4 <= pend) { memcpy(&lonI, pp, 4); pp += 4; }
            else if (wt == 0) pbVarint(pp, pend);
            else if (wt == 2) { uint64_t l = pbVarint(pp, pend); if (pp + l > pend) break; pp += l; }
            else if (wt == 5) { if (pp + 4 > pend) break; pp += 4; }
            else if (wt == 1) { if (pp + 8 > pend) break; pp += 8; }
            else break;
        }
        if (latI != 0 && lonI != 0) {
            double lat = latI * 1e-7;
            double lon = lonI * 1e-7;
            updatePeer(fromNode, nullptr, true, lat, lon, rssi, snr, false, true);
            char msg[72];
            snprintf(msg, sizeof(msg), "MESH POS !%04x: %.4f, %.4f", (unsigned)(fromNode & 0xFFFF), lat, lon);
            storeMessage(String(msg), rssi, snr);
            return true;
        }
    } else if (portnum == 4) { // NODEINFO_APP (User protobuf)
        const uint8_t *up = dataPtr, *uend = dataPtr + dataLen;
        char shortName[16] = {0};
        char longName[24] = {0};
        while (up < uend) {
            uint64_t tag = pbVarint(up, uend);
            uint32_t field = tag >> 3, wt = tag & 7;
            if (field == 2 && wt == 2) {
                uint64_t l = pbVarint(up, uend);
                if (up + l > uend) break;
                size_t cl = min<size_t>((size_t)l, sizeof(longName) - 1);
                memcpy(longName, up, cl); longName[cl] = 0;
                up += l;
            } else if (field == 3 && wt == 2) {
                uint64_t l = pbVarint(up, uend);
                if (up + l > uend) break;
                size_t cl = min<size_t>((size_t)l, sizeof(shortName) - 1);
                memcpy(shortName, up, cl); shortName[cl] = 0;
                up += l;
            } else if (wt == 0) pbVarint(up, uend);
            else if (wt == 2) { uint64_t l = pbVarint(up, uend); if (up + l > uend) break; up += l; }
            else if (wt == 5) { if (up + 4 > uend) break; up += 4; }
            else if (wt == 1) { if (up + 8 > uend) break; up += 8; }
            else break;
        }
        const char* nm = shortName[0] ? shortName : (longName[0] ? longName : nullptr);
        updatePeer(fromNode, nm, false, 0, 0, rssi, snr, false, true);
        return true;
    }
    return false;
}

// Encode and transmit a Meshtastic LongFast packet (either TEXT_MESSAGE_APP or POSITION_APP)
static void transmitMeshtasticFrame(uint32_t portnum, const uint8_t* appData, size_t appLen) {
    uint8_t plain[200];
    size_t idx = 0;
    // field 1 (portnum), varint
    plain[idx++] = (1 << 3) | 0;
    plain[idx++] = (uint8_t)portnum;
    // field 2 (payload), bytes
    plain[idx++] = (2 << 3) | 2;
    plain[idx++] = (uint8_t)appLen;
    memcpy(plain + idx, appData, appLen);
    idx += appLen;

    uint8_t pkt[220];
    uint32_t dest = 0xFFFFFFFFUL;
    uint32_t from = ourNodeId();
    uint32_t pktId = (uint32_t)esp_random();
    memcpy(pkt + 0, &dest, 4);
    memcpy(pkt + 4, &from, 4);
    memcpy(pkt + 8, &pktId, 4);
    pkt[12] = (3 << 5) | 3; // hop_start=3, hop_limit=3
    pkt[13] = 0x08;         // LongFast default channel hash
    pkt[14] = 0;
    pkt[15] = 0;

    uint8_t nonce[16] = {0};
    memcpy(nonce + 0, &pktId, 4);
    memcpy(nonce + 8, &from, 4);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, MESH_DEFAULT_PSK, 128);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    mbedtls_aes_crypt_ctr(&aes, idx, &nc_off, nonce, stream_block, plain, pkt + 16);
    mbedtls_aes_free(&aes);

    int st = radio.transmit(pkt, 16 + idx);
    rxFlag = false;
    radio.startReceive();
    if (st == RADIOLIB_ERR_NONE) {
        sentCount++;
        Serial.printf("[LORA] Meshtastic TX port=%u (%u bytes)\n", (unsigned)portnum, (unsigned)(16 + idx));
    }
}

static void doTransmit(const TxReq& req) {
    if (sMeshMode && req.isMeshPos) {
        // Encode Meshtastic POSITION_APP protobuf: field 1 sfixed32 lat_i, field 2 sfixed32 lon_i
        uint8_t posPb[16];
        int32_t latI = (int32_t)(req.lat * 1e7);
        int32_t lonI = (int32_t)(req.lon * 1e7);
        posPb[0] = (1 << 3) | 5;
        memcpy(posPb + 1, &latI, 4);
        posPb[5] = (2 << 3) | 5;
        memcpy(posPb + 6, &lonI, 4);
        transmitMeshtasticFrame(3, posPb, 10);
        return;
    }
    if (sMeshMode && strncmp(req.text, "NAV|", 4) != 0 && strncmp(req.text, "SOS|", 4) != 0) {
        transmitMeshtasticFrame(1, (const uint8_t*)req.text, strlen(req.text));
        return;
    }
    int st = radio.transmit(req.text);
    rxFlag = false;                          // TX-done also raises DIO1
    radio.startReceive();
    if (st == RADIOLIB_ERR_NONE) {
        sentCount++;
        Serial.printf("[LORA] TX '%s'\n", req.text);
    } else {
        Serial.printf("[LORA] TX failed (%d)\n", st);
        DisplayEngine::showNotification("LoRa", "Transmit failed");
    }
}

void RadioLoRa::loop(unsigned long now) {
    if (!ready) return;

    if (sReconfigRadio) {
        sReconfigRadio = false;
        radio.standby();
        configureRadioParams();
        if (SettingsStorage::get().loraEnabled) {
            radio.startReceive();
            sSleeping = false;
        }
        persistInbox();
    }

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
        configureRadioParams();
        radio.startReceive();
        Serial.println("[LORA] SX1262 wake (receiving)");
    }

    if (rxFlag) {
        rxFlag = false;
        size_t pktLen = radio.getPacketLength();
        uint8_t rawBuf[256];
        if (pktLen > 0 && pktLen < sizeof(rawBuf)) {
            int st = radio.readData(rawBuf, pktLen);
            if (st == RADIOLIB_ERR_NONE) {
                lastRssi = radio.getRSSI();
                lastSnr = radio.getSNR();
                recvCount++;
                if (!tryDecodeMeshtastic(rawBuf, pktLen, lastRssi, lastSnr)) {
                    rawBuf[pktLen] = 0;
                    String s((const char*)rawBuf);
                    storeMessage(s, lastRssi, lastSnr);
                    parseAsciiPacket(s, lastRssi, lastSnr);
                    persistInbox();
                    Serial.printf("[LORA] RX '%s' RSSI %.0f SNR %.1f\n", s.c_str(), lastRssi, lastSnr);
                    char t[40];
                    snprintf(t, sizeof(t), "LoRa  %.0f dBm", lastRssi);
                    DisplayEngine::showNotification(t, s, 6000);
                    AudioEngine::playSoundFx(s.startsWith("SOS") ? SFX_SOS : SFX_NOTIFY);
                }
            }
        }
        radio.startReceive();
    }

    TxReq req;
    while (xQueueReceive(txQueue, &req, 0) == pdTRUE) {
        doTransmit(req);
        persistInbox();
    }

    if (SettingsStorage::get().loraBeaconEnabled && now - lastBeacon > BEACON_MS) {
        lastBeacon = now;
        if (sMeshMode && GpsManager::hasLastFix()) {
            TxReq r{};
            r.isMeshPos = true;
            r.lat = GpsManager::latitude();
            r.lon = GpsManager::longitude();
            doTransmit(r);
        } else {
            TxReq r{};
            r.isMeshPos = false;
            if (GpsManager::hasLastFix())
                snprintf(r.text, sizeof(r.text), "BCN|%s|%d%%|%.5f,%.5f", CompanionServer::getApName(),
                         PowerManager::getBatteryPercent(), GpsManager::latitude(), GpsManager::longitude());
            else
                snprintf(r.text, sizeof(r.text), "BCN|%s|%d%%", CompanionServer::getApName(), PowerManager::getBatteryPercent());
            doTransmit(r);
        }
    }
}

bool RadioLoRa::sendPacket(const String& msg) {
    if (!ready || !txQueue) return false;
    if (!SettingsStorage::get().loraEnabled) {
        RadioLoRa::setEnabled(true);
    }
    TxReq r{};
    r.isMeshPos = false;
    strncpy(r.text, msg.c_str(), sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    return xQueueSend(txQueue, &r, 0) == pdTRUE;
}

bool RadioLoRa::sendPositionBeacon() {
    if (!ready || !txQueue) return false;
    if (!SettingsStorage::get().loraEnabled) RadioLoRa::setEnabled(true);
    double lat = GpsManager::hasLastFix() ? GpsManager::latitude() : 52.5200;
    double lon = GpsManager::hasLastFix() ? GpsManager::longitude() : 13.4050;
    if (sMeshMode) {
        TxReq r{};
        r.isMeshPos = true;
        r.lat = lat;
        r.lon = lon;
        return xQueueSend(txQueue, &r, 0) == pdTRUE;
    } else {
        char b[96];
        snprintf(b, sizeof(b), "BCN|%s|%d%%|%.5f,%.5f", CompanionServer::getApName(),
                 PowerManager::getBatteryPercent(), lat, lon);
        return sendPacket(b);
    }
}

bool RadioLoRa::sendWaypoint(const char* name, double lat, double lon) {
    char b[96];
    snprintf(b, sizeof(b), "NAV|%s|%.5f,%.5f", name ? name : "WP", lat, lon);
    return sendPacket(b);
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

void RadioLoRa::setMeshtasticMode(bool mesh) {
    if (sMeshMode == mesh) return;
    sMeshMode = mesh;
    sReconfigRadio = true;
    nav::markDirty(nav::PG_LORA);
    nav::markDirty(nav::PG_POSITION);
}
bool RadioLoRa::isMeshtasticMode() { return sMeshMode; }

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

uint8_t RadioLoRa::getPeers(LoraPeerNode* out, uint8_t max) {
    portENTER_CRITICAL(&inboxMux);
    uint8_t n = min(max, sPeerCount);
    for (uint8_t i = 0; i < n; i++) {
        out[i] = sPeers[i];
    }
    portEXIT_CRITICAL(&inboxMux);
    return n;
}

uint8_t RadioLoRa::getPeerCount() {
    return sPeerCount;
}

bool RadioLoRa::getMeshFallbackPosition(double& outLat, double& outLon, char* outSourceName, size_t nameLen) {
    portENTER_CRITICAL(&inboxMux);
    int best = -1;
    for (int i = 0; i < sPeerCount; i++) {
        if (!sPeers[i].hasPos) continue;
        if (best < 0 || sPeers[i].rssi > sPeers[best].rssi) {
            best = i;
        }
    }
    if (best >= 0) {
        outLat = sPeers[best].lat;
        outLon = sPeers[best].lon;
        if (outSourceName && nameLen > 0) {
            strncpy(outSourceName, sPeers[best].name, nameLen - 1);
            outSourceName[nameLen - 1] = 0;
        }
        portEXIT_CRITICAL(&inboxMux);
        return true;
    }
    portEXIT_CRITICAL(&inboxMux);
    return false;
}

void RadioLoRa::injectDemoPeers() {
    if (sPeerCount > 0) return;
    double baseLat = GpsManager::hasLastFix() ? GpsManager::latitude() : 52.5200;
    double baseLon = GpsManager::hasLastFix() ? GpsManager::longitude() : 13.4050;
    updatePeer(0xA1B2C301, "MESH-REL1", true, baseLat + 0.012, baseLon + 0.018, -74.0f, 8.5f, false, true);
    updatePeer(0xA1B2C302, "ULTRA-02",  true, baseLat - 0.009, baseLon + 0.011, -82.0f, 6.2f, false, true);
    updatePeer(0xA1B2C303, "BASE-HQ",   true, baseLat + 0.021, baseLon - 0.015, -91.0f, 3.8f, false, true);
}
