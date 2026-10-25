// System: power, memory, time synchronisation and firmware.
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

using namespace ui;

namespace pages {

static lv_obj_t* sVal[7];

static void refresh(bool) {
    char b[56];
    int bat = PowerManager::getBatteryPercent();
    bool chg = PowerManager::isCharging();
    snprintf(b, sizeof(b), "%d %% (%.2f V %s)", bat, PowerManager::getBatteryVoltage(),
             chg ? "CHG" : (PowerManager::isVbusConnected() ? "USB" : "BAT"));
    setText(sVal[0], b);

    const NavStatus& ns = NavEngine::status();
    if (ns.mapLoaded) {
        snprintf(b, sizeof(b), "%lu GB · DE (%lu Orte)", (unsigned long)max<uint32_t>(1, ns.sdTotalMB / 1024),
                 (unsigned long)ns.mapPlaces);
    } else if (ns.sdReady) {
        snprintf(b, sizeof(b), "%lu GB (%lu MB belegt)", (unsigned long)max<uint32_t>(1, ns.sdTotalMB / 1024),
                 (unsigned long)ns.sdUsedMB);
    } else {
        snprintf(b, sizeof(b), "Keine Karte erkannt");
    }
    setText(sVal[1], b);
    snprintf(b, sizeof(b), "%.1f °C", PowerManager::getTempCelsius());
    setText(sVal[2], b);
    snprintf(b, sizeof(b), "%s · %s · %s · %s",
             CompanionServer::isApActive() ? "WiFi" : "--",
             BleCompanion::isEnabled() ? "BLE" : "--",
             GpsManager::isEnabled() ? "GPS" : "--",
             RadioLoRa::isEnabled() ? "LoRa" : "--");
    setText(sVal[3], b);
    snprintf(b, sizeof(b), "%.1f MB", ESP.getFreePsram() / 1048576.0f);
    setText(sVal[4], b);
    char a[24];
    ago(a, sizeof(a), RtcManager::lastSyncEpoch());
    snprintf(b, sizeof(b), "%s  %s", RtcManager::lastSyncSource(), a);
    setText(sVal[5], b);
    unsigned long up = millis() / 1000;
    snprintf(b, sizeof(b), "%lud %02lu:%02lu (v4.1)", up / 86400, (up / 3600) % 24, (up / 60) % 60);
    setText(sVal[6], b);
}

void buildSystem(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "SYSTEM");
    lv_obj_t* c = kvCard(p, 84, 7);
    static const char* keys[7] = {"BATTERY", "SD / KARTE", "PMU TEMP", "RADIOS",
                                  "PSRAM FREE", "TIME SYNC", "UPTIME"};
    for (int i = 0; i < 7; i++) sVal[i] = kvRow(c, keys[i], i == 6);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_SYSTEM, d);
}

}  // namespace pages
