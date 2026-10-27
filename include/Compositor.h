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
    int16_t clipY0 = 0;           // only screen rows [clipY0, clipY1) are drawn
    int16_t clipY1 = H;
};

// solid rectangle drawn on top of all layers (scroll indicator)
struct Bar {
    int16_t x, y, w, h;
    uint16_t color;               // RGB565, CPU byte order
};

bool begin(uint16_t* stripA, uint16_t* stripB, int stripRows);

// mirror of everything LVGL flushes (what is on the glass right now)
void mirrorStrip(int x, int y, int w, int h, const uint16_t* px);
bool captureMirror(Surface& dst);                 // encode the glass into a surface
void setMirror(const Surface& src);               // glass now shows `src` (after a transition)

// render an LVGL object + encode (blocking). yOffset selects the window
// [yOffset, yOffset + H) of a tall object, relative to its top edge.
bool render(lv_obj_t* obj, Surface& dst, int yOffset = 0);

// incremental rendering: one strip per renderStep() (a few ms each)
bool renderBegin(lv_obj_t* obj, Surface& dst, int yOffset = 0);   // cancels a running job
bool renderStep();                                // true when the job is complete
bool renderBusy();
lv_obj_t* renderTarget();
void renderCancel();
void release(Surface& s);
void dim(const Surface& src, Surface& dst, uint16_t factor);   // factor 0..256

// compose layers (later on top) and push one frame; vsync waits for TE.
// toMirror = true also stores the frame as the glass mirror (final frame of
// an interaction that leaves LVGL's last render out of date).
void present(const Layer* layers, int n, bool vsync = true, bool toMirror = false,
             const Bar* bars = nullptr, int nBars = 0);

uint32_t lastFrameUs();
size_t bytes(const Surface& s);

extern uint32_t g_compComposeUs, g_compPushUs;    // profiling counters
}
