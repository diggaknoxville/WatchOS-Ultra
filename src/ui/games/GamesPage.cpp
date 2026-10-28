// ============================================================================
//  Games: the folder page (3 x 3 grid in the app drawer) and the game host.
//  The host owns the HUD, the play area, the input sampling (tilt, shake,
//  touch, taps, swipes), the game-over overlay and the best scores in NVS.
// ============================================================================
#include "GameKit.h"
#include "ui/Pages.h"
#include "ui/Navigator.h"
#include "MotionHaptic.h"
#include "AudioEngine.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>

using namespace ui;

namespace {

using namespace games;

const Game* const GAMES[] = {
    &GAME_LABYRINTH, &GAME_BREAKOUT, &GAME_DICE,
    &GAME_BALANCE,   &GAME_SNAKE,    &GAME_2048,
    &GAME_REACTION,  &GAME_MEMORY,   &GAME_FLAPPY,
};
const int N_GAMES = sizeof(GAMES) / sizeof(GAMES[0]);

// ---- best scores (NVS "kronos_games", keys b0..b8) ---------------------------
uint32_t sBest[N_GAMES];
Preferences sPrefs;
bool sPrefsOk = false;

void loadBest() {
    sPrefsOk = sPrefs.begin("kronos_games", false);
    for (int i = 0; i < N_GAMES; i++) {
        char k[16];
        snprintf(k, sizeof(k), "b%d", i);
        sBest[i] = sPrefsOk ? sPrefs.getUInt(k, 0) : 0;
    }
}

void storeBest(int i) {
    if (!sPrefsOk) return;
    char k[16];
    snprintf(k, sizeof(k), "b%d", i);
    sPrefs.putUInt(k, sBest[i]);
}

void fmtScore(int i, uint32_t v, char* out, size_t n) {
    const Game& g = *GAMES[i];
    if (g.fmtScore) g.fmtScore(v, out, n);
    else if (g.lowerIsBetter && v >= 1000) snprintf(out, n, "%lu,%lu s", (unsigned long)(v / 1000), (unsigned long)((v % 1000) / 100));
    else if (g.lowerIsBetter) snprintf(out, n, "%lu ms", (unsigned long)v);
    else snprintf(out, n, "%lu", (unsigned long)v);
}

void bestText(int i, char* out, size_t n) {
    if (sBest[i] == 0) { snprintf(out, n, "BESTE  --"); return; }
    char v[24];
    fmtScore(i, sBest[i], v, sizeof(v));
    snprintf(out, n, "BESTE  %s", v);
}

// ---- host state ----------------------------------------------------------------
lv_obj_t* sHostRoot = nullptr;
lv_obj_t* sArea = nullptr;
lv_obj_t* sHudName = nullptr;
lv_obj_t* sHudScore = nullptr;
lv_obj_t* sHudStatus = nullptr;
lv_obj_t* sHudHint = nullptr;
lv_obj_t* sOver = nullptr;             // game-over card
lv_obj_t* sOverTitle = nullptr;
lv_obj_t* sOverScore = nullptr;
lv_obj_t* sOverBest = nullptr;
lv_obj_t* sTileBest[N_GAMES];

int sCur = -1;                          // game in the host (-1 none)
bool sRunning = false;
uint32_t sLastTick = 0;
uint32_t sShownAt = 0;

// input accumulation between frames (LVGL events -> tick)
Input sIn;
bool sTouching = false;
int16_t sTouchX = 0, sTouchY = 0;
bool sPendPressed = false, sPendTapped = false;
Swipe sPendSwipe = SW_NONE;
int16_t sTapX = 0, sTapY = 0;
float sZeroX = 0, sZeroY = 0;           // neutral tilt captured at start()
float sPrevAx = 0, sPrevAy = 0, sPrevAz = 1;
float sShake = 0;

void clearClickable(lv_obj_t* o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) clearClickable(lv_obj_get_child(o, i));
}

void captureNeutralTilt() {
    float ax, ay, az;
    MotionHaptic::getAccel(ax, ay, az);
    sZeroX = ax;
    sZeroY = ay;
    sPrevAx = ax; sPrevAy = ay; sPrevAz = az;
    sShake = 0;
}

void startCurrent() {
    if (sCur < 0) return;
    const Game& g = *GAMES[sCur];
    setHidden(sOver, true);
    setText(sHudScore, "0");
    setText(sHudStatus, "");
    setText(sHudHint, g.hint);
    setHidden(sHudHint, false);
    captureNeutralTilt();
    sPendPressed = sPendTapped = false;
    sPendSwipe = SW_NONE;
    sRunning = true;
    sLastTick = millis();
    sShownAt = sLastTick;
    g.start();
}

void sampleInput(uint32_t dt) {
    float ax, ay, az;
    MotionHaptic::getAccel(ax, ay, az);
    float tx = TILT_SIGN_X * (ax - sZeroX) / TILT_FULL;
    float ty = TILT_SIGN_Y * (ay - sZeroY) / TILT_FULL;
    sIn.tx = tx < -1 ? -1 : (tx > 1 ? 1 : tx);
    sIn.ty = ty < -1 ? -1 : (ty > 1 ? 1 : ty);
    // orientation change rate as a shake measure (unit vector delta per 100 ms)
    float d = fabsf(ax - sPrevAx) + fabsf(ay - sPrevAy) + fabsf(az - sPrevAz);
    sPrevAx = ax; sPrevAy = ay; sPrevAz = az;
    float rate = dt ? d * 100.0f / (float)dt : 0;
    sShake = sShake * 0.6f + rate * 0.4f;
    sIn.shake = sShake;

    sIn.touching = sTouching;
    sIn.x = sTouchX;
    sIn.y = sTouchY;
    sIn.pressed = sPendPressed;
    sIn.tapped = sPendTapped;
    if (sPendTapped) { sIn.x = sTapX; sIn.y = sTapY; }
    sIn.swipe = sPendSwipe;
    sPendPressed = sPendTapped = false;
    sPendSwipe = SW_NONE;
}

void hostRefresh(bool) {
    if (sCur < 0 || !sRunning) return;
    uint32_t now = millis();
    uint32_t dt = now - sLastTick;
    sLastTick = now;
    if (dt > 50) dt = 50;                                  // after a stall: no teleporting
    if (now - sShownAt > 3500 && !lv_obj_has_flag(sHudHint, LV_OBJ_FLAG_HIDDEN)) setHidden(sHudHint, true);
    sampleInput(dt);
    GAMES[sCur]->tick(dt, sIn);
}

void onHostTouch(lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t* in = lv_indev_active();
    lv_point_t p = {0, 0};
    if (in) lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) {
        sPendPressed = true;
        sTouching = true;
        sTouchX = (int16_t)p.x;
        sTouchY = (int16_t)p.y;
    } else if (c == LV_EVENT_PRESSING) {
        sTouchX = (int16_t)p.x;
        sTouchY = (int16_t)p.y;
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        sTouching = false;
    } else if (c == LV_EVENT_SHORT_CLICKED) {
        sPendTapped = true;
        sTapX = (int16_t)p.x;
        sTapY = (int16_t)p.y;
    } else if (c == LV_EVENT_GESTURE && in) {
        switch (lv_indev_get_gesture_dir(in)) {
            case LV_DIR_TOP:    sPendSwipe = SW_UP; break;
            case LV_DIR_RIGHT:  sPendSwipe = SW_RIGHT; break;
            case LV_DIR_BOTTOM: sPendSwipe = SW_DOWN; break;
            case LV_DIR_LEFT:   sPendSwipe = SW_LEFT; break;
            default: break;
        }
    }
}

void launch(int i) {
    if (i < 0 || i >= N_GAMES) return;
    const Game& g = *GAMES[i];
    sRunning = false;
    lv_obj_clean(sArea);
    sCur = i;
    setText(sHudName, g.name);
    lv_obj_set_style_text_color(sHudName, lv_color_hex(g.color), 0);
    g.build(sArea);
    clearClickable(sArea);
    lv_obj_move_foreground(sOver);
    char b[32];
    bestText(i, b, sizeof(b));
    setText(sOverBest, b);
    setHidden(sOver, true);
    nav::markDirty(nav::PG_APP_GAME);
    nav::openApp(nav::PG_APP_GAME);
}

void buildHost(lv_obj_t* scr) {
    sHostRoot = pageRoot(scr);
    lv_obj_add_flag(sHostRoot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sHostRoot, onHostTouch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sHostRoot, onHostTouch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(sHostRoot, onHostTouch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(sHostRoot, onHostTouch, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(sHostRoot, onHostTouch, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(sHostRoot, onHostTouch, LV_EVENT_GESTURE, NULL);

    sArea = box(sHostRoot, SCR_W, SCR_H);
    lv_obj_set_pos(sArea, 0, 0);

    // HUD
    sHudName = label(sHostRoot, F.cap, accent(), "");
    lv_obj_set_style_text_letter_space(sHudName, 2, 0);
    lv_obj_set_pos(sHudName, M_HEAD, 26);
    sHudStatus = label(sHostRoot, F.cap, cText2(), "");
    lv_obj_set_style_text_letter_space(sHudStatus, 1, 0);
    lv_obj_set_pos(sHudStatus, M_HEAD, 46);
    sHudScore = label(sHostRoot, F.numS, cText(), "0");
    lv_obj_set_style_text_align(sHudScore, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(sHudScore, 140);
    lv_obj_set_pos(sHudScore, SCR_W - M_HEAD - 140, 20);
    sHudHint = caption(sHostRoot, "", cText3());
    lv_obj_set_width(sHudHint, CW_BOT);
    lv_obj_set_style_text_align(sHudHint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sHudHint, LV_ALIGN_BOTTOM_MID, 0, -40);

    // game-over card
    sOver = card(sHostRoot, 300, 236);
    lv_obj_align(sOver, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(sOver, cSurf2(), 0);
    sOverTitle = label(sOver, F.title, cText(), "SPIEL VORBEI");
    lv_obj_set_style_text_letter_space(sOverTitle, 2, 0);
    lv_obj_align(sOverTitle, LV_ALIGN_TOP_MID, 0, 22);
    sOverScore = label(sOver, F.numL, accent(), "0");
    accReg(sOverScore, ACC_TEXT);
    lv_obj_align(sOverScore, LV_ALIGN_TOP_MID, 0, 62);
    sOverBest = caption(sOver, "", cText2());
    lv_obj_align(sOverBest, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_t* again = primaryButton(sOver, 220, BTN_H, ICO_REFRESH, "Nochmal");
    lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_event_cb(again, [](lv_event_t*) { haptic(HAPTIC_TICK); startCurrent(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(sOver, LV_OBJ_FLAG_HIDDEN);

    nav::PageDef d;
    d.root = sHostRoot;
    d.refresh = hostRefresh;
    d.refreshMs = 0;              // every frame (the navigator governs to ~30 fps)
    d.ttlMs = 500;
    d.onShow = [] {
        MotionHaptic::setGameMode(true);
        PowerManager::handleActivity();
        startCurrent();
    };
    d.onHide = [] {
        sRunning = false;
        MotionHaptic::setGameMode(false);
        AudioEngine::stop();
    };
    nav::registerPage(nav::PG_APP_GAME, d);
}

void refreshFolder(bool) {
    char b[24];
    for (int i = 0; i < N_GAMES; i++) {
        if (sBest[i]) fmtScore(i, sBest[i], b, sizeof(b)); else snprintf(b, sizeof(b), "--");
        setText(sTileBest[i], b);
    }
}

void buildFolder(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "SPIELE");
    const int tw = 104, th = 108, gap = 10;
    const int x0 = (SCR_W - (3 * tw + 2 * gap)) / 2;
    const int y0 = 86;
    for (int i = 0; i < N_GAMES; i++) {
        const Game& g = *GAMES[i];
        lv_obj_t* t = box(p, tw, th);
        lv_obj_set_pos(t, x0 + (i % 3) * (tw + gap), y0 + (i / 3) * (th + gap));
        pressable(t);
        lv_obj_add_event_cb(t, [](lv_event_t* e) {
            haptic(HAPTIC_TICK);
            launch((int)(intptr_t)lv_event_get_user_data(e));
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* d = box(t, 64, 64);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(d, 18, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(g.color), 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(0x3B4A66), 0);
        lv_obj_t* ic = label(d, F.icoM, lv_color_white(), g.icon);
        lv_obj_center(ic);
        lv_obj_t* nm = label(t, F.cap, cText(), g.name);
        lv_obj_set_style_text_letter_space(nm, 1, 0);
        lv_obj_set_width(nm, tw);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 70);
        sTileBest[i] = label(t, F.cap, cText3(), "--");
        lv_obj_set_width(sTileBest[i], tw);
        lv_obj_set_style_text_align(sTileBest[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(sTileBest[i], LV_ALIGN_TOP_MID, 0, 90);
    }
    refreshFolder(true);

    nav::PageDef d;
    d.root = p;
    d.refresh = refreshFolder;
    d.refreshMs = 0xFFFF;
    d.ttlMs = 0;
    d.onShow = [] { refreshFolder(true); };
    nav::registerPage(nav::PG_APP_GAMES, d);
}

}  // namespace

// ---------------------------------------------------------------- services
namespace games {

void setScore(uint32_t score) {
    char b[16];
    snprintf(b, sizeof(b), "%lu", (unsigned long)score);
    setText(sHudScore, b);
}

void setStatus(const char* t) { setText(sHudStatus, t ? t : ""); }

void gameOver(uint32_t score, bool won) {
    if (sCur < 0) return;
    sRunning = false;
    const Game& g = *GAMES[sCur];
    bool record = false;
    if (score > 0 && (sBest[sCur] == 0 || (g.lowerIsBetter ? score < sBest[sCur] : score > sBest[sCur]))) {
        sBest[sCur] = score;
        storeBest(sCur);
        record = true;
        nav::markDirty(nav::PG_APP_GAMES);
    }
    char b[32];
    fmtScore(sCur, score, b, sizeof(b));
    setText(sOverScore, b);
    setText(sOverTitle, record ? "NEUER REKORD" : (won ? "GESCHAFFT" : "SPIEL VORBEI"));
    bestText(sCur, b, sizeof(b));
    setText(sOverBest, b);
    setHidden(sHudHint, true);
    lv_obj_move_foreground(sOver);
    setHidden(sOver, false);
    rumble(record ? HAPTIC_SUCCESS : (won ? HAPTIC_DOUBLE_CLICK : HAPTIC_ALERT_SHORT));
    if (record) { beep(880, 80); beep(1175, 120); }
    else if (!won) beep(220, 160);
}

void beep(uint16_t f, uint16_t ms) { AudioEngine::playTone(f, ms); }
void rumble(HapticPattern p) { haptic(p); }
uint32_t rnd(uint32_t n) { return n ? esp_random() % n : 0; }

lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color, int radius) {
    lv_obj_t* o = box(parent, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* disc(lv_obj_t* parent, int cx, int cy, int d, lv_color_t color) {
    return rect(parent, cx - d / 2, cy - d / 2, d, d, color, LV_RADIUS_CIRCLE);
}

lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* s) {
    lv_obj_t* l = label(parent, font, color, s);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void place(lv_obj_t* o, int x, int y) {
    if (lv_obj_get_x(o) != x || lv_obj_get_y(o) != y) lv_obj_set_pos(o, x, y);
}

void tint(lv_obj_t* o, lv_color_t c) { setBgColorIf(o, c); }

}  // namespace games

namespace pages {
void buildGames(lv_obj_t* scr) {
    loadBest();
    buildHost(scr);
    buildFolder(scr);
}
}
