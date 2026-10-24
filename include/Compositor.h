#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <stddef.h>

// Frame compositor for full-screen transitions.
//
// LVGL's software renderer needs 40-120 ms for a complex full screen on this
// MCU, far too slow for interactive page swipes, and PSRAM bandwidth (~28 MB/s,
// measured for CPU, GDMA and dual-core copies alike) caps raw full-frame
// copies at ~14 ms. Pages are therefore cached as run-length encoded surfaces:
// the AMOLED design is mostly true black and solid cards, so runs are filled
// in fast internal RAM and only literal pixels (glyph edges, icons) are read
// from PSRAM. Frames are composed strip by strip into the internal DMA buffers
// and pushed synchronised to the panel's tearing-effect (vsync) signal.
namespace Compositor {

static const int W = 410;
static const int H = 502;

// Run-length encoded full-screen surface (panel byte order).
// Row stream: tokens of [hdr][...]; hdr bit15 = literal, bits0..14 = length.
// run:     hdr, colour            literal: hdr, length x pixel
struct Surface {
    uint16_t* data = nullptr;     // PSRAM: H row offsets (as 2 words each) + token stream
    size_t words = 0;             // allocated size in 16-bit words
    bool valid = false;
    uint32_t renderedAt = 0;      // millis()
};

struct Layer {
    const Surface* src;           // nullptr = solid black
    int x, y;                     // top-left of the surface on screen
    uint16_t dim = 256;           // brightness factor 0..256 (256 = unchanged)
};

bool begin(uint16_t* stripA, uint16_t* stripB, int stripRows);

// mirror of everything LVGL flushes (what is on the glass right now)
void mirrorStrip(int x, int y, int w, int h, const uint16_t* px);
bool captureMirror(Surface& dst);                 // encode the glass into a surface
void setMirror(const Surface& src);               // glass now shows `src` (after a transition)

bool render(lv_obj_t* obj, Surface& dst);         // render an LVGL object + encode (blocking)

// incremental rendering: one strip per renderStep() (a few ms each)
bool renderBegin(lv_obj_t* obj, Surface& dst);    // cancels a running job
bool renderStep();                                // true when the job is complete
bool renderBusy();
lv_obj_t* renderTarget();
void renderCancel();
void release(Surface& s);
void dim(const Surface& src, Surface& dst, uint16_t factor);   // factor 0..256

// compose layers (later on top) and push one frame; vsync waits for TE
void present(const Layer* layers, int n, bool vsync = true);

uint32_t lastFrameUs();
size_t bytes(const Surface& s);

extern uint32_t g_compComposeUs, g_compPushUs;    // profiling counters
}
