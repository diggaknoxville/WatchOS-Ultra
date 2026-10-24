#include <Arduino.h>
#include "Config.h"
#include <LilyGoLib.h>           // T-Watch Ultra BSP: global `instance`
#include "KronosDisplay.h"       // LVGL display (async DMA) + touch driver
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "MotionHaptic.h"
#include "RadioLoRa.h"
#include "AudioEngine.h"
#include "GpsManager.h"
#include "DisplayEngine.h"
#include "CompanionServer.h"
#include "NotificationStore.h"

TaskHandle_t uiTaskHandle = NULL;
TaskHandle_t bgTaskHandle = NULL;

void backgroundTask(void* parameter) {
    Serial.println("[OS] Background services running on core 0");

    for (;;) {
        unsigned long now = millis();
        CompanionServer::handleClient();
        PowerManager::loop(now);
        MotionHaptic::updateMotion(now);
        GpsManager::loop(now);
        RadioLoRa::loop(now);
        RtcManager::periodicMaintenance();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void uiTask(void* parameter) {
    Serial.println("[OS] UI compositor running on core 1");

    // LVGL is single-threaded: display driver, UI tree and timer handler all
    // live on this task.
    KronosDisplay::begin();
    DisplayEngine::init();

    for (;;) {
        unsigned long now = millis();
        // any touch interaction keeps the display awake
        if (lv_display_get_inactive_time(NULL) < 150) {
            PowerManager::handleActivity();
        }
        DisplayEngine::update(now);
        uint32_t next = lv_timer_handler();          // ms until LVGL needs us again
        vTaskDelay(pdMS_TO_TICKS(next < 2 ? 1 : (next > 5 ? 5 : next)));
    }
}

static void printHardware(uint32_t p) {
    struct { uint32_t bit; const char* name; } hw[] = {
        {HW_PMU_ONLINE, "PMU AXP2101"}, {HW_RTC_ONLINE, "RTC PCF85063"},
        {HW_TOUCH_ONLINE, "Touch"}, {HW_DRV_ONLINE, "Haptic DRV2605"},
        {HW_BHI260AP_ONLINE, "IMU BHI260AP"}, {HW_GPS_ONLINE, "GNSS u-blox"},
        {HW_RADIO_ONLINE, "LoRa SX1262"}, {HW_NFC_ONLINE, "NFC ST25R3916"},
        {HW_SD_ONLINE, "microSD"}, {HW_EXPAND_ONLINE, "IO XL9555"},
        {HW_PSRAM_ONLINE, "PSRAM"},
    };
    Serial.println("[OS] Hardware inventory:");
    for (auto& h : hw) Serial.printf("       %-16s %s\n", h.name, (p & h.bit) ? "online" : "--");
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=======================================================");
    Serial.println("        KRONOS OS  //  LilyGo T-Watch Ultra            ");
    Serial.println("=======================================================");

    // 0. Full hardware bring-up (AMOLED, touch, AXP2101, PCF85063, BHI260,
    //    DRV2605, GNSS, SX1262, amplifier) via LilyGoLib.
    uint32_t probe = instance.begin();
    instance.setBrightness(180);
    printHardware(probe);

    SettingsStorage::init();      // NVS settings
    NotificationStore::begin();   // persistent notification history
    PowerManager::init();         // AXP2101 telemetry + backlight
    RtcManager::init();           // UTC master clock + timezone
    MotionHaptic::init();         // BHI260 steps/wrist + DRV2605
    GpsManager::init();           // GNSS time discipline + position
    RadioLoRa::init(868.0f);      // SX1262, interrupt-driven RX
    AudioEngine::init();          // MAX98357A
    CompanionServer::init();      // phone web console (WiFi AP)
    // The LVGL UI is built on the UI task (see uiTask).

    AudioEngine::playSoundFx(SFX_BOOT);
    MotionHaptic::triggerHaptic(HAPTIC_SUCCESS);

    xTaskCreatePinnedToCore(uiTask, "UiCompositor", 16384, NULL, 2, &uiTaskHandle, 1);
    xTaskCreatePinnedToCore(backgroundTask, "BgServices", 10240, NULL, 1, &bgTaskHandle, 0);

    Serial.println("[OS] Boot complete\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
