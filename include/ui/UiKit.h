#pragma once
// ============================================================================
//  WatchOS Ultra UI kit: design tokens, typography and building blocks shared
//  by every page. All functions must be called on the UI task (LVGL thread).
//
//  Page anatomy (8 px grid):
//    title()            y = 22, accent underline y = 66
//    header pills       y = 24, height 30 (right aligned, -M)
//    content            from CONTENT_Y (80) downwards, margins M on both sides
//    captions           F.cap, letter spacing 2, cText2/cText3
// ============================================================================
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include "MotionHaptic.h"
#include "ui/Icons.h"

namespace ui {

// ---- geometry & calibrated safe-area ---------------------------------------
constexpr int SCR_W = 410;
constexpr int SCR_H = 502;
constexpr int BEZEL_R = 120;           // Silkscreened outer glass corner radius (px)
constexpr int BEZEL_INSET = 6;         // Physical border cut-off at straight edges (px)

constexpr int M = 22;                  // outer margin for cards / content rows
constexpr int CW = SCR_W - 2 * M;      // content width (366 px)
constexpr int M_HEAD = 46;             // safe margin for header row at y ~ 24..32 (clears R120)
constexpr int CW_HEAD = SCR_W - 2 * M_HEAD; // safe header width (318 px)
constexpr int M_BOT = 46;              // safe margin for bottom action bars at y ~ 420..470
constexpr int CW_BOT = SCR_W - 2 * M_BOT;   // safe bottom width (318 px)

constexpr int R_CARD = 24;             // card corner radius
constexpr int R_TILE = 16;             // smaller tiles / rows
constexpr int CONTENT_Y = 82;          // first content row below the title
constexpr int PILL_H = 28;
constexpr int BTN_H = 52;

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
inline lv_color_t cMesh()   { return lv_color_hex(0xBF5AF2); }   // LoRa / Meshtastic violet
lv_color_t accent();                   // user accent colour (settings)
lv_color_t colorFrom565(uint16_t c);   // RGB565 -> lv_color_t

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

// ---- reusable components ---------------------------------------------------
// Press feedback for any clickable object (dims while pressed, cheap: no layer).
void pressable(lv_obj_t* o);

// Status chip: tinted background (30 %) + caption in the same colour.
// Child 0 of the returned object is the label.
lv_obj_t* pill(lv_obj_t* parent, const char* text, lv_color_t color);
void setPill(lv_obj_t* pill, const char* text, lv_color_t color);     // change-only
lv_obj_t* pillLabel(lv_obj_t* pill);

// Centred empty state (icon, title, hint). Hidden/shown via setHidden().
lv_obj_t* emptyState(lv_obj_t* parent, const char* icon, const char* title, const char* hint);

// Small uppercase section caption. y < 0 = leave positioning to a flex parent.
lv_obj_t* sectionHeader(lv_obj_t* parent, int y, const char* text);

// Settings row with icon, label and a switch. The whole row is clickable; cb
// receives `user` as user data. y < 0 = flex positioned. Width CW.
lv_obj_t* toggleRow(lv_obj_t* parent, int y, const char* icon, const char* text, bool state,
                    lv_event_cb_t cb, void* user = nullptr);
void setToggleRow(lv_obj_t* row, bool state);

// Settings row with icon, label and a value on the right (tap cycles / opens).
lv_obj_t* valueRow(lv_obj_t* parent, int y, const char* icon, const char* text, const char* value,
                   lv_event_cb_t cb, void* user = nullptr);
void setValueRow(lv_obj_t* row, const char* value);

// Buttons. Child layout: [icon] text. buttonLabel() returns the text label.
lv_obj_t* primaryButton(lv_obj_t* parent, int w, int h, const char* icon, const char* text);
lv_obj_t* secondaryButton(lv_obj_t* parent, int w, int h, const char* icon, const char* text);
lv_obj_t* dangerButton(lv_obj_t* parent, int w, int h, const char* icon, const char* text);
lv_obj_t* iconButton(lv_obj_t* parent, int size, const char* icon, lv_color_t bg, lv_color_t fg);
lv_obj_t* buttonLabel(lv_obj_t* btn);
void setButtonText(lv_obj_t* btn, const char* text);
void setButtonEnabled(lv_obj_t* btn, bool enabled);

// Segmented control. cb gets the tab index as user data; the page calls
// setTabActive() itself (so it can veto / react).
lv_obj_t* tabBar(lv_obj_t* parent, int x, int y, int w, const char* const* names, int n, int active,
                 lv_event_cb_t cb);
void setTabActive(lv_obj_t* bar, int active);

// Modal confirm dialog on lv_layer_top(). Only one dialog at a time.
typedef void (*ConfirmCb)(bool ok, void* user);
void confirmDialog(const char* title, const char* msg, const char* okText, const char* cancelText,
                   ConfirmCb cb, void* user = nullptr, bool danger = false);
void closeDialog();
bool dialogOpen();

// ---- change-only setters (LVGL invalidates on every set, even if equal) -----
void setText(lv_obj_t* l, const char* s);
void setTextColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part = LV_PART_MAIN);
void setBgColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part = LV_PART_MAIN);
void setBgOpaIf(lv_obj_t* o, lv_opa_t opa, lv_part_t part = LV_PART_MAIN);
void setArcColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part = LV_PART_INDICATOR);
void setBorderColorIf(lv_obj_t* o, lv_color_t c);
void setBorderWidthIf(lv_obj_t* o, int32_t w);
void setArc(lv_obj_t* arc, float frac);
void setBar(lv_obj_t* bar, int32_t v);
void setHidden(lv_obj_t* o, bool hidden);

// ---- misc ------------------------------------------------------------------
void haptic(HapticPattern p);
void ago(char* out, size_t n, time_t t);                  // "gerade eben", "vor 5 min", "nie"
void agoMs(char* out, size_t n, uint32_t sinceMs);        // from millis() stamps (0 = nie)
const char* batterySymbol(int pct, bool charging);
lv_color_t batteryColor(int pct, bool charging);
int daysInMonth(int year, int mon0);                      // mon0 = 0..11, leap years handled

// English (legacy, still used by a few faces) and German calendar names
extern const char* WD_S[7];
extern const char* WD_L[7];
extern const char* MO_S[12];
extern const char* MO_L[12];
extern const char* WD_DE_S[7];     // "SO", "MO", ...
extern const char* WD_DE_L[7];     // "SONNTAG", ...
extern const char* MO_DE_S[12];    // "JAN", "FEB", "MÄR", ...
extern const char* MO_DE_L[12];    // "JANUAR", ...

}  // namespace ui
