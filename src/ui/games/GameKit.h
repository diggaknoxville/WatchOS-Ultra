#pragma once
// ============================================================================
//  Game kit: the contract between the game host (GamesPage.cpp) and the nine
//  games. A game builds its widgets into the play area, gets start() when it
//  becomes visible or is restarted, and tick() every frame (~30 fps) with the
//  elapsed time and the current input (tilt, shake, touch, taps, swipes).
//
//  Play area: the whole screen minus the HUD rows. Keep interactive content
//  inside [AREA_X0, AREA_X1) x [AREA_Y0, AREA_Y1): the navigator owns the left
//  edge (back), the bottom edge (home) and the top 72 px (control centre).
//
//  Every object a game creates must be non-clickable (the host strips the flag
//  after build()); touch input reaches the game through Input only.
// ============================================================================
#include <lvgl.h>
#include <stdint.h>
#include "ui/UiKit.h"

namespace games {

constexpr int AREA_X0 = 40;
constexpr int AREA_Y0 = 72;
constexpr int AREA_X1 = 370;
constexpr int AREA_Y1 = 440;
constexpr int AREA_W = AREA_X1 - AREA_X0;    // 330
constexpr int AREA_H = AREA_Y1 - AREA_Y0;    // 368

// Tilt axis mapping (orientation from the BHI260 game rotation vector). The
// signs make "tilt the watch towards the right / towards you" move things
// right / down; flip them here if the hardware mounts the sensor mirrored.
constexpr float TILT_SIGN_X = 1.0f;
constexpr float TILT_SIGN_Y = 1.0f;
constexpr float TILT_FULL = 0.34f;           // sin(20 deg): full deflection

enum Swipe : uint8_t { SW_NONE = 0, SW_UP, SW_RIGHT, SW_DOWN, SW_LEFT };

struct Input {
    float tx, ty;          // tilt relative to the neutral pose captured at start(), -1..1
    float shake;           // orientation change rate, ~0 at rest, > 0.5 when shaken
    bool touching;         // finger on the glass
    int16_t x, y;          // last finger position
    bool pressed;          // finger went down this frame
    bool tapped;           // short tap released this frame (at x, y)
    Swipe swipe;           // swipe completed this frame
};

struct Game {
    const char* name;      // "LABYRINTH"
    const char* icon;      // ICO_*
    uint32_t color;        // tile colour
    const char* hint;      // one line shown under the HUD at start
    bool lowerIsBetter;    // best score = minimum (times) instead of maximum
    void (*build)(lv_obj_t* area);
    void (*start)();
    void (*tick)(uint32_t dtMs, const Input& in);
    // optional score formatter ("12,3 s", "14 Züge"); nullptr = integer, or
    // milliseconds when lowerIsBetter
    void (*fmtScore)(uint32_t v, char* out, size_t n);
};

// ---- host services for games --------------------------------------------
void setScore(uint32_t score);                 // HUD, right
void setStatus(const char* text);              // HUD, left of the score (lives, round ...)
void gameOver(uint32_t score, bool won = false);   // overlay with best + "Nochmal"
void beep(uint16_t freqHz, uint16_t ms);       // honours the sound setting
void rumble(HapticPattern p);
uint32_t rnd(uint32_t n);                      // 0 .. n-1

// ---- small widget helpers (non-clickable, no padding) ---------------------
lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color, int radius = 0);
lv_obj_t* disc(lv_obj_t* parent, int cx, int cy, int d, lv_color_t color);
lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* s);
void place(lv_obj_t* o, int x, int y);         // change-only lv_obj_set_pos
void tint(lv_obj_t* o, lv_color_t c);          // change-only background colour

}  // namespace games
