#include "RadioLoRa.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "GpsManager.h"
#include "DisplayEngine.h"
#include "AudioEngine.h"
#include "CompanionServer.h"
#include "ui/Navigator.h"
#include <RadioLib.h>
#include <LilyGoLib.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>

#define INBOX_LEN 8
#define MAX_PEERS 16
#define BEACON_MS 60000UL
#define SPECTRUM_BINS 24

static bool ready = false;
static bool sSleeping = false;
static MeshPreset sPreset = MESH_PRESET_LONG_FAST;
static volatile bool sReconfigRadio = false;
static uint32_t sentCount = 0, recvCount = 0;
static float lastRssi = 0, lastSnr = 0;

static LoraMessage inbox[INBOX_LEN];
static uint8_t inboxHead = 0, inboxCount = 0;
static portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;

static LoraPeerNode sPeers[MAX_PEERS];
static uint8_t sPeerCount = 0;

// 868 MHz Spectrum Analyzer state
static volatile bool sSpectrumActive = false;
static int8_t sSpectrumDbm[SPECTRUM_BINS];
static float sPeakFreqMhz = 869.525f;
static int8_t sPeakDbm = -120;
static unsigned long sLastSpectrumSweepMs = 0;

// BLE Anti-Tracker Threat Radar state
static constexpr int MAX_THREATS = 12;
static BleThreatInfo sThreats[MAX_THREATS];
static uint8_t sThreatCount = 0;
static volatile bool sThreatScanReq = false;
static volatile bool sThreatScanRunning = false;

enum TxKind : uint8_t {
    TX_ASCII_OR_TEXT = 0,
    TX_MESH_POS      = 1,
    TX_MESH_NODEINFO = 2,
};

static QueueHandle_t txQueue = nullptr;
struct TxReq {
    TxKind kind;
    uint32_t targetNode;
    char text[96];
    double lat;
    double lon;
};

static volatile bool rxFlag = false;
static unsigned long lastBeacon = 0;
static unsigned long sLastAutoDiscoveryMs = 0;

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

static bool isLegacyFakePeerName(const char* name) {
    if (!name) return false;
    return strcmp(name, "MESH-REL1") == 0 ||
           strcmp(name, "ULTRA-02") == 0 ||
           strcmp(name, "BASE-HQ") == 0;
}

static void persistPeers();

static void updatePeer(uint32_t nodeId, const char* name, const char* shortName,
                       bool hasPos, double lat, double lon,
                       float rssi, float snr, uint8_t chanHash, uint8_t batPct,
                       bool isSos, bool isMesh) {
    if (isLegacyFakePeerName(name)) return;

    bool needNameQuery = false;
    portENTER_CRITICAL(&inboxMux);
    int idx = -1;
    for (int i = 0; i < sPeerCount; i++) {
        if (sPeers[i].nodeId == nodeId ||
            (name && name[0] && sPeers[i].hasName && strcmp(sPeers[i].name, name) == 0)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (sPeerCount < MAX_PEERS) {
            idx = sPeerCount++;
        } else {
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
        sPeers[idx].hasName = true;
    } else if (!sPeers[idx].hasName) {
        snprintf(sPeers[idx].name, sizeof(sPeers[idx].name), "!%08lx", (unsigned long)nodeId);
        needNameQuery = isMesh;
    }
    if (shortName && shortName[0]) {
        strncpy(sPeers[idx].shortName, shortName, sizeof(sPeers[idx].shortName) - 1);
        sPeers[idx].shortName[sizeof(sPeers[idx].shortName) - 1] = 0;
    }

    if (hasPos && fabs(lat) > 0.001 && fabs(lon) > 0.001) {
        sPeers[idx].lat = lat;
        sPeers[idx].lon = lon;
        sPeers[idx].hasPos = true;
    }
    if (chanHash != 0) sPeers[idx].chanHash = chanHash;
    if (batPct > 0 && batPct <= 100) sPeers[idx].batPct = batPct;
    sPeers[idx].rssi = rssi;
    sPeers[idx].snr = snr;
    sPeers[idx].epoch = (uint32_t)time(NULL);
    sPeers[idx].isSos = isSos;
    sPeers[idx].isMeshtastic = isMesh;
    portEXIT_CRITICAL(&inboxMux);

    persistPeers();
    nav::markDirty(nav::PG_POSITION);
    nav::markDirty(nav::PG_LORA);

    // If we heard a real Meshtastic node whose human-readable name isn't known yet,
    // request its NodeInfo (throttled to at most once every 20s)
    if (needNameQuery && millis() - sLastAutoDiscoveryMs > 20000) {
        sLastAutoDiscoveryMs = millis();
        RadioLoRa::sendNodeInfoDiscovery(nodeId);
    }
}

static void persistInbox() {
    Preferences p;
    if (!p.begin("kronos_lora", false)) return;
    p.putBytes("inbox", inbox, sizeof(inbox));
    p.putUChar("head", inboxHead);
    p.putUChar("count", inboxCount);
    p.putUInt("rx", recvCount);
    p.putUInt("tx", sentCount);
    p.putUChar("preset", (uint8_t)sPreset);
    p.end();
}

static void persistPeers() {
    Preferences p;
    if (!p.begin("kronos_lora", false)) return;
    p.putUChar("pcnt", sPeerCount);
    if (sPeerCount > 0) {
        p.putBytes("peers", sPeers, sPeerCount * sizeof(LoraPeerNode));
    }
    p.end();
}

static void configureRadioParams() {
    instance.setRFSwitch(false); // Route RF to internal T-Watch Ultra LoRa antenna
    if (sPreset == MESH_PRESET_LONG_FAST) {
        // Meshtastic EU_868 LongFast (Slot 1): 869.525 MHz, BW 250 kHz, SF11, CR 4/5, SyncWord 0x2B, Preamble 16
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(11);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(20);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else if (sPreset == MESH_PRESET_MED_FAST) {
        // Meshtastic EU_868 MediumFast: 869.525 MHz, BW 250 kHz, SF9, CR 4/5, SyncWord 0x2B, Preamble 16
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(9);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(20);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else if (sPreset == MESH_PRESET_SHORT_FAST) {
        // Meshtastic EU_868 ShortFast: 869.525 MHz, BW 250 kHz, SF7, CR 4/5, SyncWord 0x2B, Preamble 16
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(7);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(20);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else {
        // KRONOS Direct: 868.0 MHz, BW 125 kHz, SF9, CR 4/7, SyncWord 0x12, Preamble 8
        radio.setFrequency(868.0f);
        radio.setBandwidth(125.0f);
        radio.setSpreadingFactor(9);
        radio.setCodingRate(7);
        radio.setSyncWord(0x12);
        radio.setOutputPower(17);
        radio.setPreambleLength(8);
        radio.setCRC(true);
    }
    radio.setRxBoostedGainMode(true); // +3 dB SX1262 LNA sensitivity boost
}

bool RadioLoRa::init(float freq) {
    (void)freq;
    for (int i = 0; i < SPECTRUM_BINS; i++) sSpectrumDbm[i] = -118;

    {
        Preferences p;
        if (p.begin("kronos_lora", false)) {
            p.getBytes("inbox", inbox, sizeof(inbox));
            inboxHead = p.getUChar("head", 0) % INBOX_LEN;
            inboxCount = min<uint8_t>(p.getUChar("count", 0), INBOX_LEN);
            recvCount = p.getUInt("rx", 0);
            sentCount = p.getUInt("tx", 0);
            sPreset = (MeshPreset)(p.getUChar("preset", 0) % 4);

            // Load persisted real peers (filtering out any legacy fake demo peers)
            uint8_t savedPeers = min<uint8_t>(p.getUChar("pcnt", 0), MAX_PEERS);
            if (savedPeers > 0) {
                LoraPeerNode tmp[MAX_PEERS];
                memset(tmp, 0, sizeof(tmp));
                p.getBytes("peers", tmp, savedPeers * sizeof(LoraPeerNode));
                sPeerCount = 0;
                for (uint8_t i = 0; i < savedPeers; i++) {
                    if (tmp[i].nodeId != 0 && !isLegacyFakePeerName(tmp[i].name)) {
                        sPeers[sPeerCount++] = tmp[i];
                    }
                }
            }

            // One-time migration: ensure LoRa is enabled so the user's Meshtastic Hub is discovered immediately
            if (!p.getBool("v2_en", false)) {
                p.putBool("v2_en", true);
                SettingsStorage::get().loraEnabled = true;
                SettingsStorage::save();
            }
            p.end();
        }
    }
    txQueue = xQueueCreate(8, sizeof(TxReq));

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
        Serial.printf("[LORA] SX1262 listening in %s mode (%u real peers, %u msgs)\n",
                      getPresetLabel(), sPeerCount, inboxCount);
        // Send an initial Meshtastic NodeInfo discovery request 3s after boot
        sendNodeInfoDiscovery();
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
            updatePeer(hash, name.c_str(), nullptr, hasCoord, lat, lon, rssi, snr, 0, 0, isSos, false);
            if (isNav && hasCoord) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Ziel %s (%.4f, %.4f)", name.c_str(), lat, lon);
                DisplayEngine::showNotification("LoRa Wegpunkt", msg, 5000, "location");
            }
        }
    }
}

static uint64_t pbVarint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) break;
    }
    return v;
}

static bool tryParseMeshtasticDataProtobuf(const uint8_t* plain, size_t payLen,
                                           uint32_t& outPortnum, const uint8_t*& outDataPtr, size_t& outDataLen) {
    const uint8_t *p = plain, *end = plain + payLen;
    outPortnum = 0;
    outDataPtr = nullptr;
    outDataLen = 0;
    while (p < end) {
        uint64_t tag = pbVarint(p, end);
        uint32_t field = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
        if (field == 0 || field > 16) return false;
        if (field == 1 && wt == 0) {
            outPortnum = (uint32_t)pbVarint(p, end);
        } else if (field == 2 && wt == 2) {
            uint64_t l = pbVarint(p, end);
            if (p + l > end) return false;
            outDataPtr = p;
            outDataLen = (size_t)l;
            p += l;
        } else if (wt == 0) {
            pbVarint(p, end);
        } else if (wt == 2) {
            uint64_t l = pbVarint(p, end);
            if (p + l > end) return false;
            p += l;
        } else if (wt == 5) {
            if (p + 4 > end) return false;
            p += 4;
        } else if (wt == 1) {
            if (p + 8 > end) return false;
            p += 8;
        } else {
            return false;
        }
    }
    return (outPortnum > 0 && outPortnum <= 512 && outDataPtr != nullptr);
}

// Decode Meshtastic OTA frame (16-byte header + AES-128 CTR encrypted Data protobuf)
static bool tryDecodeMeshtastic(const uint8_t* pkt, size_t len, float rssi, float snr) {
    if (len < 18 || len > 255) return false;
    uint32_t destNode = *(const uint32_t*)(pkt + 0);
    uint32_t fromNode = *(const uint32_t*)(pkt + 4);
    uint32_t packetId = *(const uint32_t*)(pkt + 8);
    uint8_t flags     = pkt[12];
    uint8_t chanHash  = pkt[13];
    (void)destNode;

    uint8_t hopLimit = flags & 0x07;
    uint8_t hopStart = (flags >> 5) & 0x07;
    if (fromNode == 0 || fromNode == 0xFFFFFFFFUL || fromNode == ourNodeId()) return false;
    if (hopLimit > 7 || hopStart > 7) return false;

    uint8_t nonce[16] = {0};
    memcpy(nonce + 0, &packetId, 4);
    memcpy(nonce + 8, &fromNode, 4);

    uint8_t plain[240];
    size_t payLen = len - 16;
    uint32_t portnum = 0;
    const uint8_t* dataPtr = nullptr;
    size_t dataLen = 0;
    bool decrypted = false;

    // Try default PSK (AQ==, key index 1) and simple 1-byte PSKs (key index 2..10)
    for (int keyIdx = 1; keyIdx <= 10 && !decrypted; keyIdx++) {
        uint8_t key[16];
        memcpy(key, MESH_DEFAULT_PSK, 16);
        key[15] = (uint8_t)keyIdx;

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, key, 128);
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        uint8_t ncopy[16];
        memcpy(ncopy, nonce, 16);
        mbedtls_aes_crypt_ctr(&aes, payLen, &nc_off, ncopy, stream_block, pkt + 16, plain);
        mbedtls_aes_free(&aes);

        if (tryParseMeshtasticDataProtobuf(plain, payLen, portnum, dataPtr, dataLen)) {
            decrypted = true;
        }
    }

    // Also test unencrypted payload
    if (!decrypted && tryParseMeshtasticDataProtobuf(pkt + 16, payLen, portnum, dataPtr, dataLen)) {
        decrypted = true;
    }

    if (!decrypted) {
        // If we are in Meshtastic mode (SyncWord 0x2B) and the 16-byte header is structurally valid,
        // register the node ID + channel hash even if encrypted with a custom 128/256-bit key!
        if (sPreset != MESH_PRESET_KRONOS_DIR && (hopStart >= hopLimit)) {
            updatePeer(fromNode, nullptr, nullptr, false, 0, 0, rssi, snr, chanHash, 0, false, true);
            char msg[72];
            snprintf(msg, sizeof(msg), "HUB !%08lx (CH 0x%02X · verschlüsselt)",
                     (unsigned long)fromNode, (unsigned)chanHash);
            storeMessage(String(msg), rssi, snr);
            persistInbox();
            Serial.printf("[LORA] Meshtastic encrypted frame from !%08lx ch=0x%02X RSSI=%.0f SNR=%.1f\n",
                          (unsigned long)fromNode, (unsigned)chanHash, rssi, snr);
            return true;
        }
        return false;
    }

    Serial.printf("[LORA] Meshtastic RX from !%08lx portnum=%u len=%u ch=0x%02X RSSI=%.0f SNR=%.1f\n",
                  (unsigned long)fromNode, (unsigned)portnum, (unsigned)dataLen, (unsigned)chanHash, rssi, snr);

    if (portnum == 1) { // TEXT_MESSAGE_APP
        char txt[96];
        size_t clen = min(dataLen, sizeof(txt) - 18);
        snprintf(txt, sizeof(txt), "[!%04x] ", (unsigned)(fromNode & 0xFFFF));
        size_t prefixLen = strlen(txt);
        memcpy(txt + prefixLen, dataPtr, clen);
        txt[prefixLen + clen] = 0;
        updatePeer(fromNode, nullptr, nullptr, false, 0, 0, rssi, snr, chanHash, 0, false, true);
        storeMessage(String(txt), rssi, snr);
        persistInbox();
        DisplayEngine::showNotification("Meshtastic", txt, 5000);
        return true;
    } else if (portnum == 3) { // POSITION_APP
        const uint8_t *pp = dataPtr, *pend = dataPtr + dataLen;
        int32_t latI = 0, lonI = 0;
        while (pp < pend) {
            uint64_t tag = pbVarint(pp, pend);
            uint32_t field = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
            if (field == 1 && wt == 5 && pp + 4 <= pend) { memcpy(&latI, pp, 4); pp += 4; }
            else if (field == 2 && wt == 5 && pp + 4 <= pend) { memcpy(&lonI, pp, 4); pp += 4; }
            else if (wt == 0) pbVarint(pp, pend);
            else if (wt == 2) { uint64_t l = pbVarint(pp, pend); if (pp + l > pend) break; pp += l; }
            else if (wt == 5) { if (pp + 4 > pend) break; pp += 4; }
            else if (wt == 1) { if (pp + 8 > pend) break; pp += 8; }
            else break;
        }
        bool hasPos = (latI != 0 && lonI != 0);
        double lat = hasPos ? (latI * 1e-7) : 0.0;
        double lon = hasPos ? (lonI * 1e-7) : 0.0;
        updatePeer(fromNode, nullptr, nullptr, hasPos, lat, lon, rssi, snr, chanHash, 0, false, true);
        if (hasPos) {
            char msg[72];
            snprintf(msg, sizeof(msg), "POS !%04x: %.4f, %.4f", (unsigned)(fromNode & 0xFFFF), lat, lon);
            storeMessage(String(msg), rssi, snr);
            persistInbox();
        }
        return true;
    } else if (portnum == 4 || portnum == 73) { // NODEINFO_APP (4) or MAP_REPORT_APP (73)
        const uint8_t *up = dataPtr, *uend = dataPtr + dataLen;
        char shortName[12] = {0};
        char longName[24] = {0};
        int32_t latI = 0, lonI = 0;
        while (up < uend) {
            uint64_t tag = pbVarint(up, uend);
            uint32_t field = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
            if (((portnum == 4 && field == 2) || (portnum == 73 && field == 1)) && wt == 2) {
                uint64_t l = pbVarint(up, uend);
                if (up + l > uend) break;
                size_t cl = min<size_t>((size_t)l, sizeof(longName) - 1);
                memcpy(longName, up, cl); longName[cl] = 0;
                up += l;
            } else if (((portnum == 4 && field == 3) || (portnum == 73 && field == 2)) && wt == 2) {
                uint64_t l = pbVarint(up, uend);
                if (up + l > uend) break;
                size_t cl = min<size_t>((size_t)l, sizeof(shortName) - 1);
                memcpy(shortName, up, cl); shortName[cl] = 0;
                up += l;
            } else if (portnum == 73 && field == 6 && wt == 5 && up + 4 <= uend) {
                memcpy(&latI, up, 4); up += 4;
            } else if (portnum == 73 && field == 7 && wt == 5 && up + 4 <= uend) {
                memcpy(&lonI, up, 4); up += 4;
            } else if (wt == 0) pbVarint(up, uend);
            else if (wt == 2) { uint64_t l = pbVarint(up, uend); if (up + l > uend) break; up += l; }
            else if (wt == 5) { if (up + 4 > uend) break; up += 4; }
            else if (wt == 1) { if (up + 8 > uend) break; up += 8; }
            else break;
        }
        // Prefer full longName so user clearly recognizes their Hub's exact name!
        const char* bestName = longName[0] ? longName : (shortName[0] ? shortName : nullptr);
        bool hasPos = (latI != 0 && lonI != 0);
        updatePeer(fromNode, bestName, shortName, hasPos, latI * 1e-7, lonI * 1e-7,
                   rssi, snr, chanHash, 0, false, true);
        if (bestName) {
            char msg[72];
            snprintf(msg, sizeof(msg), "HUB ENTDECKT: %s (%s)", bestName,
                     shortName[0] ? shortName : "MESH");
            storeMessage(String(msg), rssi, snr);
            persistInbox();
            DisplayEngine::showNotification("Meshtastic Hub", msg, 4000, "podcasts");
        }
        return true;
    } else {
        // Any other Meshtastic portnum (e.g. 67=TELEMETRY_APP, 5=ROUTING_APP, 70=TRACEROUTE, 71=NEIGHBORINFO)
        updatePeer(fromNode, nullptr, nullptr, false, 0, 0, rssi, snr, chanHash, 0, false, true);
        char msg[72];
        const char* pName = (portnum == 67) ? "TELEMETRIE" :
                            (portnum == 71) ? "NEIGHBORINFO" :
                            (portnum == 70) ? "TRACEROUTE" :
                            (portnum == 5)  ? "ROUTING ACK" : "PAKET";
        snprintf(msg, sizeof(msg), "MESH %s von !%08lx", pName, (unsigned long)fromNode);
        storeMessage(String(msg), rssi, snr);
        persistInbox();
        return true;
    }
}

// Encode and transmit a Meshtastic frame (with optional want_response=true to trigger Hub NodeInfo reply)
static void transmitMeshtasticFrame(uint32_t destNode, uint32_t portnum,
                                    const uint8_t* appData, size_t appLen, bool wantResponse = false) {
    uint8_t plain[220];
    size_t idx = 0;
    // field 1 (portnum), varint
    plain[idx++] = (1 << 3) | 0;
    plain[idx++] = (uint8_t)portnum;
    // field 2 (payload), bytes
    plain[idx++] = (2 << 3) | 2;
    plain[idx++] = (uint8_t)appLen;
    memcpy(plain + idx, appData, appLen);
    idx += appLen;
    // field 6 (want_response), bool varint = 1
    if (wantResponse) {
        plain[idx++] = (6 << 3) | 0;
        plain[idx++] = 1;
    }

    uint8_t pkt[240];
    uint32_t from = ourNodeId();
    uint32_t pktId = (uint32_t)esp_random();
    memcpy(pkt + 0, &destNode, 4);
    memcpy(pkt + 4, &from, 4);
    memcpy(pkt + 8, &pktId, 4);
    // hop_start=3 (bits 5..7), want_ack=1 (bit 3), hop_limit=3 (bits 0..2)
    pkt[12] = (3 << 5) | (wantResponse ? (1 << 3) : 0) | 3;
    pkt[13] = 0x08; // LongFast default channel hash (0x08)
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
        Serial.printf("[LORA] Meshtastic TX dest=!%08lx port=%u wantResp=%d (%u bytes)\n",
                      (unsigned long)destNode, (unsigned)portnum, (int)wantResponse, (unsigned)(16 + idx));
    }
}

static void doTransmit(const TxReq& req) {
    bool isMesh = (sPreset != MESH_PRESET_KRONOS_DIR);
    if (isMesh && req.kind == TX_MESH_NODEINFO) {
        // Build Meshtastic User protobuf (portnum = 4 NODEINFO_APP):
        // field 1 (id string), field 2 (long_name string), field 3 (short_name string), field 5 (hw_model varint = 43)
        char idStr[16];
        snprintf(idStr, sizeof(idStr), "!%08lx", (unsigned long)ourNodeId());
        const char* longNm = "KRONOS Ultra";
        const char* shortNm = "KRON";

        uint8_t userPb[64];
        size_t u = 0;
        userPb[u++] = (1 << 3) | 2;
        userPb[u++] = (uint8_t)strlen(idStr);
        memcpy(userPb + u, idStr, strlen(idStr)); u += strlen(idStr);

        userPb[u++] = (2 << 3) | 2;
        userPb[u++] = (uint8_t)strlen(longNm);
        memcpy(userPb + u, longNm, strlen(longNm)); u += strlen(longNm);

        userPb[u++] = (3 << 3) | 2;
        userPb[u++] = (uint8_t)strlen(shortNm);
        memcpy(userPb + u, shortNm, strlen(shortNm)); u += strlen(shortNm);

        userPb[u++] = (5 << 3) | 0;
        userPb[u++] = 43; // HW_MODEL

        transmitMeshtasticFrame(req.targetNode, 4, userPb, u, true);
        return;
    }
    if (isMesh && req.kind == TX_MESH_POS) {
        uint8_t posPb[16];
        int32_t latI = (int32_t)(req.lat * 1e7);
        int32_t lonI = (int32_t)(req.lon * 1e7);
        posPb[0] = (1 << 3) | 5;
        memcpy(posPb + 1, &latI, 4);
        posPb[5] = (2 << 3) | 5;
        memcpy(posPb + 6, &lonI, 4);
        transmitMeshtasticFrame(0xFFFFFFFFUL, 3, posPb, 10, true);
        return;
    }
    if (isMesh && strncmp(req.text, "NAV|", 4) != 0 && strncmp(req.text, "SOS|", 4) != 0) {
        transmitMeshtasticFrame(0xFFFFFFFFUL, 1, (const uint8_t*)req.text, strlen(req.text), true);
        return;
    }
    int st = radio.transmit(req.text);
    rxFlag = false;
    radio.startReceive();
    if (st == RADIOLIB_ERR_NONE) {
        sentCount++;
        Serial.printf("[LORA] TX '%s'\n", req.text);
    } else {
        Serial.printf("[LORA] TX failed (%d)\n", st);
    }
}

static void runBleThreatScanSlice() {
    sThreatScanRunning = true;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan) {
        pScan->setActiveScan(true);
        pScan->setInterval(45);
        pScan->setWindow(30);
        NimBLEScanResults results = pScan->getResults(1500, false);
        int count = results.getCount();
        uint8_t outIdx = 0;

        for (int i = 0; i < count && outIdx < MAX_THREATS; i++) {
            const NimBLEAdvertisedDevice* dev = results.getDevice(i);
            if (!dev) continue;
            int rssi = dev->getRSSI();
            std::string name = dev->getName();
            std::string addr = dev->getAddress().toString();
            std::string mfg  = dev->getManufacturerData();

            bool isTracker = false;
            const char* tag = "BLE NODE";

            if (mfg.size() >= 2) {
                uint16_t cid = (uint8_t)mfg[0] | ((uint16_t)(uint8_t)mfg[1] << 8);
                if (cid == 0x004C) {
                    // Apple Company ID: 0x12 = FindMy/AirTag offline finding, 0x07 = AirPods/Proximity
                    if (mfg.size() >= 3 && ((uint8_t)mfg[2] == 0x12 || (uint8_t)mfg[2] == 0x10)) {
                        tag = "AIRTAG/FINDMY";
                        isTracker = true;
                    } else {
                        tag = "APPLE BLE";
                    }
                } else if (cid == 0x0075) {
                    tag = "SMARTTAG/GALAXY";
                    isTracker = (name.find("Tag") != std::string::npos || mfg.size() > 8);
                } else if (cid == 0xFEED) {
                    tag = "TILE TRACKER";
                    isTracker = true;
                }
            }
            if (name.find("Flipper") != std::string::npos) {
                tag = "FLIPPER ZERO";
                isTracker = true;
            } else if (name.find("AirTag") != std::string::npos || name.find("SmartTag") != std::string::npos ||
                       name.find("Tile") != std::string::npos) {
                tag = "TRACKER TAG";
                isTracker = true;
            }

            BleThreatInfo& ti = sThreats[outIdx++];
            if (!name.empty()) {
                strncpy(ti.name, name.c_str(), sizeof(ti.name) - 1);
                ti.name[sizeof(ti.name) - 1] = 0;
            } else {
                strncpy(ti.name, addr.c_str(), sizeof(ti.name) - 1);
                ti.name[sizeof(ti.name) - 1] = 0;
            }
            strncpy(ti.mac, addr.c_str(), sizeof(ti.mac) - 1);
            ti.mac[sizeof(ti.mac) - 1] = 0;
            strncpy(ti.typeTag, tag, sizeof(ti.typeTag) - 1);
            ti.typeTag[sizeof(ti.typeTag) - 1] = 0;
            ti.rssi = (int8_t)constrain(rssi, -110, -10);
            ti.isTracker = isTracker;
            ti.lastSeenMs = millis();
        }
        sThreatCount = outIdx;
        pScan->clearResults();
    }
    sThreatScanRunning = false;
    nav::markDirty(nav::PG_LORA);
}

void RadioLoRa::loop(unsigned long now) {
    if (!ready) return;

    if (sThreatScanReq) {
        sThreatScanReq = false;
        runBleThreatScanSlice();
    }

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

    // 868 MHz Spectrum Analyzer sweep (when user is viewing the SPEKTRUM tab on PG_LORA)
    if (sSpectrumActive && (now - sLastSpectrumSweepMs >= 650)) {
        sLastSpectrumSweepMs = now;
        int8_t bestDbm = -125;
        float bestFreq = 868.0f;
        for (int i = 0; i < SPECTRUM_BINS; i++) {
            float f = 863.0f + i * (7.0f / (SPECTRUM_BINS - 1));
            radio.setFrequency(f);
            radio.startReceive();
            delayMicroseconds(750);
            float rssi = radio.getRSSI(false);
            int8_t dbm = (int8_t)constrain((int)rssi, -120, -25);
            sSpectrumDbm[i] = dbm;
            if (dbm > bestDbm) {
                bestDbm = dbm;
                bestFreq = f;
            }
        }
        sPeakDbm = bestDbm;
        sPeakFreqMhz = bestFreq;
        // Restore active Meshtastic / KRONOS channel immediately
        configureRadioParams();
        rxFlag = false;
        radio.startReceive();
        nav::markDirty(nav::PG_LORA);
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
        if (sPreset != MESH_PRESET_KRONOS_DIR && GpsManager::hasLastFix()) {
            TxReq r{};
            r.kind = TX_MESH_POS;
            r.lat = GpsManager::latitude();
            r.lon = GpsManager::longitude();
            doTransmit(r);
        } else {
            TxReq r{};
            r.kind = TX_ASCII_OR_TEXT;
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
    r.kind = TX_ASCII_OR_TEXT;
    r.targetNode = 0xFFFFFFFFUL;
    strncpy(r.text, msg.c_str(), sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    return xQueueSend(txQueue, &r, 0) == pdTRUE;
}

bool RadioLoRa::sendNodeInfoDiscovery(uint32_t targetNode) {
    if (!ready || !txQueue) return false;
    if (!SettingsStorage::get().loraEnabled) {
        RadioLoRa::setEnabled(true);
    }
    TxReq r{};
    r.kind = TX_MESH_NODEINFO;
    r.targetNode = targetNode;
    return xQueueSend(txQueue, &r, 0) == pdTRUE;
}

bool RadioLoRa::sendPositionBeacon() {
    if (!ready || !txQueue) return false;
    if (!SettingsStorage::get().loraEnabled) RadioLoRa::setEnabled(true);
    double lat = GpsManager::hasLastFix() ? GpsManager::latitude() : 48.7758;
    double lon = GpsManager::hasLastFix() ? GpsManager::longitude() : 9.1829;
    if (sPreset != MESH_PRESET_KRONOS_DIR) {
        TxReq r{};
        r.kind = TX_MESH_POS;
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
    sPreset = mesh ? MESH_PRESET_LONG_FAST : MESH_PRESET_KRONOS_DIR;
    sReconfigRadio = true;
    nav::markDirty(nav::PG_LORA);
    nav::markDirty(nav::PG_POSITION);
}
bool RadioLoRa::isMeshtasticMode() { return sPreset != MESH_PRESET_KRONOS_DIR; }

void RadioLoRa::cyclePreset() {
    sPreset = (MeshPreset)(((int)sPreset + 1) % 4);
    sReconfigRadio = true;
    nav::markDirty(nav::PG_LORA);
    nav::markDirty(nav::PG_POSITION);
}

MeshPreset RadioLoRa::getPreset() { return sPreset; }

const char* RadioLoRa::getPresetLabel() {
    switch (sPreset) {
        case MESH_PRESET_LONG_FAST:  return "MESH LongFast (869.5 SF11)";
        case MESH_PRESET_MED_FAST:   return "MESH MedFast (869.5 SF9)";
        case MESH_PRESET_SHORT_FAST: return "MESH ShortFast (869.5 SF7)";
        default:                     return "KRONOS Direct (868.0 SF9)";
    }
}

uint32_t RadioLoRa::getSentCount() { return sentCount; }
uint32_t RadioLoRa::getRecvCount() { return recvCount; }
float RadioLoRa::getLastRssi() { return lastRssi; }
float RadioLoRa::getLastSnr() { return lastSnr; }

String RadioLoRa::getLastReceivedMessage() {
    LoraMessage m;
    return getInbox(&m, 1) ? String(m.text) : String("Warte auf Pakete...");
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
    // No fake demo nodes! Broadcast a real Meshtastic NodeInfo discovery request instead.
    sendNodeInfoDiscovery();
}

void RadioLoRa::setSpectrumScanActive(bool active) {
    sSpectrumActive = active;
}
bool RadioLoRa::isSpectrumScanActive() { return sSpectrumActive; }

void RadioLoRa::getSpectrumBins(int8_t* outDbm, int numBins, float& outPeakFreqMhz, int8_t& outPeakDbm) {
    int n = min(numBins, SPECTRUM_BINS);
    for (int i = 0; i < n; i++) outDbm[i] = sSpectrumDbm[i];
    outPeakFreqMhz = sPeakFreqMhz;
    outPeakDbm = sPeakDbm;
}

void RadioLoRa::triggerThreatScan() {
    if (!sThreatScanRunning) sThreatScanReq = true;
}
bool RadioLoRa::isThreatScanRunning() { return sThreatScanRunning; }

uint8_t RadioLoRa::getThreats(BleThreatInfo* out, uint8_t maxOut) {
    if (!out || maxOut == 0) return 0;
    uint8_t n = min(maxOut, sThreatCount);
    for (uint8_t i = 0; i < n; i++) out[i] = sThreats[i];
    return n;
}
