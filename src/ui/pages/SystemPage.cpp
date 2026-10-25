// System: power, storage, radios, memory, time sync, firmware.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "CompanionServer.h"
#include "BleCompanion.h"
#include "GpsManager.h"
#include "RadioLoRa.h"
#include "NavEngine.h"
#include "Config.h"
#include <esp_heap_caps.h>

using namespace ui;

namespace pages {

static const int N_ROWS = 8;
static lv_obj_t* sVal[N_ROWS];

static void refresh(bool) {
    char b[56];
    int bat = PowerManager::getBatteryPercent();
    const char* state = PowerManager::isCharging() ? "Lädt" : (PowerManager::isVbusConnected() ? "USB" : "Akku");
    if (PowerManager::isBatteryConnected()) snprintf(b, sizeof(b), "%d %% · %.2f V · %s", bat, PowerManager::getBatteryVoltage(), state);
    else snprintf(b, sizeof(b), "Kein Akku · %s", state);
    setText(sVal[0], b);

    NavStatus ns = NavEngine::status();
    if (ns.mapLoaded) {
        snprintf(b, sizeof(b), "%lu GB · Karte DE (%lu Orte)", (unsigned long)max<uint32_t>(1, ns.sdTotalMB / 1024),
                 (unsigned long)ns.mapPlaces);
    } else if (ns.sdReady) {
        snprintf(b, sizeof(b), "%lu GB (%lu MB belegt) · keine Karte", (unsigned long)max<uint32_t>(1, ns.sdTotalMB / 1024),
                 (unsigned long)ns.sdUsedMB);
    } else {
        snprintf(b, sizeof(b), "Keine SD-Karte");
    }
    setText(sVal[1], b);
    snprintf(b, sizeof(b), "%.1f °C", PowerManager::getTempCelsius());
    setText(sVal[2], b);
    snprintf(b, sizeof(b), "%s · %s · %s · %s",
             CompanionServer::isApActive() ? "WLAN" : "--",
             BleCompanion::isEnabled() ? (BleCompanion::isConnected() ? "BLE+" : "BLE") : "--",
             GpsManager::isEnabled() ? (GpsManager::hasFix() ? "GPS+" : "GPS") : "--",
             RadioLoRa::isEnabled() ? "LoRa" : "--");
    setText(sVal[3], b);
    snprintf(b, sizeof(b), "%u KB intern · %.1f MB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), ESP.getFreePsram() / 1048576.0f);
    setText(sVal[4], b);
    char a[24];
    ago(a, sizeof(a), RtcManager::lastSyncEpoch());
    const char* src = RtcManager::lastSyncSource();
    snprintf(b, sizeof(b), "%s  %s", src && src[0] ? src : "--", a);
    setText(sVal[5], b);
    unsigned long up = millis() / 1000;
    snprintf(b, sizeof(b), "%lud %02lu:%02lu", up / 86400, (up / 3600) % 24, (up / 60) % 60);
    setText(sVal[6], b);
    snprintf(b, sizeof(b), "%s (%s)", OS_VERSION, OS_BUILD);
    setText(sVal[7], b);
}

void buildSystem(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "SYSTEM");
    lv_obj_t* c = kvCard(p, CONTENT_Y, N_ROWS);
    static const char* keys[N_ROWS] = {"AKKU", "SD / KARTE", "PMU-TEMP", "FUNK",
                                       "SPEICHER FREI", "ZEITSYNC", "LAUFZEIT", "FIRMWARE"};
    for (int i = 0; i < N_ROWS; i++) {
        sVal[i] = kvRow(c, keys[i], i == N_ROWS - 1);
        lv_obj_set_style_text_font(sVal[i], F.small, 0);
    }

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_SYSTEM, d);
}

}  // namespace pages
