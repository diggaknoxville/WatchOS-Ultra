#pragma once
#include "Config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// Phone companion: WiFi access point + captive portal serving an offline web
// app (no internet on the watch network, so everything is self-contained).
// Runs on the background task; UI-affecting calls use thread-safe entry points.
//
// Security model:
//   * per-device AP password (SHA-256 of the eFuse MAC, stored in NVS)
//   * every /api/* endpoint requires the portal PIN (X-Pin header or ?pin=),
//     the PIN is regenerated at every AP start and shown on the watch only
//   * Origin header (if present) must match the AP address
//   * OTA images are received, hashed and only activated after the user
//     confirms on the watch (otaPending / otaConfirm)
class CompanionServer {
public:
    static bool init();
    static void handleClient();
    static bool isApActive();
    static const char* getApName();
    static const char* getApPassword();   // 10 chars [a-z0-9], per device
    static const char* getPortalPin();    // 6 digits, new at every AP start
    static void requestAp(bool on);       // thread-safe; applied on the bg task

    // OTA confirmation flow (UI polls otaPending, user decides, UI calls otaConfirm)
    static bool otaPending(char* sha256Hex, size_t hexLen, uint32_t* sizeBytes);
    static void otaConfirm(bool accept);  // thread-safe

    // Image sink shared by the portal upload, the GitHub updater and the USB
    // console: one image at a time into the inactive OTA slot, SHA-256 over
    // the stream. otaFinish() checks the ECDSA signature (UpdateKey.h) when one
    // is given or required, then waits for the confirmation on the watch
    // (autoConfirm = the user already accepted this very image).
    static bool otaBegin(const char* source);
    static bool otaWrite(const uint8_t* data, size_t len);
    static bool otaFinish(const uint8_t* sig, size_t sigLen, bool requireSig, bool autoConfirm);
    static void otaAbort(const char* why);
    static bool otaSigned();              // pending image carried a valid signature
    static bool signingKeyPresent();

private:
    static WebServer server;
    static DNSServer dnsServer;
    static bool apRunning;
    static char apName[32];
    static char apPassword[12];
    static char portalPin[8];
    static volatile int8_t apRequest;   // -1 none, 0 off, 1 on
    static void startAp();
    static void stopAp();
    static void loadOrCreateApPassword();
    static void regeneratePin();

    static bool requirePin();           // sends 403 and returns false on mismatch
    static bool checkOrigin();          // sends 403 and returns false on mismatch
    static bool checkJsonBody(size_t maxLen);

    static void handleRoot();
    static void handleApiStatus();
    static void handleApiSettings();
    static void handleApiInbox();
    static void handleApiTimeSync();
    static void handleApiNotification();
    static void handleApiLoraSend();
    static void handleApiFind();
    static void handleApiWifi();
    static void handleApiAiLimits();
    static void handleApiCalendar();
    static void handleApiMedia();
    static void handleApiOtaStatus();
    static void handleApiOtaUploadDone();
    static void handleApiOtaUploadData();
    static void handleApiSdUploadDone();
    static void handleApiSdUploadData();
    static void otaServiceTimeout(unsigned long now);
};
