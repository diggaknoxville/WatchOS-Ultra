#include "KronosDisplay.h"
#include "Compositor.h"
#include "PowerManager.h"
#include "DisplayEngine.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <esp_heap_caps.h>

// Strip height of each render buffer: 2 x (410 x 24 x 2 B) = 39.3 KB internal DMA SRAM.
// Leaves >50 KB of internal 8-bit heap free so ESP-IDF WiFi AP and NimBLE stacks
// can initialize without running out of internal heap (ESP_ERR_NO_MEM).
#define STRIP_ROWS          24
#define STRIP_ROWS_FALLBACK 16

static int sStripRows = STRIP_ROWS;

// flush instrumentation shared with the UI perf monitor (defined in LV_Helper_v9.cpp)
extern volatile uint32_t g_kronosFlushUs;
extern volatile uint32_t g_kronosFlushPx;

static lv_display_t* sDisp = nullptr;
static lv_indev_t* sTouch = nullptr;

static uint32_t tickCb() { return millis(); }

static void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    uint32_t t0 = micros();
    const uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(px, w * h);                    // panel wants big-endian RGB565
    instance.pushColorsAsync(area->x1, area->y1, w, h, (uint16_t*)px);
    if (!DisplayEngine::isOverlayBlockingMirror()) {
        Compositor::mirrorStrip(area->x1, area->y1, w, h, (const uint16_t*)px);   // overlaps the DMA
    }
    g_kronosFlushUs += micros() - t0;                     // CPU time spent issuing the flush
    g_kronosFlushPx += w * h;
    // flush_ready is signalled implicitly: LVGL calls flushWaitCb before it
    // touches this buffer again.
}

static void flushWaitCb(lv_display_t*) {
    uint32_t t0 = micros();
    instance.waitPushComplete();
    g_kronosFlushUs += micros() - t0;
}

// CO5300 window constraints: start column/row even, end column/row odd.
static void rounderCb(lv_event_t* e) {
    lv_area_t* a = (lv_area_t*)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static KronosDisplay::Touch sLast = {false, 0, 0, 0};

// touch filter state (see pollTouch)
static const uint32_t RELEASE_DEBOUNCE_MS = 35;
struct RawTouch {
    bool pressed = false;
    int16_t x = 0, y = 0;
    uint32_t downAt = 0, upAt = 0, irqAt = 0;
    uint16_t frames = 0;
    uint8_t maxPoints = 0;
};
static RawTouch sRaw;
static bool sDown = false;          // debounced contact
static bool sSwallow = false;       // contact that woke the display: not reported
static bool sPalm = false;
static bool sPalmSlept = false;
static uint32_t sPalmSince = 0;

static bool sSynth = false;
static int sSx0, sSy0, sSx1, sSy1;
static uint32_t sSt0, sSdur;

void KronosDisplay::injectSwipe(int x0, int y0, int x1, int y1, uint32_t ms) {
    sSx0 = x0; sSy0 = y0; sSx1 = x1; sSy1 = y1;
    sSdur = ms ? ms : 1;
    sSt0 = millis();
    sSynth = true;
}

KronosDisplay::Touch KronosDisplay::pollTouch() {
    uint32_t now = millis();
    if (sSynth) {
        uint32_t t = now - sSt0;
        if (t <= sSdur) {
            float p = (float)t / sSdur;
            sLast.pressed = true;
            sLast.x = (int16_t)(sSx0 + (sSx1 - sSx0) * p);
            sLast.y = (int16_t)(sSy0 + (sSy1 - sSy0) * p);
        } else {
            sLast.pressed = false;
            sSynth = false;
        }
        sLast.ms = now;
        return sLast;
    }

    // ---- 1. raw controller state ------------------------------------------
    // CST9217 sends a TP_INT falling edge for each new scan frame (~70 Hz while
    // pressed) and one final TP_INT falling edge with evt=0x00 on finger release.
    // Polling touch.getPoint() when TP_INT has not fired reads an already-ACKed
    // buffer (0xAB) and falsely returns 0. Only read I2C when TP_INT fired!
    if (instance.getTouched()) {
        int16_t px[2] = {0, 0}, py[2] = {0, 0};
        if (PowerManager::lockI2C(25)) {
            uint8_t tp = instance.getPoint(px, py, 2);
            PowerManager::unlockI2C();
            if (tp > 0) {
                if (!sRaw.pressed) { sRaw.downAt = now; sRaw.frames = 0; sRaw.maxPoints = 0; }
                sRaw.pressed = true;
                sRaw.x = px[0];
                sRaw.y = py[0];
                sRaw.frames++;
                if (tp > sRaw.maxPoints) sRaw.maxPoints = tp;
                sRaw.upAt = 0;
            } else if (sRaw.pressed) {
                sRaw.pressed = false;             // evt=0x00 release frame from CST9217
                sRaw.upAt = now;
            }
            sRaw.irqAt = now;
        }
        // If I2C was busy the IRQ bit stays set and the frame is read next pass.
    } else if (sRaw.pressed && (now - sRaw.irqAt > 350)) {
        // Safety fallback if the release IRQ was ever missed: release at once
        sRaw.pressed = false;
        sRaw.upAt = now - RELEASE_DEBOUNCE_MS;
    }

    // ---- 2. palm / cover: two or more contacts --------------------------------
    // A hand or sleeve over the glass reports several points. The whole
    // contact is ignored; held for 0.7 s it switches the display off (as on
    // other watches: cover to sleep).
    if (sRaw.pressed && sRaw.maxPoints >= 2 && !sPalm) {
        sPalm = true;
        sPalmSince = now;
        sPalmSlept = false;
    }
    if (sPalm) {
        bool released = !sRaw.pressed && sRaw.upAt && (now - sRaw.upAt >= RELEASE_DEBOUNCE_MS);
        if (released) {
            sPalm = false;
        } else if (!sPalmSlept && now - sPalmSince >= 700 && !PowerManager::isLowPowerState()) {
            sPalmSlept = true;
            PowerManager::requestSleep();
        }
        sDown = false;
        sSwallow = false;
        sLast.pressed = false;
        sLast.ms = now;
        return sLast;
    }

    // ---- 3. debounced press / release ----------------------------------------
    if (!sDown) {
        // a contact must last 2 scan frames (or 24 ms): drops, sleeve brushes
        // and single-frame glitches never become taps
        if (sRaw.pressed && (sRaw.frames >= 2 || now - sRaw.downAt >= 24)) {
            sDown = true;
            // touching a dark / always-on display only wakes it; the contact
            // is not passed on, so nothing under the finger is activated
            sSwallow = PowerManager::isLowPowerState();
            PowerManager::handleActivity();
            sLast.x = sRaw.x;
            sLast.y = sRaw.y;
        }
    } else if (!sRaw.pressed) {
        // a release followed by a new contact within RELEASE_DEBOUNCE_MS is a
        // dropout of the controller, not a lift: drags no longer split into taps
        if (sRaw.upAt && now - sRaw.upAt >= RELEASE_DEBOUNCE_MS) {
            sDown = false;
            sSwallow = false;
        }
    } else {
        // jitter deadband: a resting finger does not wander by 1-2 px
        if (abs(sRaw.x - sLast.x) >= 3 || abs(sRaw.y - sLast.y) >= 3) {
            sLast.x = sRaw.x;
            sLast.y = sRaw.y;
        }
    }
    sLast.pressed = sDown && !sSwallow;
    sLast.ms = now;
    return sLast;
}

KronosDisplay::Touch KronosDisplay::lastTouch() { return sLast; }

static void touchReadCb(lv_indev_t*, lv_indev_data_t* data) {
    KronosDisplay::Touch t = KronosDisplay::pollTouch();
    if (t.pressed) {
        PowerManager::handleActivity();
    }
    data->state = t.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = t.x;
    data->point.y = t.y;
}

bool KronosDisplay::begin() {
    lv_init();
    lv_tick_set_cb(tickCb);

    const uint16_t W = instance.width(), H = instance.height();
    uint16_t stripRows = STRIP_ROWS;
    size_t bytes = (size_t)W * stripRows * sizeof(uint16_t);
    void* b1 = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void* b2 = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) {
        if (b1) heap_caps_free(b1);
        if (b2) heap_caps_free(b2);
        // Fallback: smaller strips when two 48-row contiguous chunks are unavailable
        stripRows = STRIP_ROWS_FALLBACK;
        bytes = (size_t)W * stripRows * sizeof(uint16_t);
        b1 = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        b2 = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b1 || !b2) {
            if (b1) heap_caps_free(b1);
            if (b2) heap_caps_free(b2);
            Serial.println("[DISP] internal DMA buffers unavailable");
            return false;
        }
        Serial.printf("[DISP] 48-row strips unavailable, using %u rows\n", (unsigned)stripRows);
    }
    sStripRows = stripRows;

    sDisp = lv_display_create(W, H);
    lv_display_set_color_format(sDisp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(sDisp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(sDisp, flushCb);
    lv_display_set_flush_wait_cb(sDisp, flushWaitCb);
    lv_display_add_event_cb(sDisp, rounderCb, LV_EVENT_INVALIDATE_AREA, NULL);

    Compositor::begin((uint16_t*)b1, (uint16_t*)b2, stripRows);

    sTouch = lv_indev_create();
    lv_indev_set_type(sTouch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(sTouch, touchReadCb);
    lv_indev_set_display(sTouch, sDisp);
    lv_indev_set_scroll_limit(sTouch, 14);         // a resting finger is a tap, not a scroll
    lv_indev_set_long_press_time(sTouch, 450);

    Serial.printf("[DISP] %ux%u, 2x %u-row strips in internal DMA RAM (%u KB), heap left %u KB\n",
                  W, H, stripRows, (unsigned)(2 * bytes / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

lv_display_t* KronosDisplay::display() { return sDisp; }
lv_indev_t* KronosDisplay::touch() { return sTouch; }
int KronosDisplay::stripRows() { return sStripRows; }
