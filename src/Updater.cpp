#include "Updater.h"
#include "Config.h"
#include "SemVer.h"
#include "CompanionServer.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "DisplayEngine.h"
#include "ui/Icons.h"
#include "ui/Navigator.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

// root CA bundle that ESP-IDF links into every build (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
extern const uint8_t ca_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t ca_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace Updater {

static const char* BASE_URL = "https://github.com/diggaknoxville/WatchOS-Ultra/releases/latest/download/";
static const uint32_t CHECK_EVERY_S = 86400;            // automatic check: once a day
static const uint32_t RETRY_MS = 30UL * 60UL * 1000UL;  // after a failed automatic check
static const uint32_t TLS_MIN_HEAP = 48 * 1024;         // mbedTLS needs ~40 KB of internal RAM
static const size_t SIG_MAX = 80;                       // DER ECDSA P-256 signature <= 72 bytes

static char sSsid[33] = "", sPass[65] = "";
static bool sAuto = true;
static uint32_t sLastCheckEpoch = 0;
static volatile State sState = IDLE;
static char sAvail[16] = "";
static char sOffered[16] = "";           // version the user was already asked about
static volatile bool sOffer = false;
static volatile int sProgress = 0;
static char sErr[40] = "";
static unsigned long sLastTryMs = 0;
static volatile uint8_t sJob = 0;        // 1 = check, 2 = install

static void fail(const char* why) {
    strncpy(sErr, why, sizeof(sErr) - 1);
    sErr[sizeof(sErr) - 1] = 0;
    sState = FAILED;
    Serial.printf("[UPD] failed: %s\n", why);
}

static bool wifiUp() {
    if (!sSsid[0]) { fail("Kein WLAN hinterlegt"); return false; }
    if (CompanionServer::isApActive()) { fail("Hotspot ist aktiv"); return false; }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < TLS_MIN_HEAP) { fail("Zu wenig Speicher"); return false; }
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(sSsid, sPass);
    for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() != WL_CONNECTED) { fail("WLAN nicht erreichbar"); return false; }
    return true;
}

static void wifiDown() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// GET a release asset; redirects (github.com -> asset CDN) are followed.
static int get(HTTPClient& http, NetworkClientSecure& tls, const char* asset) {
    String url = String(BASE_URL) + asset;
    tls.setCACertBundle(ca_bundle_start, (size_t)(ca_bundle_end - ca_bundle_start));
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (!http.begin(tls, url)) return -1;
    return http.GET();
}

static void doCheck() {
    if (!wifiUp()) { wifiDown(); return; }
    String body;
    int code;
    {
        NetworkClientSecure tls;
        HTTPClient http;
        code = get(http, tls, "version.json");
        if (code == 200) body = http.getString();
        http.end();
    }
    wifiDown();
    if (code != 200) { fail(code == 404 ? "Kein Release gefunden" : "GitHub nicht erreichbar"); return; }
    int k = body.indexOf("\"version\"");
    int q1 = k < 0 ? -1 : body.indexOf('"', body.indexOf(':', k));
    int q2 = q1 < 0 ? -1 : body.indexOf('"', q1 + 1);
    if (q2 < 0 || q2 - q1 - 1 >= (int)sizeof(sAvail)) { fail("version.json unlesbar"); return; }
    String v = body.substring(q1 + 1, q2);
    if (RtcManager::isTimeValid()) {
        sLastCheckEpoch = (uint32_t)time(nullptr);
        Preferences p;
        if (p.begin("kronos_upd", false)) { p.putUInt("last", sLastCheckEpoch); p.end(); }
    }
    if (semverCompare(v.c_str(), OS_VERSION) > 0) {
        strncpy(sAvail, v.c_str(), sizeof(sAvail) - 1);
        sState = AVAILABLE;
        if (strcmp(sOffered, sAvail) != 0) {           // ask once per version
            strncpy(sOffered, sAvail, sizeof(sOffered) - 1);
            sOffer = true;
        }
        Serial.printf("[UPD] %s available (running %s)\n", sAvail, OS_VERSION);
    } else {
        sState = UP_TO_DATE;
        Serial.printf("[UPD] up to date (%s, latest %s)\n", OS_VERSION, v.c_str());
    }
}

static void doInstall() {
    sProgress = 0;
    if (!wifiUp()) { wifiDown(); return; }
    const bool needSig = CompanionServer::signingKeyPresent();
    uint8_t sig[SIG_MAX];
    size_t sigLen = 0;
    {
        NetworkClientSecure tls;
        HTTPClient http;
        if (get(http, tls, "firmware.bin.sig") == 200) {
            int n = http.getSize();
            if (n > 0 && n <= (int)SIG_MAX) sigLen = http.getStream().readBytes(sig, n);
        }
        http.end();
    }
    if (needSig && !sigLen) { wifiDown(); fail("Signatur fehlt"); return; }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    bool ok = buf != nullptr;
    {
        NetworkClientSecure tls;
        HTTPClient http;
        int total = (ok && get(http, tls, "firmware.bin") == 200) ? http.getSize() : 0;
        ok = total > 0 && CompanionServer::otaBegin("GitHub");
        if (ok) {
            NetworkClient* s = http.getStreamPtr();
            int got = 0;
            unsigned long lastData = millis();
            while (got < total && millis() - lastData < 20000) {
                int n = s->available();
                if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                n = s->readBytes(buf, n > 4096 ? 4096 : n);
                if (n <= 0) continue;
                if (!CompanionServer::otaWrite(buf, n)) break;
                got += n;
                lastData = millis();
                sProgress = (int)((int64_t)got * 100 / total);
            }
            ok = got == total;
            if (!ok) CompanionServer::otaAbort("download incomplete");
        }
        http.end();
    }
    if (buf) heap_caps_free(buf);
    wifiDown();
    if (!ok) { fail("Download fehlgeschlagen"); return; }
    // the user accepted this version on the watch already: no second prompt
    if (!CompanionServer::otaFinish(sigLen ? sig : nullptr, sigLen, needSig, true)) {
        fail(needSig ? "Signatur ungültig" : "Image ungültig");
        return;
    }
    DisplayEngine::showToast("Update", "Installiert, Neustart", 3000, ICO_UPDATE);
}

static void worker(void*) {
    if (sJob == 1) doCheck();
    else doInstall();
    if (sState == FAILED) DisplayEngine::showToast("Update", sErr, 3000, ICO_UPDATE);
    nav::markDirty(nav::PG_APP_SETTINGS);
    sJob = 0;
    vTaskDelete(nullptr);
}

static void start(uint8_t job) {
    if (sJob) return;
    sJob = job;
    sState = job == 1 ? CHECKING : DOWNLOADING;
    // the TLS handshake runs on this stack
    if (xTaskCreatePinnedToCore(worker, "Updater", 12 * 1024, nullptr, 1, nullptr, 0) != pdPASS) {
        sJob = 0;
        fail("Zu wenig Speicher");
    }
}

void init() {
    Preferences p;
    if (!p.begin("kronos_upd", true)) return;
    p.getString("ssid", sSsid, sizeof(sSsid));
    p.getString("pass", sPass, sizeof(sPass));
    sAuto = p.getBool("auto", true);
    sLastCheckEpoch = p.getUInt("last", 0);
    p.end();
}

void loop(unsigned long now) {
    if (!sAuto || sJob || !sSsid[0]) return;
    if (!PowerManager::isVbusConnected() || CompanionServer::isApActive()) return;
    if (now < 120000UL) return;                              // let the boot settle
    if (sLastTryMs && now - sLastTryMs < RETRY_MS) return;
    if (RtcManager::isTimeValid() && sLastCheckEpoch &&
        (uint32_t)time(nullptr) - sLastCheckEpoch < CHECK_EVERY_S) return;
    sLastTryMs = now;
    start(1);
}

void setWifi(const char* ssid, const char* pass) {
    strncpy(sSsid, ssid ? ssid : "", sizeof(sSsid) - 1);
    sSsid[sizeof(sSsid) - 1] = 0;
    strncpy(sPass, pass ? pass : "", sizeof(sPass) - 1);
    sPass[sizeof(sPass) - 1] = 0;
    Preferences p;
    if (p.begin("kronos_upd", false)) { p.putString("ssid", sSsid); p.putString("pass", sPass); p.end(); }
    sLastTryMs = 0;
}

const char* ssid() { return sSsid; }
bool autoCheck() { return sAuto; }
void setAutoCheck(bool on) {
    sAuto = on;
    Preferences p;
    if (p.begin("kronos_upd", false)) { p.putBool("auto", on); p.end(); }
}

void checkNow() { start(1); }
void install() { if (sState == AVAILABLE) start(2); }
State state() { return sState; }
bool busy() { return sJob != 0; }
const char* availableVersion() { return sAvail; }
int progressPct() { return sProgress; }

const char* statusText() {
    static char b[40];
    switch (sState) {
        case CHECKING:    return "Suche...";
        case UP_TO_DATE:  return "Aktuell";
        case AVAILABLE:   snprintf(b, sizeof(b), "%s verfügbar", sAvail); return b;
        case DOWNLOADING: snprintf(b, sizeof(b), "Lädt %d %%", (int)sProgress); return b;
        case FAILED:      return sErr;
        default:          return sSsid[0] ? "Tippen zum Suchen" : "Kein WLAN";
    }
}

bool takeOffer() {
    if (!sOffer) return false;
    sOffer = false;
    return true;
}

}  // namespace Updater
