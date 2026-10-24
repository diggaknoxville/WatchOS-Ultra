// System: power, memory, time synchronisation and firmware.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "Config.h"

using namespace ui;

namespace pages {

static lv_obj_t* sVal[8];

static void refresh(bool) {
    char b[48];
    int bat = PowerManager::getBatteryPercent();
    bool chg = PowerManager::isCharging();
    snprintf(b, sizeof(b), "%d %%", bat);                                 setText(sVal[0], b);
    snprintf(b, sizeof(b), "%.2f V", PowerManager::getBatteryVoltage());  setText(sVal[1], b);
    setText(sVal[2], chg ? "Charging" : (PowerManager::isVbusConnected() ? "USB" : "Battery"));
    snprintf(b, sizeof(b), "%.1f °C", PowerManager::getTempCelsius());   setText(sVal[3], b);
    snprintf(b, sizeof(b), "%.1f MB", ESP.getFreePsram() / 1048576.0f);   setText(sVal[4], b);
    char a[24];
    ago(a, sizeof(a), RtcManager::lastSyncEpoch());
    snprintf(b, sizeof(b), "%s  %s", RtcManager::lastSyncSource(), a);   setText(sVal[5], b);
    unsigned long up = millis() / 1000;
    snprintf(b, sizeof(b), "%lud %02lu:%02lu", up / 86400, (up / 3600) % 24, (up / 60) % 60);
    setText(sVal[6], b);
    setText(sVal[7], OS_VERSION);
}

void buildSystem(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "SYSTEM");
    lv_obj_t* c = kvCard(p, 94, 8);
    static const char* keys[8] = {"BATTERY", "VOLTAGE", "POWER", "PMU TEMP",
                                  "PSRAM FREE", "TIME SYNC", "UPTIME", "FIRMWARE"};
    for (int i = 0; i < 8; i++) sVal[i] = kvRow(c, keys[i], i == 7);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 15000;
    nav::registerPage(nav::PG_SYSTEM, d);
}

}  // namespace pages
