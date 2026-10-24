#include "KronosDisplay.h"
#include "Compositor.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <esp_heap_caps.h>

// Strip height of each render buffer. 2 x (410 x 48 x 2 B) = 79 KB internal SRAM.
#define STRIP_ROWS 48

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
    Compositor::mirrorStrip(area->x1, area->y1, w, h, (const uint16_t*)px);   // overlaps the DMA
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
    if (sSynth) {
        uint32_t t = millis() - sSt0;
        if (t <= sSdur) {
            float p = (float)t / sSdur;
            sLast.pressed = true;
            sLast.x = (int16_t)(sSx0 + (sSx1 - sSx0) * p);
            sLast.y = (int16_t)(sSy0 + (sSy1 - sSy0) * p);
        } else {
            sLast.pressed = false;
            sSynth = false;
        }
        sLast.ms = millis();
        return sLast;
    }
    int16_t px, py;
    if (instance.getPoint(&px, &py, 1)) {
        sLast.pressed = true;
        sLast.x = px;
        sLast.y = py;
    } else {
        sLast.pressed = false;                // keep the last coordinates for release handling
    }
    sLast.ms = millis();
    return sLast;
}

KronosDisplay::Touch KronosDisplay::lastTouch() { return sLast; }

static void touchReadCb(lv_indev_t*, lv_indev_data_t* data) {
    KronosDisplay::Touch t = KronosDisplay::pollTouch();
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
        // Fallback to 32 rows if 48 rows contiguous chunk is unavailable
        stripRows = 32;
        bytes = (size_t)W * stripRows * sizeof(uint16_t);
        b1 = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        b2 = heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b1 || !b2) {
            Serial.println("[DISP] internal DMA buffers unavailable");
            return false;
        }
    }

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

    Serial.printf("[DISP] %ux%u, 2x %u-row strips in internal DMA RAM (%u KB), heap left %u KB\n",
                  W, H, STRIP_ROWS, (unsigned)(2 * bytes / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

lv_display_t* KronosDisplay::display() { return sDisp; }
lv_indev_t* KronosDisplay::touch() { return sTouch; }
