#include "Compositor.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <esp_heap_caps.h>
// private LVGL APIs: offscreen strip rendering (same mechanism as lv_snapshot)
#include "src/draw/lv_draw_private.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"

namespace Compositor {

static const int RUN_MIN = 6;                 // shorter repeats are stored as literals
static const uint16_t LIT = 0x8000;

static uint16_t* sStrip[2] = {nullptr, nullptr};
static int sRows = 48;
static uint16_t* sMirror = nullptr;           // raw W*H, panel byte order
static uint16_t* sScratchEnc = nullptr;       // worst-case encode buffer
static size_t sScratchEncWords = 0;
// Incremental encoding of the glass: every mirror row keeps its RLE tokens in a
// fixed slot; flushes only mark rows dirty, so capturing the glass re-encodes
// just the rows LVGL touched since the last capture.
static const int ROW_CAP = W + W / 2 + 4;     // worst-case tokens per row (words)
static uint16_t* sRowEnc = nullptr;           // H * ROW_CAP words, PSRAM
static uint16_t sRowLen[H];
static uint8_t sRowDirty[H];
static SemaphoreHandle_t sVsync = nullptr;
static uint32_t sLastFrameUs = 0;
uint32_t g_compComposeUs = 0, g_compPushUs = 0;

static void IRAM_ATTR teIsr() {
    BaseType_t woken = pdFALSE;
    if (sVsync) xSemaphoreGiveFromISR(sVsync, &woken);
    if (woken) portYIELD_FROM_ISR();
}

bool begin(uint16_t* stripA, uint16_t* stripB, int stripRows) {
    sStrip[0] = stripA;
    sStrip[1] = stripB;
    sRows = stripRows;
    sMirror = (uint16_t*)heap_caps_calloc(W * H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // worst case per row: alternating literal/run tokens -> < W + W/2 words, plus 2 offset words
    sScratchEncWords = (size_t)H * 2 + (size_t)H * (W + W / 2 + 4);
    sScratchEnc = (uint16_t*)heap_caps_malloc(sScratchEncWords * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    sRowEnc = (uint16_t*)heap_caps_malloc((size_t)H * ROW_CAP * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    memset(sRowDirty, 1, sizeof(sRowDirty));
    sVsync = xSemaphoreCreateBinary();
    pinMode(DISP_TE, INPUT);
    attachInterrupt(DISP_TE, teIsr, RISING);
    bool ok = sMirror && sScratchEnc && sRowEnc;
    Serial.printf("[COMP] compositor %s (vsync GPIO%d)\n", ok ? "ready" : "ALLOC FAILED", DISP_TE);
    return ok;
}

void mirrorStrip(int x, int y, int w, int h, const uint16_t* px) {
    if (!sMirror) return;
    for (int r = 0; r < h; r++) {
        memcpy(sMirror + (y + r) * W + x, px + r * w, w * sizeof(uint16_t));
        sRowDirty[y + r] = 1;
    }
}

// ---------------------------------------------------------------- encoding
static size_t encodeRow(const uint16_t* row, uint16_t* out) {
    size_t o = 0;
    int x = 0;
    int litStart = 0;
    while (x < W) {
        uint16_t c = row[x];
        int r = x + 1;
        while (r < W && row[r] == c) r++;
        int len = r - x;
        if (len >= RUN_MIN) {
            if (x > litStart) {                               // flush pending literal
                int n = x - litStart;
                out[o++] = LIT | (uint16_t)n;
                memcpy(out + o, row + litStart, n * sizeof(uint16_t));
                o += n;
            }
            out[o++] = (uint16_t)len;
            out[o++] = c;
            litStart = r;
        }
        x = r;
    }
    if (W > litStart) {
        int n = W - litStart;
        out[o++] = LIT | (uint16_t)n;
        memcpy(out + o, row + litStart, n * sizeof(uint16_t));
        o += n;
    }
    return o;
}

// assemble a surface from the per-row encodings, re-encoding dirty rows only
bool captureMirror(Surface& dst) {
    if (!sMirror || !sRowEnc) return false;
    static uint16_t rowIn[W];
    static uint16_t rowOut[ROW_CAP];
    size_t total = (size_t)H * 2;
    for (int y = 0; y < H; y++) {
        if (sRowDirty[y]) {
            memcpy(rowIn, sMirror + y * W, sizeof(rowIn));
            size_t n = encodeRow(rowIn, rowOut);
            memcpy(sRowEnc + (size_t)y * ROW_CAP, rowOut, n * sizeof(uint16_t));
            sRowLen[y] = (uint16_t)n;
            sRowDirty[y] = 0;
        }
        total += sRowLen[y];
    }
    if (dst.words < total) {
        if (dst.data) heap_caps_free(dst.data);
        dst.data = (uint16_t*)heap_caps_malloc(total * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        dst.words = dst.data ? total : 0;
    }
    if (!dst.data) { dst.valid = false; return false; }
    size_t pos = (size_t)H * 2;
    for (int y = 0; y < H; y++) {
        dst.data[2 * y] = (uint16_t)(pos & 0xFFFF);
        dst.data[2 * y + 1] = (uint16_t)(pos >> 16);
        memcpy(dst.data + pos, sRowEnc + (size_t)y * ROW_CAP, sRowLen[y] * sizeof(uint16_t));
        pos += sRowLen[y];
    }
    dst.valid = true;
    dst.renderedAt = millis();
    return true;
}

static void decodeSpan(const Surface& s, int sy, int sx0, int sx1, uint16_t* out, uint16_t dimF);

// The glass now shows `src`: refresh the raw mirror and adopt src's row tokens.
void setMirror(const Surface& src) {
    if (!sMirror || !src.valid) return;
    static uint16_t row[W];
    for (int y = 0; y < H; y++) {
        decodeSpan(src, y, 0, W, row, 256);
        memcpy(sMirror + y * W, row, sizeof(row));
        size_t p = src.data[2 * y] | ((size_t)src.data[2 * y + 1] << 16), q = p;
        for (int x = 0; x < W;) {                        // token length of this row
            uint16_t h = src.data[q];
            int len = h & 0x7FFF;
            q += (h & LIT) ? 1 + len : 2;
            x += len;
        }
        if (sRowEnc && q - p <= (size_t)ROW_CAP) {
            memcpy(sRowEnc + (size_t)y * ROW_CAP, src.data + p, (q - p) * sizeof(uint16_t));
            sRowLen[y] = (uint16_t)(q - p);
            sRowDirty[y] = 0;
        } else {
            sRowDirty[y] = 1;
        }
    }
}

// ---------------------------------------------------------------- strip renderer
// Renders an LVGL object strip by strip into the internal DMA strip buffer and
// encodes each strip immediately. A step blocks the UI task for only a few ms,
// so pages can be prepared in the background without hurting touch latency.
struct Job {
    lv_obj_t* obj = nullptr;
    Surface* dst = nullptr;
    int y = 0;
    int yOff = 0;                 // window start inside a tall object
    size_t pos = 0;
};
static Job sJob;

// make `obj` drawable without letting LVGL repaint the glass
static bool showForRender(lv_obj_t* obj) {
    bool wasHidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (wasHidden) {
        lv_display_t* d = lv_display_get_default();
        lv_display_enable_invalidation(d, false);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_display_enable_invalidation(d, true);
    }
    return wasHidden;
}

static void hideAfterRender(lv_obj_t* obj, bool wasHidden) {
    if (!wasHidden) return;
    lv_display_t* d = lv_display_get_default();
    lv_display_enable_invalidation(d, false);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_display_enable_invalidation(d, true);
}

void renderCancel() { sJob = Job(); }
bool renderBusy() { return sJob.obj != nullptr; }
lv_obj_t* renderTarget() { return sJob.obj; }

bool renderBegin(lv_obj_t* obj, Surface& dst, int yOffset) {
    if (!obj || !sScratchEnc) return false;
    sJob.obj = obj;
    sJob.dst = &dst;
    sJob.y = 0;
    sJob.yOff = yOffset;
    sJob.pos = (size_t)H * 2;
    bool wasHidden = showForRender(obj);
    lv_display_t* d = lv_display_get_default();
    lv_display_enable_invalidation(d, false);
    lv_obj_update_layout(obj);
    lv_display_enable_invalidation(d, true);
    hideAfterRender(obj, wasHidden);
    return true;
}

bool renderStep() {
    if (!sJob.obj) return true;
    lv_obj_t* obj = sJob.obj;
    instance.waitPushComplete();                        // strip buffer may still feed a DMA
    const int y0 = sJob.y;
    const int rows = (y0 + sRows > H) ? H - y0 : sRows;
    uint16_t* buf = sStrip[1];

    lv_draw_buf_t db;
    lv_draw_buf_init(&db, W, rows, LV_COLOR_FORMAT_RGB565, W * sizeof(uint16_t), buf, W * rows * sizeof(uint16_t));
    memset(buf, 0, W * rows * sizeof(uint16_t));

    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    lv_area_t area = {oc.x1, oc.y1 + sJob.yOff + y0, oc.x1 + W - 1, oc.y1 + sJob.yOff + y0 + rows - 1};

    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = &db;
    layer.buf_area = area;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = area;
    layer.phy_clip_area = area;

    bool wasHidden = showForRender(obj);
    lv_display_t* disp = lv_obj_get_display(obj);
    lv_display_t* dispOld = lv_refr_get_disp_refreshing();
    lv_layer_t* headOld = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    lv_obj_redraw(&layer, obj);
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    disp->layer_head = headOld;
    lv_refr_set_disp_refreshing(dispOld);
    hideAfterRender(obj, wasHidden);

    lv_draw_sw_rgb565_swap(buf, W * rows);             // -> panel byte order
    static uint16_t rowOut[W + W / 2 + 4];
    for (int r = 0; r < rows; r++) {
        size_t n = encodeRow(buf + r * W, rowOut);
        int y = y0 + r;
        sScratchEnc[2 * y] = (uint16_t)(sJob.pos & 0xFFFF);
        sScratchEnc[2 * y + 1] = (uint16_t)(sJob.pos >> 16);
        memcpy(sScratchEnc + sJob.pos, rowOut, n * sizeof(uint16_t));
        sJob.pos += n;
    }
    sJob.y += rows;
    if (sJob.y < H) return false;

    Surface& dst = *sJob.dst;
    size_t total = sJob.pos;
    if (dst.words < total) {
        if (dst.data) heap_caps_free(dst.data);
        dst.data = (uint16_t*)heap_caps_malloc(total * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        dst.words = dst.data ? total : 0;
    }
    if (dst.data) {
        memcpy(dst.data, sScratchEnc, total * sizeof(uint16_t));
        dst.valid = true;
        dst.renderedAt = millis();
    } else {
        dst.valid = false;
    }
    sJob = Job();
    return true;
}

bool render(lv_obj_t* obj, Surface& dst, int yOffset) {
    if (!renderBegin(obj, dst, yOffset)) return false;
    while (!renderStep()) {}
    return dst.valid;
}

void release(Surface& s) {
    if (s.data) heap_caps_free(s.data);
    s.data = nullptr;
    s.words = 0;
    s.valid = false;
}

size_t bytes(const Surface& s) { return s.words * sizeof(uint16_t); }

static inline uint16_t dimPx(uint16_t p, uint16_t f) {
    uint16_t v = (uint16_t)((p >> 8) | (p << 8));
    uint32_t r = ((v >> 11) & 0x1F) * f >> 8, g = ((v >> 5) & 0x3F) * f >> 8, b = (v & 0x1F) * f >> 8;
    v = (uint16_t)((r << 11) | (g << 5) | b);
    return (uint16_t)((v >> 8) | (v << 8));
}

void dim(const Surface& src, Surface& dst, uint16_t f) {
    if (!src.valid) return;
    if (dst.words < src.words) {
        if (dst.data) heap_caps_free(dst.data);
        dst.data = (uint16_t*)heap_caps_malloc(src.words * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        dst.words = dst.data ? src.words : 0;
    }
    if (!dst.data) return;
    memcpy(dst.data, src.data, (size_t)H * 2 * sizeof(uint16_t));
    for (int y = 0; y < H; y++) {
        size_t p = src.data[2 * y] | ((size_t)src.data[2 * y + 1] << 16);
        int x = 0;
        while (x < W) {
            uint16_t h = src.data[p];
            dst.data[p++] = h;
            int len = h & 0x7FFF;
            if (h & LIT) { for (int i = 0; i < len; i++, p++) dst.data[p] = dimPx(src.data[p], f); }
            else { dst.data[p] = dimPx(src.data[p], f); p++; }
            x += len;
        }
    }
    dst.valid = true;
    dst.renderedAt = millis();
}

// ---------------------------------------------------------------- composing
static inline void fill16(uint16_t* d, uint16_t c, int n) {
    if ((c >> 8) == (c & 0xFF)) { memset(d, c & 0xFF, n * 2); return; }
    if (((uintptr_t)d & 2) && n) { *d++ = c; n--; }
    uint32_t c2 = ((uint32_t)c << 16) | c;
    uint32_t* d32 = (uint32_t*)d;
    for (int i = 0; i < n / 2; i++) d32[i] = c2;
    if (n & 1) d[n - 1] = c;
}

// decode surface row `sy`, columns [sx0, sx1) into out[0..), optionally dimmed
static void decodeSpan(const Surface& s, int sy, int sx0, int sx1, uint16_t* out, uint16_t dimF) {
    const uint16_t* d = s.data;
    size_t p = d[2 * sy] | ((size_t)d[2 * sy + 1] << 16);
    int x = 0;
    const bool dimmed = dimF < 256;
    while (x < sx1) {
        uint16_t h = d[p];
        int len = h & 0x7FFF;
        int a = x > sx0 ? x : sx0, b = (x + len) < sx1 ? (x + len) : sx1;
        if (h & LIT) {
            if (b > a) {
                uint16_t* o = out + (a - sx0);
                const uint16_t* q = d + p + 1 + (a - x);
                if (!dimmed) memcpy(o, q, (b - a) * sizeof(uint16_t));
                else for (int i = 0; i < b - a; i++) o[i] = dimPx(q[i], dimF);
            }
            p += 1 + len;
        } else {
            if (b > a) fill16(out + (a - sx0), dimmed ? dimPx(d[p + 1], dimF) : d[p + 1], b - a);
            p += 2;
        }
        x += len;
    }
}

static inline bool layerRow(const Layer& L, int y) {
    return L.src && L.src->valid && y >= L.clipY0 && y < L.clipY1 && y - L.y >= 0 && y - L.y < H;
}

static void composeStrip(int y0, int rows, const Layer* layers, int n, uint16_t* out,
                         const Bar* bars, int nBars) {
    for (int r = 0; r < rows; r++) {
        int y = y0 + r;
        uint16_t* o = out + r * W;
        // black where no layer covers this row (cheap check: full coverage by two layers is the norm)
        int covered = 0;
        for (int i = 0; i < n; i++) {
            const Layer& L = layers[i];
            if (layerRow(L, y)) {
                int x0 = L.x < 0 ? 0 : L.x, x1 = L.x + W > W ? W : L.x + W;
                if (x1 > x0) covered += x1 - x0;
            }
        }
        if (covered < W) memset(o, 0, W * sizeof(uint16_t));
        for (int i = 0; i < n; i++) {
            const Layer& L = layers[i];
            if (!layerRow(L, y)) continue;
            int sy = y - L.y;
            int x0 = L.x < 0 ? 0 : L.x, x1 = L.x + W > W ? W : L.x + W;
            if (x1 <= x0) continue;
            decodeSpan(*L.src, sy, x0 - L.x, x1 - L.x, o + x0, L.dim);
        }
        for (int b = 0; b < nBars; b++) {
            const Bar& B = bars[b];
            if (y < B.y || y >= B.y + B.h) continue;
            int x0 = B.x < 0 ? 0 : B.x, x1 = B.x + B.w > W ? W : B.x + B.w;
            if (x1 <= x0) continue;
            fill16(o + x0, (uint16_t)((B.color >> 8) | (B.color << 8)), x1 - x0);   // panel byte order
        }
    }
}

static inline void toMirrorRows(int y0, int rows, const uint16_t* px) {
    if (!sMirror) return;
    memcpy(sMirror + y0 * W, px, (size_t)rows * W * sizeof(uint16_t));
    memset(sRowDirty + y0, 1, rows);
}

void present(const Layer* layers, int n, bool vsync, bool toMirror, const Bar* bars, int nBars) {
    uint32_t t0 = micros();
    instance.waitPushComplete();
    int cur = 0;
    int rows0 = sRows < H ? sRows : H;
    uint32_t tc = micros();
    composeStrip(0, rows0, layers, n, sStrip[0], bars, nBars);
    g_compComposeUs += micros() - tc;
    if (vsync && sVsync) {
        xSemaphoreTake(sVsync, 0);                         // drop a stale edge
        xSemaphoreTake(sVsync, pdMS_TO_TICKS(20));         // start on the next vblank
    }
    for (int y0 = 0; y0 < H; y0 += sRows) {
        int rows = (y0 + sRows > H) ? H - y0 : sRows;
        uint32_t tp = micros();
        instance.pushColorsAsync(0, y0, W, rows, sStrip[cur]);
        g_compPushUs += micros() - tp;
        if (toMirror) toMirrorRows(y0, rows, sStrip[cur]);              // DMA only reads the strip
        int y1 = y0 + sRows;
        if (y1 < H) {
            int rows1 = (y1 + sRows > H) ? H - y1 : sRows;
            tc = micros();
            composeStrip(y1, rows1, layers, n, sStrip[cur ^ 1], bars, nBars);   // overlaps the DMA of strip y0
            g_compComposeUs += micros() - tc;
        }
        cur ^= 1;
    }
    instance.waitPushComplete();
    sLastFrameUs = micros() - t0;
}

uint32_t lastFrameUs() { return sLastFrameUs; }

}  // namespace Compositor
