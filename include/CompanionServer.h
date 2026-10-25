#pragma once
#include "Config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// Phone companion: WiFi access point + captive portal serving an offline web
// app (no internet on the watch network, so everything is self-contained).
// Runs on the background task; UI-affecting calls use thread-safe entry points.
class CompanionServer {
public:
    static bool init();
    static void handleClient();
    static bool isApActive();
    static const char* getApName();
    static void requestAp(bool on);     // thread-safe; applied on the bg task

private:
    static WebServer server;
    static DNSServer dnsServer;
    static bool apRunning;
    static char apName[32];
    static volatile int8_t apRequest;   // -1 none, 0 off, 1 on
    static void startAp();
    static void stopAp();

    static void handleRoot();
    static void handleApiStatus();
    static void handleApiSettings();
    static void handleApiInbox();
    static void handleApiTimeSync();
    static void handleApiNotification();
    static void handleApiLoraSend();
    static void handleApiFind();
    static void handleApiAiLimits();
    static void handleApiCalendar();
    static void handleApiMedia();
    static void handleApiOtaStatus();
};
