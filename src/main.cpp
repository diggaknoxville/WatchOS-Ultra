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
#include <esp_heap_caps.h>

TaskHandle_t uiTaskHandle = NULL;
TaskHandle_t bgTaskHandle = NULL;
static volatile bool sSubsystemsReady = false;   // setup() -> UI task: build the UI now

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
        // 100 Hz while the screen is on; 25 Hz while it is off (light sleep,
        // when allowed, happens inside PowerManager::loop)
        vTaskDelay(pdMS_TO_TICKS(PowerManager::isLowPowerState() ? 40 : 10));
    }
}

void uiTask(void* parameter) {
    Serial.println("[OS] UI compositor running on core 1");

    // LVGL is single-threaded: display driver, UI tree and timer handler all
    // live on this task. The splash appears as soon as the display driver is
    // up, while setup() still brings up the subsystems on the other task.
    KronosDisplay::begin();
    DisplayEngine::beginSplash();
    while (!sSubsystemsReady) {
        DisplayEngine::splashTick();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
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
            // AOD: 10 Hz is plenty for a clock face. SLEEP: the panel is off;
            // the touch that wakes it is read on the next pass. uiIdle() also
            // parks this task while the background task light-sleeps.
            PowerManager::uiIdle(PowerManager::getPowerState() == POWER_SLEEP ? 250 : 100);
        } else {
            vTaskDelay(pdMS_TO_TICKS(next < 2 ? 1 : (next > 5 ? 5 : next)));
        }
    }
}

// Fill the panel RAM with black (synchronous, before any UI task exists).
static void clearPanel() {
    const int rows = 16, w = instance.width(), h = instance.height();
    uint16_t* buf = (uint16_t*)heap_caps_calloc((size_t)w * rows, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) return;
    for (int y = 0; y < h; y += rows) {
        instance.pushColorsAsync(0, y, w, (y + rows > h) ? h - y : rows, buf);
        instance.waitPushComplete();
    }
    heap_caps_free(buf);
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
    // Dark and blank until the UI task shows the splash: after a soft reset
    // the AMOLED still holds the last frame (e.g. the old watch face).
    instance.setBrightness(0);
    clearPanel();
    printHardware(probe);
    RtcManager::setHardwareMask(probe);       // rtcReady only with HW_RTC_ONLINE
    MotionHaptic::setHardwareMask(probe);     // drvReady / sensorOnline from the probe

    SettingsStorage::init();      // NVS settings (the splash needs brightness + accent)
    NotificationStore::begin();   // persistent notification history

    // The UI task shows the splash now; the steps below report progress to it.
    xTaskCreatePinnedToCore(uiTask, "UiCompositor", UI_TASK_STACK, NULL, 2, &uiTaskHandle, 1);

    struct Step { const char* name; void (*fn)(); };
    static const Step steps[] = {
        {"pmu",   [] { PowerManager::init(); }},          // AXP2101 telemetry, CPU policy
        {"rtc",   [] { RtcManager::init(); }},            // UTC master clock + timezone
        {"motion",[] { MotionHaptic::init(); }},          // BHI260 steps/wrist + DRV2605
        {"gps",   [] { GpsManager::init(); }},            // GNSS time discipline + position
        {"lora",  [] { RadioLoRa::init(868.0f); }},       // SX1262, interrupt-driven RX
        {"nav",   [] { NavEngine::init(); }},             // vector map + SD detection
        {"audio", [] { AudioEngine::init(); }},           // MAX98357A
        {"web",   [] { CompanionServer::init(); }},       // phone web console (hotspot on demand)
        {"ai",    [] { AiLimitsStore::init(); }},         // AI limits persistent cache
        {"usb",   [] { UsbMscManager::init(); }},         // USB mass storage for the SD card
        {"cal",   [] { BleCompanion::preload(); }},       // cached calendar for the first face render
    };
    const int nSteps = sizeof(steps) / sizeof(steps[0]);
    for (int i = 0; i < nSteps; i++) {
        steps[i].fn();
        DisplayEngine::bootProgress((float)(i + 1) / nSteps);
    }
    sSubsystemsReady = true;      // the UI task builds the interface now

    AudioEngine::playSoundFx(SFX_BOOT);
    MotionHaptic::triggerHaptic(HAPTIC_SUCCESS);

    xTaskCreatePinnedToCore(backgroundTask, "BgServices", BG_TASK_STACK, NULL, 1, &bgTaskHandle, 0);

    Serial.println("[OS] Boot complete\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
