#include "RadioLoRa.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "GpsManager.h"
#include "DisplayEngine.h"
#include "AudioEngine.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
#include "RtcManager.h"
#include "ui/Navigator.h"
#include "ui/Icons.h"
#include <RadioLib.h>
#include <LilyGoLib.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <esp_random.h>
#include <math.h>

#define INBOX_LEN 8
#define MAX_PEERS 16
#define BEACON_MS 60000UL
#define SPECTRUM_BINS LORA_SPECTRUM_BINS

static const uint32_t NVS_FLUSH_MS       = 60000UL;
static const uint32_t NODEINFO_MIN_MS    = 120000UL;
static const uint32_t TX_TIMEOUT_MS      = 3000UL;
static const uint32_t DUTY_WINDOW_MS     = 3600000UL;
static const uint32_t PEER_POS_MAX_AGE_S = 1800UL;      // 30 min
static const uint32_t OWN_FIX_MAX_AGE_S  = 1800UL;      // 30 min
static const double   PEER_JUMP_KM       = 50.0;
static const uint32_t PEER_JUMP_MS       = 60000UL;

static bool ready = false;
static bool sSleeping = false;
static MeshPreset sPreset = MESH_PRESET_LONG_FAST;
static volatile bool sReconfigRadio = false;
static uint32_t sentCount = 0, recvCount = 0;
static float lastRssi = 0, lastSnr = 0;
static uint32_t sLastTxMs = 0;

static LoraMessage inbox[INBOX_LEN];
static uint8_t inboxHead = 0, inboxCount = 0;
static portMUX_TYPE inboxMux = portMUX_INITIALIZER_UNLOCKED;

static LoraPeerNode sPeers[MAX_PEERS];
static uint32_t sPeerPosEpoch[MAX_PEERS];        // UTC when the position was received (persisted)
static unsigned long sPeerPosMs[MAX_PEERS];      // millis() of last position (this session only)
static uint8_t sPeerCount = 0;

// NVS dirty flags, flushed at most every 60 s (or on request)
static bool sPeersDirty = false;
static bool sInboxDirty = false;
static volatile bool sFlushRequest = false;
static unsigned long sLastNvsFlushMs = 0;

// 868 MHz Spectrum Analyzer state (one bin per loop() pass)
static volatile bool sSpectrumActive = false;
static int8_t sSpectrumDbm[SPECTRUM_BINS];
static float sPeakFreqMhz = 869.525f;
static int8_t sPeakDbm = -120;
static unsigned long sLastSpectrumSweepMs = 0;
static int8_t sSpecBin = -1;                     // -1 = no sweep in progress
static unsigned long sSpecBinMs = 0;

// BLE Anti-Tracker Threat Radar state
static constexpr int MAX_THREATS = 12;
static BleThreatInfo sThreats[MAX_THREATS];
static uint8_t sThreatCount = 0;
static BleThreatInfo sThreatStage[MAX_THREATS];  // filled by the NimBLE scan callback
static uint8_t sThreatStageCount = 0;
static portMUX_TYPE sThreatMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool sThreatScanReq = false;
static volatile bool sThreatScanRunning = false;
static volatile bool sThreatScanDone = false;
static unsigned long sThreatScanStartMs = 0;

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

// TX state machine (non-blocking)
enum TxState : uint8_t { TXS_IDLE = 0, TXS_WAIT = 1 };
static TxState sTxState = TXS_IDLE;
static unsigned long sTxStartMs = 0;
static uint32_t sTxAirtimeMs = 0;
static uint8_t sTxBuf[256];
static size_t sTxLen = 0;
static char sTxLogText[40];
static TxReq sTxDeferred;              // dequeued but held back by the duty-cycle budget
static bool sTxHaveDeferred = false;
static unsigned long sLastDutyLogMs = 0;

// Airtime accounting (rolling 1 h window)
struct AirRec { unsigned long ms; uint32_t airMs; };
static AirRec sAir[48];
static uint8_t sAirHead = 0, sAirCount = 0;

static volatile bool rxFlag = false;
static unsigned long lastBeacon = 0;
static unsigned long sLastNodeInfoMs = 0;

// Default Meshtastic PSK ("AQ==" expanded per Meshtastic spec, key index 1)
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static void IRAM_ATTR onDio1() { rxFlag = true; }

static inline uint32_t rd_u32le(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t  rd_s32le(const uint8_t* p) { int32_t v;  memcpy(&v, p, 4); return v; }

static uint32_t ourNodeId() {
    uint64_t mac = ESP.getEfuseMac();
    return (uint32_t)((mac >> 16) ^ (mac & 0xFFFFFFFFULL));
}

static float currentFreqMhz() {
    return sPreset == MESH_PRESET_KRONOS_DIR ? 868.0f : 869.525f;
}

// XOR of the default channel name for the active preset (Meshtastic channel hash input)
static uint8_t presetNameXor() {
    switch (sPreset) {
        case MESH_PRESET_LONG_FAST:  return 0x0A;   // "LongFast"
        case MESH_PRESET_MED_FAST:   return 0x1D;   // "MediumFast"
        case MESH_PRESET_SHORT_FAST: return 0x72;   // "ShortFast"
        default:                     return 0x0A;
    }
}
// channel hash = xor(name) ^ xor(key); xor(default key with index k) = 0x03 ^ k
static uint8_t presetChannelHash() { return presetNameXor() ^ 0x03 ^ 0x01; }   // 0x08 for LongFast

static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) * sin(dLon / 2) * sin(dLon / 2);
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

// A position we may legitimately broadcast: live fix or an own fix younger than 30 min.
static bool ownPositionAllowed() {
    if (GpsManager::hasFix()) return true;
    if (!GpsManager::hasLastFix()) return false;
    time_t fixAt = GpsManager::lastFixEpoch();
    time_t now = time(NULL);
    if (fixAt <= 0 || now < fixAt) return false;
    return (uint32_t)(now - fixAt) < OWN_FIX_MAX_AGE_S;
}

// ---------------------------------------------------------------------------
//  Peer table
// ---------------------------------------------------------------------------
static void updatePeer(uint32_t nodeId, const char* name, const char* shortName,
                       bool hasPos, double lat, double lon,
                       float rssi, float snr, uint8_t chanHash, uint8_t batPct,
                       bool isSos, bool isMesh) {
    bool needNameQuery = false;
    bool posRejected = false;
    unsigned long nowMs = millis();
    uint32_t nowEpoch = (uint32_t)time(NULL);

    portENTER_CRITICAL(&inboxMux);
    int idx = -1;
    for (int i = 0; i < sPeerCount; i++) {
        if (sPeers[i].nodeId == nodeId) { idx = i; break; }
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
        sPeerPosEpoch[idx] = 0;
        sPeerPosMs[idx] = 0;
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

    if (hasPos && !(lat == 0.0 && lon == 0.0) && fabs(lat) <= 90.0 && fabs(lon) <= 180.0) {
        // Plausibility: a peer cannot move > 50 km within 60 s.
        bool plausible = true;
        if (sPeers[idx].hasPos && sPeerPosMs[idx] != 0 && (nowMs - sPeerPosMs[idx]) < PEER_JUMP_MS) {
            if (haversineKm(sPeers[idx].lat, sPeers[idx].lon, lat, lon) > PEER_JUMP_KM) plausible = false;
        }
        if (plausible) {
            sPeers[idx].lat = lat;
            sPeers[idx].lon = lon;
            sPeers[idx].hasPos = true;
            sPeerPosEpoch[idx] = nowEpoch;
            sPeerPosMs[idx] = nowMs;
        } else {
            posRejected = true;
        }
    }
    if (chanHash != 0) sPeers[idx].chanHash = chanHash;
    if (batPct > 0 && batPct <= 100) sPeers[idx].batPct = batPct;
    sPeers[idx].rssi = rssi;
    sPeers[idx].snr = snr;
    sPeers[idx].epoch = nowEpoch;
    sPeers[idx].isSos = isSos;
    sPeers[idx].isMeshtastic = isMesh;
    portEXIT_CRITICAL(&inboxMux);

    sPeersDirty = true;
    nav::markDirty(nav::PG_POSITION);
    nav::markDirty(nav::PG_LORA);
    if (posRejected) {
        Serial.printf("[LORA] Peer !%08lx position jump > %.0f km ignored\n", (unsigned long)nodeId, PEER_JUMP_KM);
    }

    // Ask an unnamed Meshtastic node for its NodeInfo (rate-limited inside)
    if (needNameQuery) RadioLoRa::sendNodeInfoDiscovery(nodeId);
}

static void flushNvs(bool force) {
    if (!force && !sPeersDirty && !sInboxDirty) return;
    Preferences p;
    if (!p.begin("kronos_lora", false)) return;
    if (sInboxDirty || force) {
        LoraMessage tmp[INBOX_LEN];
        uint8_t head, count;
        portENTER_CRITICAL(&inboxMux);
        memcpy(tmp, inbox, sizeof(tmp));
        head = inboxHead; count = inboxCount;
        portEXIT_CRITICAL(&inboxMux);
        p.putBytes("inbox", tmp, sizeof(tmp));
        p.putUChar("head", head);
        p.putUChar("count", count);
        p.putUInt("rx", recvCount);
        p.putUInt("tx", sentCount);
        p.putUChar("preset", (uint8_t)sPreset);
        sInboxDirty = false;
    }
    if (sPeersDirty || force) {
        LoraPeerNode tmp[MAX_PEERS];
        uint32_t posEp[MAX_PEERS];
        uint8_t n;
        portENTER_CRITICAL(&inboxMux);
        n = sPeerCount;
        memcpy(tmp, sPeers, n * sizeof(LoraPeerNode));
        memcpy(posEp, sPeerPosEpoch, sizeof(posEp));
        portEXIT_CRITICAL(&inboxMux);
        p.putUChar("pcnt", n);
        if (n > 0) {
            p.putBytes("peers", tmp, n * sizeof(LoraPeerNode));
            p.putBytes("ppos", posEp, sizeof(posEp));
        }
        sPeersDirty = false;
    }
    p.end();
    sLastNvsFlushMs = millis();
}

static void configureRadioParams() {
    if (sPreset == MESH_PRESET_LONG_FAST) {
        // Meshtastic EU_868 LongFast: 869.525 MHz, BW 250 kHz, SF11, CR 4/5, SyncWord 0x2B, Preamble 16
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(11);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(20);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else if (sPreset == MESH_PRESET_MED_FAST) {
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(9);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(20);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else if (sPreset == MESH_PRESET_SHORT_FAST) {
        radio.setFrequency(869.525f);
        radio.setBandwidth(250.0f);
        radio.setSpreadingFactor(7);
        radio.setCodingRate(5);
        radio.setSyncWord(0x2B);
        radio.setOutputPower(20);
        radio.setPreambleLength(16);
        radio.setCRC(true);
    } else {
        // Direct ASCII mode: 868.0 MHz, BW 125 kHz, SF9, CR 4/7, SyncWord 0x12, Preamble 8
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
    memset(sPeerPosEpoch, 0, sizeof(sPeerPosEpoch));
    memset(sPeerPosMs, 0, sizeof(sPeerPosMs));

    {
        Preferences p;
        if (p.begin("kronos_lora", false)) {
            p.getBytes("inbox", inbox, sizeof(inbox));
            inboxHead = p.getUChar("head", 0) % INBOX_LEN;
            inboxCount = min<uint8_t>(p.getUChar("count", 0), INBOX_LEN);
            recvCount = p.getUInt("rx", 0);
            sentCount = p.getUInt("tx", 0);
            sPreset = (MeshPreset)(p.getUChar("preset", 0) % 4);

            uint8_t savedPeers = min<uint8_t>(p.getUChar("pcnt", 0), MAX_PEERS);
            if (savedPeers > 0) {
                LoraPeerNode tmp[MAX_PEERS];
                memset(tmp, 0, sizeof(tmp));
                size_t got = p.getBytes("peers", tmp, savedPeers * sizeof(LoraPeerNode));
                savedPeers = (uint8_t)(got / sizeof(LoraPeerNode));
                uint32_t posEp[MAX_PEERS] = {0};
                p.getBytes("ppos", posEp, sizeof(posEp));
                sPeerCount = 0;
                for (uint8_t i = 0; i < savedPeers; i++) {
                    if (tmp[i].nodeId == 0) continue;
                    sPeerPosEpoch[sPeerCount] = posEp[i];
                    sPeers[sPeerCount++] = tmp[i];
                }
            }
            p.end();
        }
    }
    txQueue = xQueueCreate(8, sizeof(TxReq));

    if (!(instance.getDeviceProbe() & HW_RADIO_ONLINE)) {
        Serial.println("[LORA] SX1262 not detected");
        return false;
    }
    // RF switch lives on the XL9555 I2C expander: route to the internal antenna once.
    if (PowerManager::lockI2C(100)) {
        instance.setRFSwitch(false);
        PowerManager::unlockI2C();
    }
    configureRadioParams();
    radio.setPacketReceivedAction(onDio1);
    ready = true;
    if (SettingsStorage::get().loraEnabled) {
        radio.startReceive();
        sSleeping = false;
        Serial.printf("[LORA] SX1262 listening in %s mode (%u peers, %u msgs)\n",
                      getPresetLabel(), sPeerCount, inboxCount);
    } else {
        radio.sleep();
        sSleeping = true;
        Serial.println("[LORA] SX1262 initialized in sleep mode (disabled by user)");
    }
    return true;
}

static void storeMessage(const char* s, float rssi, float snr) {
    portENTER_CRITICAL(&inboxMux);
    LoraMessage& m = inbox[inboxHead];
    strncpy(m.text, s, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
    m.rssi = rssi; m.snr = snr; m.epoch = (uint32_t)time(NULL);
    inboxHead = (inboxHead + 1) % INBOX_LEN;
    if (inboxCount < INBOX_LEN) inboxCount++;
    portEXIT_CRITICAL(&inboxMux);
    sInboxDirty = true;
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
                hasCoord = !(lat == 0.0 && lon == 0.0);
            }
            uint32_t hash = 0x811C9DC5u;
            for (unsigned int i = 0; i < name.length(); i++) hash = (hash ^ (uint8_t)name[i]) * 0x01000193u;
            updatePeer(hash, name.c_str(), nullptr, hasCoord, lat, lon, rssi, snr, 0, 0, isSos, false);
            if (isNav && hasCoord) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Ziel %s (%.4f, %.4f)", name.c_str(), lat, lon);
                DisplayEngine::showNotification("LoRa Wegpunkt", msg, 5000, ICO_LOCATION_ON);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Protobuf helpers
// ---------------------------------------------------------------------------
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

static size_t pbPutVarint(uint8_t* out, uint64_t v) {
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        out[n++] = v ? (b | 0x80) : b;
    } while (v && n < 10);
    return n;
}

// Skips one field of wire type `wt`; returns false on malformed input.
static bool pbSkip(const uint8_t*& p, const uint8_t* end, uint32_t wt) {
    if (wt == 0) { pbVarint(p, end); return true; }
    if (wt == 2) { uint64_t l = pbVarint(p, end); if (l > (uint64_t)(end - p)) return false; p += l; return true; }
    if (wt == 5) { if (p + 4 > end) return false; p += 4; return true; }
    if (wt == 1) { if (p + 8 > end) return false; p += 8; return true; }
    return false;
}

// meshtastic.Data: portnum=1, payload=2, want_response=3, dest=4, source=5,
// request_id=6, reply_id=7, emoji=8
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
            if (l > (uint64_t)(end - p)) return false;
            outDataPtr = p;
            outDataLen = (size_t)l;
            p += l;
        } else if (!pbSkip(p, end, wt)) {
            return false;
        }
    }
    return (outPortnum > 0 && outPortnum <= 512 && outDataPtr != nullptr);
}

static void aesCtr(const uint8_t* key, const uint8_t* nonce16, const uint8_t* in, uint8_t* out, size_t len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    uint8_t nc[16];
    memcpy(nc, nonce16, 16);
    mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nc, stream_block, in, out);
    mbedtls_aes_free(&aes);
}

// Decode Meshtastic OTA frame (16-byte header + AES-128 CTR encrypted Data protobuf)
static bool tryDecodeMeshtastic(const uint8_t* pkt, size_t len, float rssi, float snr) {
    if (len < 18 || len > 255) return false;
    uint32_t destNode = rd_u32le(pkt + 0);
    uint32_t fromNode = rd_u32le(pkt + 4);
    uint32_t packetId = rd_u32le(pkt + 8);
    uint8_t flags     = pkt[12];
    uint8_t chanHash  = pkt[13];
    (void)destNode;
    (void)flags;   // hop_limit / hop_start / want_ack / via_mqtt: not needed for display

    if (fromNode == 0 || fromNode == 0xFFFFFFFFUL || fromNode == ourNodeId()) return false;

    uint8_t nonce[16] = {0};
    memcpy(nonce + 0, &packetId, 4);
    memcpy(nonce + 8, &fromNode, 4);

    uint8_t plain[240];
    size_t payLen = len - 16;
    uint32_t portnum = 0;
    const uint8_t* dataPtr = nullptr;
    size_t dataLen = 0;
    bool decrypted = false;

    // The channel hash tells us which default-key index could match
    // (hash = xor(name) ^ xor(key), xor(default key k) = 0x03 ^ k): a single
    // AES attempt instead of brute-forcing all simple keys.
    int keyIdx = (int)(uint8_t)(chanHash ^ presetNameXor() ^ 0x03);
    if (keyIdx >= 1 && keyIdx <= 10) {
        uint8_t key[16];
        memcpy(key, MESH_DEFAULT_PSK, 16);
        key[15] = (uint8_t)keyIdx;
        aesCtr(key, nonce, pkt + 16, plain, payLen);
        if (tryParseMeshtasticDataProtobuf(plain, payLen, portnum, dataPtr, dataLen)) decrypted = true;
    }

    // Also test unencrypted payload
    if (!decrypted && tryParseMeshtasticDataProtobuf(pkt + 16, payLen, portnum, dataPtr, dataLen)) {
        decrypted = true;
    }

    if (!decrypted) {
        // Meshtastic mode and a structurally valid header: register the node id
        // and channel hash even if the channel uses a private key.
        if (sPreset != MESH_PRESET_KRONOS_DIR) {
            updatePeer(fromNode, nullptr, nullptr, false, 0, 0, rssi, snr, chanHash, 0, false, true);
            char msg[72];
            snprintf(msg, sizeof(msg), "Knoten !%08lx (CH 0x%02X, verschlüsselt)",
                     (unsigned long)fromNode, (unsigned)chanHash);
            storeMessage(msg, rssi, snr);
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
        storeMessage(txt, rssi, snr);
        DisplayEngine::showNotification("Meshtastic", txt, 5000, ICO_CHAT);
        return true;
    } else if (portnum == 3) { // POSITION_APP: latitude_i=1, longitude_i=2 (sfixed32), altitude=3, time=4
        const uint8_t *pp = dataPtr, *pend = dataPtr + dataLen;
        int32_t latI = 0, lonI = 0;
        while (pp < pend) {
            uint64_t tag = pbVarint(pp, pend);
            uint32_t field = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
            if (field == 1 && wt == 5 && pp + 4 <= pend) { latI = rd_s32le(pp); pp += 4; }
            else if (field == 2 && wt == 5 && pp + 4 <= pend) { lonI = rd_s32le(pp); pp += 4; }
            else if (!pbSkip(pp, pend, wt)) break;
        }
        bool hasPos = (latI != 0 && lonI != 0);
        double lat = hasPos ? (latI * 1e-7) : 0.0;
        double lon = hasPos ? (lonI * 1e-7) : 0.0;
        updatePeer(fromNode, nullptr, nullptr, hasPos, lat, lon, rssi, snr, chanHash, 0, false, true);
        if (hasPos) {
            char msg[72];
            snprintf(msg, sizeof(msg), "POS !%04x: %.4f, %.4f", (unsigned)(fromNode & 0xFFFF), lat, lon);
            storeMessage(msg, rssi, snr);
        }
        return true;
    } else if (portnum == 4 || portnum == 73) {
        // NODEINFO_APP (User): id=1, long_name=2, short_name=3, macaddr=4, hw_model=5, is_licensed=6
        // MAP_REPORT_APP (MapReport): long_name=1, short_name=2, role=3, hw_model=4,
        //   firmware_version=5, region=6, modem_preset=7, has_default_channel=8,
        //   latitude_i=9, longitude_i=10, altitude=11, position_precision=12, num_online_local_nodes=13
        const uint8_t *up = dataPtr, *uend = dataPtr + dataLen;
        char shortName[12] = {0};
        char longName[24] = {0};
        int32_t latI = 0, lonI = 0;
        uint32_t fLong = (portnum == 4) ? 2 : 1;
        uint32_t fShort = (portnum == 4) ? 3 : 2;
        while (up < uend) {
            uint64_t tag = pbVarint(up, uend);
            uint32_t field = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
            if (field == fLong && wt == 2) {
                uint64_t l = pbVarint(up, uend);
                if (l > (uint64_t)(uend - up)) break;
                size_t cl = min<size_t>((size_t)l, sizeof(longName) - 1);
                memcpy(longName, up, cl); longName[cl] = 0;
                up += l;
            } else if (field == fShort && wt == 2) {
                uint64_t l = pbVarint(up, uend);
                if (l > (uint64_t)(uend - up)) break;
                size_t cl = min<size_t>((size_t)l, sizeof(shortName) - 1);
                memcpy(shortName, up, cl); shortName[cl] = 0;
                up += l;
            } else if (portnum == 73 && field == 9 && wt == 5 && up + 4 <= uend) {
                latI = rd_s32le(up); up += 4;
            } else if (portnum == 73 && field == 10 && wt == 5 && up + 4 <= uend) {
                lonI = rd_s32le(up); up += 4;
            } else if (!pbSkip(up, uend, wt)) {
                break;
            }
        }
        const char* bestName = longName[0] ? longName : (shortName[0] ? shortName : nullptr);
        bool hasPos = (latI != 0 && lonI != 0);
        updatePeer(fromNode, bestName, shortName, hasPos, latI * 1e-7, lonI * 1e-7,
                   rssi, snr, chanHash, 0, false, true);
        if (bestName) {
            char msg[72];
            snprintf(msg, sizeof(msg), "Knoten: %s (%s)", bestName, shortName[0] ? shortName : "MESH");
            storeMessage(msg, rssi, snr);
            DisplayEngine::showNotification("Meshtastic", msg, 4000, ICO_PODCASTS);
        }
        return true;
    } else {
        // Any other Meshtastic portnum (e.g. 67=TELEMETRY_APP, 5=ROUTING_APP, 70=TRACEROUTE, 71=NEIGHBORINFO)
        updatePeer(fromNode, nullptr, nullptr, false, 0, 0, rssi, snr, chanHash, 0, false, true);
        char msg[72];
        const char* pName = (portnum == 67) ? "TELEMETRIE" :
                            (portnum == 71) ? "NEIGHBORINFO" :
                            (portnum == 70) ? "TRACEROUTE" :
                            (portnum == 5)  ? "ROUTING" : "PAKET";
        snprintf(msg, sizeof(msg), "MESH %s von !%08lx", pName, (unsigned long)fromNode);
        storeMessage(msg, rssi, snr);
        return true;
    }
}

// ---------------------------------------------------------------------------
//  Frame building (no radio access)
// ---------------------------------------------------------------------------
// Builds an encrypted Meshtastic frame into out; returns total length or 0.
static size_t buildMeshtasticFrame(uint32_t destNode, uint32_t portnum,
                                   const uint8_t* appData, size_t appLen, bool wantResponse,
                                   uint8_t* out, size_t outCap) {
    if (appLen > 200) return 0;
    uint8_t plain[220];
    size_t idx = 0;
    // field 1 (portnum), varint
    plain[idx++] = (1 << 3) | 0;
    idx += pbPutVarint(plain + idx, portnum);
    // field 2 (payload), bytes
    plain[idx++] = (2 << 3) | 2;
    idx += pbPutVarint(plain + idx, appLen);
    memcpy(plain + idx, appData, appLen);
    idx += appLen;
    // field 3 (want_response), bool varint
    if (wantResponse) {
        plain[idx++] = (3 << 3) | 0;
        plain[idx++] = 1;
    }
    if (16 + idx > outCap) return 0;

    uint32_t from = ourNodeId();
    uint32_t pktId = (uint32_t)esp_random();
    memcpy(out + 0, &destNode, 4);
    memcpy(out + 4, &from, 4);
    memcpy(out + 8, &pktId, 4);
    // flags: hop_limit bits 0..2, want_ack bit 3, via_mqtt bit 4, hop_start bits 5..7
    out[12] = (uint8_t)((3 << 5) | 3);
    out[13] = presetChannelHash();
    out[14] = 0;
    out[15] = 0;

    uint8_t nonce[16] = {0};
    memcpy(nonce + 0, &pktId, 4);
    memcpy(nonce + 8, &from, 4);
    aesCtr(MESH_DEFAULT_PSK, nonce, plain, out + 16, idx);
    return 16 + idx;
}

static size_t buildTxFrame(const TxReq& req, uint8_t* out, size_t outCap, char* logText, size_t logLen) {
    bool isMesh = (sPreset != MESH_PRESET_KRONOS_DIR);
    if (isMesh && req.kind == TX_MESH_NODEINFO) {
        // meshtastic.User: id=1, long_name=2, short_name=3, hw_model=5
        char idStr[16];
        snprintf(idStr, sizeof(idStr), "!%08lx", (unsigned long)ourNodeId());
        const char* longNm = "WatchOS Ultra";
        const char* shortNm = "WOSU";
        uint8_t userPb[64];
        size_t u = 0;
        userPb[u++] = (1 << 3) | 2; userPb[u++] = (uint8_t)strlen(idStr);
        memcpy(userPb + u, idStr, strlen(idStr)); u += strlen(idStr);
        userPb[u++] = (2 << 3) | 2; userPb[u++] = (uint8_t)strlen(longNm);
        memcpy(userPb + u, longNm, strlen(longNm)); u += strlen(longNm);
        userPb[u++] = (3 << 3) | 2; userPb[u++] = (uint8_t)strlen(shortNm);
        memcpy(userPb + u, shortNm, strlen(shortNm)); u += strlen(shortNm);
        // hw_model: the T-Watch Ultra has no confirmed HardwareModel value in
        // this firmware's protobuf snapshot -> UNSET (0) rather than a wrong model.
        userPb[u++] = (5 << 3) | 0; userPb[u++] = 0;
        snprintf(logText, logLen, "NODEINFO -> !%08lx", (unsigned long)req.targetNode);
        return buildMeshtasticFrame(req.targetNode, 4, userPb, u, true, out, outCap);
    }
    if (isMesh && req.kind == TX_MESH_POS) {
        // meshtastic.Position: latitude_i=1, longitude_i=2 (sfixed32), time=4 (fixed32)
        uint8_t posPb[24];
        size_t n = 0;
        int32_t latI = (int32_t)(req.lat * 1e7);
        int32_t lonI = (int32_t)(req.lon * 1e7);
        posPb[n++] = (1 << 3) | 5; memcpy(posPb + n, &latI, 4); n += 4;
        posPb[n++] = (2 << 3) | 5; memcpy(posPb + n, &lonI, 4); n += 4;
        if (RtcManager::isTimeValid()) {
            uint32_t t = (uint32_t)time(NULL);
            posPb[n++] = (4 << 3) | 5; memcpy(posPb + n, &t, 4); n += 4;
        }
        snprintf(logText, logLen, "POSITION");
        return buildMeshtasticFrame(0xFFFFFFFFUL, 3, posPb, n, false, out, outCap);
    }
    if (isMesh && strncmp(req.text, "NAV|", 4) != 0 && strncmp(req.text, "SOS|", 4) != 0 &&
        strncmp(req.text, "BCN|", 4) != 0) {
        snprintf(logText, logLen, "TEXT");
        return buildMeshtasticFrame(0xFFFFFFFFUL, 1, (const uint8_t*)req.text, strlen(req.text), false, out, outCap);
    }
    size_t len = strlen(req.text);
    if (len == 0 || len > outCap) return 0;
    memcpy(out, req.text, len);
    snprintf(logText, logLen, "'%.30s'", req.text);
    return len;
}

// ---------------------------------------------------------------------------
//  Airtime / duty cycle
// ---------------------------------------------------------------------------
static uint32_t airtimeUsedMs(unsigned long now) {
    // Entries 0..sAirCount-1 are exactly the valid ring slots (ring fills from 0).
    uint32_t sum = 0;
    for (uint8_t i = 0; i < sAirCount; i++) {
        if (now - sAir[i].ms < DUTY_WINDOW_MS) sum += sAir[i].airMs;
    }
    return sum;
}

static void recordAirtime(unsigned long now, uint32_t airMs) {
    const uint8_t cap = (uint8_t)(sizeof(sAir) / sizeof(sAir[0]));
    sAir[sAirHead] = {now, airMs};
    sAirHead = (sAirHead + 1) % cap;
    if (sAirCount < cap) sAirCount++;
}

static uint32_t dutyBudgetMs() {
    float f = currentFreqMhz();
    // EU 863-870: sub-band g1 869.4-869.65 MHz allows 10 %, everything else 1 % here.
    return (f >= 869.4f && f <= 869.65f) ? (DUTY_WINDOW_MS / 10) : (DUTY_WINDOW_MS / 100);
}

enum TxBegin : uint8_t { TXB_STARTED, TXB_DEFERRED, TXB_FAILED };

static TxBegin beginTransmit(const uint8_t* data, size_t len, unsigned long now) {
    uint32_t airMs = (uint32_t)(radio.getTimeOnAir(len) / 1000UL);
    if (airMs == 0) airMs = 1;
    if (airtimeUsedMs(now) + airMs > dutyBudgetMs()) {
        if (now - sLastDutyLogMs > 10000) {
            sLastDutyLogMs = now;
            Serial.printf("[LORA] Duty cycle budget exhausted (%lu/%lu ms per h), TX queued\n",
                          (unsigned long)airtimeUsedMs(now), (unsigned long)dutyBudgetMs());
        }
        return TXB_DEFERRED;
    }
    radio.standby();
    int st = radio.startTransmit(data, len);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] startTransmit failed (%d)\n", st);
        rxFlag = false;
        radio.startReceive();
        return TXB_FAILED;
    }
    sTxState = TXS_WAIT;
    sTxStartMs = now;
    sTxAirtimeMs = airMs;
    return TXB_STARTED;
}

static void serviceTx(unsigned long now) {
    if (sTxState == TXS_WAIT) {
        uint32_t irq = radio.getIrqFlags();
        bool done = (irq & RADIOLIB_SX126X_IRQ_TX_DONE) != 0;
        bool timeout = (now - sTxStartMs) > TX_TIMEOUT_MS;
        if (!done && !timeout) return;
        radio.finishTransmit();
        rxFlag = false;
        if (done) {
            sentCount++;
            sLastTxMs = now ? now : 1;
            recordAirtime(now, sTxAirtimeMs);
            sInboxDirty = true;   // tx counter
            Serial.printf("[LORA] TX %s (%u bytes, %lu ms airtime)\n", sTxLogText, (unsigned)sTxLen, (unsigned long)sTxAirtimeMs);
        } else {
            Serial.println("[LORA] TX timeout, radio reset to RX");
            radio.standby();
        }
        sTxState = TXS_IDLE;
        radio.startReceive();
        nav::markDirty(nav::PG_LORA);
        return;
    }

    // IDLE: pick the deferred request first, then the queue
    TxReq req;
    bool have = false;
    if (sTxHaveDeferred) {
        req = sTxDeferred;
        have = true;
    } else if (xQueueReceive(txQueue, &req, 0) == pdTRUE) {
        have = true;
    }
    if (!have) return;

    size_t len = buildTxFrame(req, sTxBuf, sizeof(sTxBuf), sTxLogText, sizeof(sTxLogText));
    if (len == 0) {
        sTxHaveDeferred = false;
        return;
    }
    TxBegin r = beginTransmit(sTxBuf, len, now);
    if (r == TXB_DEFERRED) {
        sTxDeferred = req;
        sTxHaveDeferred = true;
        return;
    }
    sTxHaveDeferred = false;
    if (r == TXB_STARTED) sTxLen = len;
}

// ---------------------------------------------------------------------------
//  BLE threat radar (async NimBLE scan)
// ---------------------------------------------------------------------------
class ThreatScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev) return;
        std::string name = dev->getName();
        std::string addr = dev->getAddress().toString();
        std::string mfg  = dev->getManufacturerData();

        bool isTracker = false;
        const char* tag = "BLE GERÄT";
        if (mfg.size() >= 3) {
            uint16_t cid = (uint8_t)mfg[0] | ((uint16_t)(uint8_t)mfg[1] << 8);
            if (cid == 0x004C && (uint8_t)mfg[2] == 0x12) { tag = "FINDMY"; isTracker = true; }   // Apple offline finding
        }
        if (!isTracker) {
            if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFEED)))      { tag = "TILE";     isTracker = true; }
            else if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFD5A))) { tag = "SMARTTAG"; isTracker = true; }
            else if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFE33))) { tag = "CHIPOLO";  isTracker = true; }
        }

        portENTER_CRITICAL(&sThreatMux);
        int slot = -1;
        for (int i = 0; i < sThreatStageCount; i++) {
            if (strncmp(sThreatStage[i].mac, addr.c_str(), sizeof(sThreatStage[i].mac) - 1) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            if (sThreatStageCount < MAX_THREATS) {
                slot = sThreatStageCount++;
            } else if (isTracker) {
                for (int i = 0; i < MAX_THREATS; i++) if (!sThreatStage[i].isTracker) { slot = i; break; }
            }
        }
        if (slot >= 0) {
            BleThreatInfo& ti = sThreatStage[slot];
            const char* shown = name.empty() ? addr.c_str() : name.c_str();
            strncpy(ti.name, shown, sizeof(ti.name) - 1);  ti.name[sizeof(ti.name) - 1] = 0;
            strncpy(ti.mac, addr.c_str(), sizeof(ti.mac) - 1); ti.mac[sizeof(ti.mac) - 1] = 0;
            strncpy(ti.typeTag, tag, sizeof(ti.typeTag) - 1); ti.typeTag[sizeof(ti.typeTag) - 1] = 0;
            ti.rssi = (int8_t)constrain(dev->getRSSI(), -110, -10);
            ti.isTracker = isTracker;
            ti.lastSeenMs = millis();
        }
        portEXIT_CRITICAL(&sThreatMux);
    }
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        (void)results; (void)reason;
        sThreatScanDone = true;
    }
};
static ThreatScanCallbacks sThreatScanCb;

static void startThreatScan() {
    if (!BleCompanion::isEnabled() || !NimBLEDevice::isInitialized()) {
        Serial.println("[LORA] BLE radar: Bluetooth is off");
        sThreatScanRunning = false;
        nav::markDirty(nav::PG_LORA);
        return;
    }
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan || pScan->isScanning()) return;
    portENTER_CRITICAL(&sThreatMux);
    sThreatStageCount = 0;
    portEXIT_CRITICAL(&sThreatMux);
    sThreatScanDone = false;
    pScan->setScanCallbacks(&sThreatScanCb, false);
    pScan->setActiveScan(true);
    pScan->setInterval(45);
    pScan->setWindow(30);
    pScan->setMaxResults(0);        // we keep our own table; do not store results
    if (pScan->start(1500, false, true)) {
        sThreatScanRunning = true;
        sThreatScanStartMs = millis();
        nav::markDirty(nav::PG_LORA);
    } else {
        Serial.println("[LORA] BLE radar: scan start failed");
    }
}

static void serviceThreatScan(unsigned long now) {
    if (sThreatScanReq && !sThreatScanRunning) {
        sThreatScanReq = false;
        startThreatScan();
        return;
    }
    if (!sThreatScanRunning) return;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    bool finished = sThreatScanDone || !pScan || !pScan->isScanning() || (now - sThreatScanStartMs > 4000);
    if (!finished) return;
    if (pScan && pScan->isScanning()) pScan->stop();
    portENTER_CRITICAL(&sThreatMux);
    sThreatCount = sThreatStageCount;
    memcpy(sThreats, sThreatStage, sThreatCount * sizeof(BleThreatInfo));
    portEXIT_CRITICAL(&sThreatMux);
    sThreatScanRunning = false;
    Serial.printf("[LORA] BLE radar: %u devices\n", (unsigned)sThreatCount);
    nav::markDirty(nav::PG_LORA);
}

// ---------------------------------------------------------------------------
//  Spectrum sweep: one bin per loop() pass, never blocks
// ---------------------------------------------------------------------------
static float binFreq(int i) { return 863.0f + i * (7.0f / (SPECTRUM_BINS - 1)); }

// returns true while a sweep step owns the radio
static bool serviceSpectrum(unsigned long now) {
    if (!sSpectrumActive) {
        if (sSpecBin >= 0) {          // deactivated mid-sweep: restore the channel
            sSpecBin = -1;
            radio.standby();
            configureRadioParams();
            rxFlag = false;
            radio.startReceive();
        }
        return false;
    }
    if (sSpecBin < 0) {
        if (now - sLastSpectrumSweepMs < 650) return false;
        radio.standby();
        radio.setFrequency(binFreq(0), true);
        radio.startReceive();
        sSpecBin = 0;
        sSpecBinMs = now;
        return true;
    }
    if (now - sSpecBinMs < 2) return true;   // let the receiver settle (loop runs every ~10 ms)
    float rssi = radio.getRSSI(false);
    sSpectrumDbm[sSpecBin] = (int8_t)constrain((int)rssi, -120, -25);
    sSpecBin++;
    if (sSpecBin >= SPECTRUM_BINS) {
        int8_t bestDbm = -125;
        float bestFreq = 868.0f;
        for (int i = 0; i < SPECTRUM_BINS; i++) {
            if (sSpectrumDbm[i] > bestDbm) { bestDbm = sSpectrumDbm[i]; bestFreq = binFreq(i); }
        }
        sPeakDbm = bestDbm;
        sPeakFreqMhz = bestFreq;
        sSpecBin = -1;
        sLastSpectrumSweepMs = now;
        radio.standby();
        configureRadioParams();
        rxFlag = false;
        radio.startReceive();
        nav::markDirty(nav::PG_LORA);
        return false;
    }
    radio.standby();
    radio.setFrequency(binFreq(sSpecBin), true);
    radio.startReceive();
    sSpecBinMs = now;
    return true;
}

// ---------------------------------------------------------------------------
void RadioLoRa::loop(unsigned long now) {
    if (!ready) return;

    serviceThreatScan(now);

    // NVS housekeeping (dirty flags, at most once per minute, or on request)
    if (sFlushRequest) {
        sFlushRequest = false;
        flushNvs(true);
    } else if ((sPeersDirty || sInboxDirty) && now - sLastNvsFlushMs >= NVS_FLUSH_MS) {
        flushNvs(false);
    }

    if (sReconfigRadio) {
        sReconfigRadio = false;
        sSpecBin = -1;
        sTxState = TXS_IDLE;
        radio.standby();
        configureRadioParams();
        if (SettingsStorage::get().loraEnabled) {
            radio.startReceive();
            sSleeping = false;
        }
        sInboxDirty = true;   // preset is stored with the inbox blob
    }

    bool wantEn = SettingsStorage::get().loraEnabled;
    if (!wantEn) {
        if (!sSleeping) {
            rxFlag = false;
            sSpecBin = -1;
            sTxState = TXS_IDLE;
            radio.standby();
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

    if (sTxState == TXS_WAIT) {
        serviceTx(now);
        return;
    }

    if (serviceSpectrum(now)) return;   // radio is parked on a sweep frequency

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
                sInboxDirty = true;
                if (!tryDecodeMeshtastic(rawBuf, pktLen, lastRssi, lastSnr)) {
                    rawBuf[pktLen] = 0;
                    String s((const char*)rawBuf);
                    storeMessage(s.c_str(), lastRssi, lastSnr);
                    parseAsciiPacket(s, lastRssi, lastSnr);
                    Serial.printf("[LORA] RX '%s' RSSI %.0f SNR %.1f\n", s.c_str(), lastRssi, lastSnr);
                    char t[40];
                    snprintf(t, sizeof(t), "LoRa  %.0f dBm", lastRssi);
                    DisplayEngine::showNotification(t, s, 6000, ICO_CELL_TOWER);
                    AudioEngine::playSoundFx(s.startsWith("SOS") ? SFX_SOS : SFX_NOTIFY);
                }
            }
        }
        radio.startReceive();
    }

    serviceTx(now);

    // Periodic beacon (user setting): position only with a real, recent fix
    if (SettingsStorage::get().loraBeaconEnabled && now - lastBeacon > BEACON_MS) {
        lastBeacon = now;
        if (sPreset != MESH_PRESET_KRONOS_DIR) {
            if (ownPositionAllowed()) sendPositionBeacon();
        } else {
            char b[96];
            if (ownPositionAllowed())
                snprintf(b, sizeof(b), "BCN|%s|%d%%|%.5f,%.5f", CompanionServer::getApName(),
                         PowerManager::getBatteryPercent(), GpsManager::latitude(), GpsManager::longitude());
            else
                snprintf(b, sizeof(b), "BCN|%s|%d%%", CompanionServer::getApName(), PowerManager::getBatteryPercent());
            sendPacket(b);
        }
    }
}

// ---------------------------------------------------------------------------
//  Public TX API (enqueue only; never enables the radio)
// ---------------------------------------------------------------------------
bool RadioLoRa::sendPacket(const String& msg) {
    if (!ready || !txQueue || !isEnabled()) return false;
    if (msg.length() == 0) return false;
    TxReq r{};
    r.kind = TX_ASCII_OR_TEXT;
    r.targetNode = 0xFFFFFFFFUL;
    strncpy(r.text, msg.c_str(), sizeof(r.text) - 1);
    r.text[sizeof(r.text) - 1] = 0;
    return xQueueSend(txQueue, &r, 0) == pdTRUE;
}

bool RadioLoRa::sendNodeInfoDiscovery(uint32_t targetNode) {
    if (!ready || !txQueue || !isEnabled()) return false;
    if (sPreset == MESH_PRESET_KRONOS_DIR) return false;
    unsigned long now = millis();
    if (sLastNodeInfoMs != 0 && now - sLastNodeInfoMs < NODEINFO_MIN_MS) return false;
    TxReq r{};
    r.kind = TX_MESH_NODEINFO;
    r.targetNode = targetNode;
    if (xQueueSend(txQueue, &r, 0) != pdTRUE) return false;
    sLastNodeInfoMs = now ? now : 1;
    return true;
}

bool RadioLoRa::sendPositionBeacon() {
    if (!ready || !txQueue || !isEnabled()) return false;
    if (!ownPositionAllowed()) return false;
    double lat = GpsManager::latitude();
    double lon = GpsManager::longitude();
    if (lat == 0.0 && lon == 0.0) return false;
    if (sPreset != MESH_PRESET_KRONOS_DIR) {
        TxReq r{};
        r.kind = TX_MESH_POS;
        r.lat = lat;
        r.lon = lon;
        return xQueueSend(txQueue, &r, 0) == pdTRUE;
    }
    char b[96];
    snprintf(b, sizeof(b), "BCN|%s|%d%%|%.5f,%.5f", CompanionServer::getApName(),
             PowerManager::getBatteryPercent(), lat, lon);
    return sendPacket(b);
}

bool RadioLoRa::sendWaypoint(const char* name, double lat, double lon) {
    if (!isEnabled()) return false;
    char b[96];
    snprintf(b, sizeof(b), "NAV|%s|%.5f,%.5f", name ? name : "WP", lat, lon);
    return sendPacket(b);
}

bool RadioLoRa::sendSosBeacon() {
    if (!isEnabled()) return false;
    char b[96];
    if (ownPositionAllowed())
        snprintf(b, sizeof(b), "SOS|%s|%.5f,%.5f|%d%%", CompanionServer::getApName(),
                 GpsManager::latitude(), GpsManager::longitude(), PowerManager::getBatteryPercent());
    else
        snprintf(b, sizeof(b), "SOS|%s|NOFIX|%d%%", CompanionServer::getApName(), PowerManager::getBatteryPercent());
    return sendPacket(b);
}

uint32_t RadioLoRa::getLastTxMs() { return sLastTxMs; }

bool RadioLoRa::pollPacket(String&, float&, float&) { return false; }
bool RadioLoRa::isReady() { return ready && SettingsStorage::get().loraEnabled; }
void RadioLoRa::setEnabled(bool en) {
    SettingsStorage::get().loraEnabled = en;
    SettingsStorage::save();
    if (!en) sFlushRequest = true;   // persist peers/inbox on the bg task right away
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
        default:                     return "Direkt (868.0 SF9)";
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

bool RadioLoRa::getMeshFallbackPosition(double& outLat, double& outLon, char* outSourceName,
                                        size_t nameLen, uint32_t* ageSec) {
    unsigned long nowMs = millis();
    uint32_t nowEpoch = (uint32_t)time(NULL);
    portENTER_CRITICAL(&inboxMux);
    int best = -1;
    uint32_t bestAge = 0;
    for (int i = 0; i < sPeerCount; i++) {
        if (!sPeers[i].hasPos) continue;
        uint32_t age;
        if (sPeerPosMs[i] != 0) age = (uint32_t)((nowMs - sPeerPosMs[i]) / 1000UL);
        else if (sPeerPosEpoch[i] != 0 && nowEpoch >= sPeerPosEpoch[i]) age = nowEpoch - sPeerPosEpoch[i];
        else continue;   // unknown age -> never trust
        if (age >= PEER_POS_MAX_AGE_S) continue;
        if (best < 0 || sPeers[i].rssi > sPeers[best].rssi) { best = i; bestAge = age; }
    }
    if (best >= 0) {
        outLat = sPeers[best].lat;
        outLon = sPeers[best].lon;
        if (outSourceName && nameLen > 0) {
            strncpy(outSourceName, sPeers[best].name, nameLen - 1);
            outSourceName[nameLen - 1] = 0;
        }
        if (ageSec) *ageSec = bestAge;
        portEXIT_CRITICAL(&inboxMux);
        return true;
    }
    portEXIT_CRITICAL(&inboxMux);
    return false;
}

void RadioLoRa::setSpectrumScanActive(bool active) {
    sSpectrumActive = active;
}
bool RadioLoRa::isSpectrumScanActive() { return sSpectrumActive; }

void RadioLoRa::getSpectrumBins(int8_t* outDbm, int numBins, float& outPeakFreqMhz, int8_t& outPeakDbm) {
    if (!outDbm || numBins <= 0) return;
    int n = min(numBins, (int)SPECTRUM_BINS);
    for (int i = 0; i < n; i++) outDbm[i] = sSpectrumDbm[i];
    for (int i = n; i < numBins; i++) outDbm[i] = -120;   // callers asking for more bins get the floor
    outPeakFreqMhz = sPeakFreqMhz;
    outPeakDbm = sPeakDbm;
}

void RadioLoRa::triggerThreatScan() {
    if (!sThreatScanRunning) sThreatScanReq = true;
}
bool RadioLoRa::isThreatScanRunning() { return sThreatScanRunning; }

uint8_t RadioLoRa::getThreats(BleThreatInfo* out, uint8_t maxOut) {
    if (!out || maxOut == 0) return 0;
    portENTER_CRITICAL(&sThreatMux);
    uint8_t n = min(maxOut, sThreatCount);
    for (uint8_t i = 0; i < n; i++) out[i] = sThreats[i];
    portEXIT_CRITICAL(&sThreatMux);
    return n;
}
