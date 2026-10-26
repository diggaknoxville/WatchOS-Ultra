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
#include "NavEngine.h"
#include "DisplayEngine.h"
#include "CompanionServer.h"
#include "NotificationStore.h"
#include "AiLimitsStore.h"
#include "BleCompanion.h"
#include "UsbMscManager.h"

TaskHandle_t uiTaskHandle = NULL;
TaskHandle_t bgTaskHandle = NULL;

// Stack sizes. The background task hosts the WebServer request handlers
// (multipart OTA/SD upload, JSON building), NavEngine's asynchronous route
// computation, Meshtastic packet decoding and the BLE payload parser; 12 KB
// left no headroom. The UI task builds LVGL pages with several KB of local
// arrays (place pools, peer tables) inside event callbacks.
static const uint32_t UI_TASK_STACK = 20 * 1024;
static const uint32_t BG_TASK_STACK = 14 * 1024;

void backgroundTask(void* parameter) {
    Serial.println("[OS] Background services running on core 0");

    // Wait until UI compositor has initialized and claimed display DMA buffers
    while (!DisplayEngine::isReady()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    BleCompanion::init();

    for (;;) {
        unsigned long now = millis();
        CompanionServer::handleClient();
        BleCompanion::loop(now);
        PowerManager::loop(now);
        MotionHaptic::updateMotion(now);
        GpsManager::loop(now);
        RadioLoRa::loop(now);
        NavEngine::loop(now);
        UsbMscManager::loop(now);
        SettingsStorage::loop(now);
        NotificationStore::loop(now);
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
        // Any touch interaction keeps the display awake. PowerManager ignores
        // this for 800 ms after requestSleep() so the triggering touch cannot
        // re-wake the display.
        if (lv_display_get_inactive_time(NULL) < 150) {
            PowerManager::handleActivity();
        }
        DisplayEngine::update(now);
        uint32_t next = lv_timer_handler();          // ms until LVGL needs us again

        if (PowerManager::isLowPowerState()) {
            // AOD: 10 Hz is plenty for a clock face and touch-to-wake.
            // SLEEP: display is off; 4 Hz keeps touch/key wake responsive
            // without burning CPU on invisible rendering.
            vTaskDelay(pdMS_TO_TICKS(PowerManager::getPowerState() == POWER_SLEEP ? 250 : 100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(next < 2 ? 1 : (next > 5 ? 5 : next)));
        }
    }
}

static void printHardware(uint32_t p) {
    struct { uint32_t bit; const char* name; } hw[] = {
        {HW_PMU_ONLINE, "PMU AXP2101"}, {HW_RTC_ONLINE, "RTC PCF85063"},
        {HW_TOUCH_ONLINE, "Touch CST9217"}, {HW_DRV_ONLINE, "Haptic DRV2605"},
        {HW_BHI260AP_ONLINE, "IMU BHI260AP"}, {HW_GPS_ONLINE, "GNSS u-blox"},
        {HW_RADIO_ONLINE, "LoRa SX1262"}, {HW_NFC_ONLINE, "NFC ST25R3916"},
        {HW_SD_ONLINE, "microSD"}, {HW_EXPAND_ONLINE, "IO XL9555"},
        {HW_PSRAM_ONLINE, "PSRAM"},
    };
    Serial.println("[OS] Hardware inventory:");
    for (auto& h : hw) Serial.printf("       %-16s %s\n", h.name, (p & h.bit) ? "online" : "--");
}

void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);
    Serial.setTxTimeoutMs(50);
    delay(200);
    Serial.println("\n=======================================================");
    Serial.printf("   WatchOS Ultra %s (build %s)  //  LilyGo T-Watch Ultra\n", OS_VERSION, OS_BUILD);
    Serial.println("=======================================================");

    // 0. Full hardware bring-up (AMOLED, touch, AXP2101, PCF85063, BHI260,
    //    DRV2605, GNSS, SX1262, SD, amplifier) via LilyGoLib.
    //    Skip unused FFat (/fs) and NFC to preserve >55 KB internal SRAM for WiFi + BLE + SD.
    uint32_t probe = instance.begin(NO_INIT_FATFS | NO_HW_NFC);
    instance.setBrightness(180);
    printHardware(probe);
    RtcManager::setHardwareMask(probe);       // rtcReady only with HW_RTC_ONLINE
    MotionHaptic::setHardwareMask(probe);     // drvReady / sensorOnline from the probe

    SettingsStorage::init();      // NVS settings
    NotificationStore::begin();   // persistent notification history
    PowerManager::init();         // AXP2101 telemetry + backlight
    RtcManager::init();           // UTC master clock + timezone
    MotionHaptic::init();         // BHI260 steps/wrist + DRV2605
    GpsManager::init();           // GNSS time discipline + position
    RadioLoRa::init(868.0f);      // SX1262, interrupt-driven RX
    NavEngine::init();            // Vector map + SD detection early
    AudioEngine::init();          // MAX98357A
    CompanionServer::init();      // phone web console (WiFi AP)
    AiLimitsStore::init();        // AI limits persistent cache
    UsbMscManager::init();        // USB Mass Storage class for SD card
    // The LVGL UI is built on the UI task (see uiTask).

    AudioEngine::playSoundFx(SFX_BOOT);
    MotionHaptic::triggerHaptic(HAPTIC_SUCCESS);

    xTaskCreatePinnedToCore(uiTask, "UiCompositor", UI_TASK_STACK, NULL, 2, &uiTaskHandle, 1);
    xTaskCreatePinnedToCore(backgroundTask, "BgServices", BG_TASK_STACK, NULL, 1, &bgTaskHandle, 0);

    Serial.println("[OS] Boot complete\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
