#pragma once
// ============================================================================
//  KRONOS UI kit: design tokens, typography and building blocks shared by
//  every page. All functions must be called on the UI task (LVGL thread).
// ============================================================================
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include "MotionHaptic.h"
#include "ui/Icons.h"

namespace ui {

// ---- geometry --------------------------------------------------------------
constexpr int SCR_W = 410;
constexpr int SCR_H = 502;
constexpr int M = 24;                  // outer margin / grid unit
constexpr int CW = SCR_W - 2 * M;      // content width (362)
constexpr int R_CARD = 28;             // card corner radius

// ---- colour tokens (true black: AMOLED pixels off) -------------------------
inline lv_color_t cBg()     { return lv_color_hex(0x000000); }
inline lv_color_t cSurf()   { return lv_color_hex(0x0F1218); }
inline lv_color_t cSurf2()  { return lv_color_hex(0x181C25); }
inline lv_color_t cStroke() { return lv_color_hex(0x252A35); }
inline lv_color_t cText()   { return lv_color_hex(0xF4F6FA); }
inline lv_color_t cText2()  { return lv_color_hex(0x9BA4B4); }
inline lv_color_t cText3()  { return lv_color_hex(0x5A6270); }
inline lv_color_t cOk()     { return lv_color_hex(0x30D158); }
inline lv_color_t cWarn()   { return lv_color_hex(0xFF9F0A); }
inline lv_color_t cCrit()   { return lv_color_hex(0xFF453A); }
inline lv_color_t cInfo()   { return lv_color_hex(0x64D2FF); }
lv_color_t accent();                   // user accent colour (settings)

// ---- typography ------------------------------------------------------------
struct Fonts {
    lv_font_t *hero, *giant, *numL, *numM, *numS;   // Orbitron numerals
    lv_font_t *title, *body, *small, *cap;          // Rajdhani UI text
    const lv_font_t *icon, *iconS, *iconL;          // LV_SYMBOL_* glyphs (legacy)
    lv_font_t *icoS, *icoM, *icoL, *icoXL;          // Material Symbols (ui/Icons.h)
};
extern Fonts F;
void initFonts();

// ---- accent registry: objects whose colour follows the user accent ---------
enum AccKind : uint8_t { ACC_TEXT, ACC_BG, ACC_ARC, ACC_LINE, ACC_BORDER };
void accReg(lv_obj_t* o, AccKind k, lv_part_t part = LV_PART_MAIN);
void applyAccent();

// ---- building blocks -------------------------------------------------------
lv_obj_t* label(lv_obj_t* p, const lv_font_t* f, lv_color_t c, const char* t = "");
lv_obj_t* caption(lv_obj_t* p, const char* t, lv_color_t c = cText2());
lv_obj_t* box(lv_obj_t* p, int w, int h);                 // transparent, no scroll
lv_obj_t* card(lv_obj_t* p, int w, int h);
lv_obj_t* ring(lv_obj_t* p, int size, int width, lv_color_t col);
lv_obj_t* pageRoot(lv_obj_t* parent);                     // full-screen page container
void title(lv_obj_t* page, const char* text);             // page title + accent underline
lv_obj_t* kvCard(lv_obj_t* page, int y, int rows);
lv_obj_t* kvRow(lv_obj_t* card, const char* key, bool last);
lv_obj_t* iconText(lv_obj_t* btn, const char* icon, const char* text, lv_color_t col);

// ---- change-only setters (LVGL invalidates on every set, even if equal) -----
void setText(lv_obj_t* l, const char* s);
void setTextColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part = LV_PART_MAIN);
void setBgColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part = LV_PART_MAIN);
void setArcColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part = LV_PART_INDICATOR);
void setBorderWidthIf(lv_obj_t* o, int32_t w);
void setArc(lv_obj_t* arc, float frac);
void setHidden(lv_obj_t* o, bool hidden);

// ---- misc ------------------------------------------------------------------
void haptic(HapticPattern p);
void ago(char* out, size_t n, time_t t);
const char* batterySymbol(int pct, bool charging);
lv_color_t batteryColor(int pct, bool charging);
extern const char* WD_S[7];
extern const char* WD_L[7];
extern const char* MO_S[12];
extern const char* MO_L[12];

}  // namespace ui
