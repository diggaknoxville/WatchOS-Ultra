// Breakout: tilt left / right to move the paddle, clear the bricks.
#include "GameKit.h"
#include <math.h>

using namespace games;
using namespace ui;

namespace {

const int BRK_COLS = 6, BRK_ROWS = 5, BRK_W = 50, BRK_H = 16, BRK_GAP = 4;
const int OX = AREA_X0 + (AREA_W - (BRK_COLS * (BRK_W + BRK_GAP) - BRK_GAP)) / 2;
const int OY = 92;
const int PW = 72, PH = 10, PY = AREA_Y1 - 22;
const int BALL = 12;
const uint32_t ROWCOL[BRK_ROWS] = {0xFF2A6D, 0xFF7700, 0xFFEA00, 0x05FFA1, 0x00F0FF};

lv_obj_t* sBrick[BRK_ROWS][BRK_COLS];
lv_obj_t* sPaddle;
lv_obj_t* sBall;
lv_obj_t* sBallGlint;
bool alive[BRK_ROWS][BRK_COLS];
int left;
float px, bx, by, vx, vy, speed;
int lives, level;
uint32_t score;
bool serving, over;

void status() {
    char b[32];
    snprintf(b, sizeof(b), "LEBEN %d  •  LEVEL %d", lives, level);
    setStatus(b);
}

void resetBricks() {
    left = 0;
    for (int r = 0; r < BRK_ROWS; r++)
        for (int c = 0; c < BRK_COLS; c++) { alive[r][c] = true; setHidden(sBrick[r][c], false); left++; }
}

void serve() {
    bx = px;
    by = PY - BALL;
    float a = (-60.0f + rnd(120)) * (float)M_PI / 180.0f;
    vx = sinf(a) * speed;
    vy = -cosf(a) * speed;
    serving = true;
}

void build(lv_obj_t* area) {
    // Cyberpunk style boundary walls with neon glow
    rect(area, AREA_X0 - 4, AREA_Y0 + 2, AREA_W + 8, 3, lv_color_hex(0x00F0FF), 1);
    lv_obj_set_style_shadow_width(area, 6, 0);
    lv_obj_set_style_shadow_color(area, lv_color_hex(0x00A0FF), 0);
    rect(area, AREA_X0 - 4, AREA_Y0 + 2, 2, AREA_H, lv_color_hex(0x1B2C46));
    rect(area, AREA_X1 + 2, AREA_Y0 + 2, 2, AREA_H, lv_color_hex(0x1B2C46));

    for (int r = 0; r < BRK_ROWS; r++) {
        for (int c = 0; c < BRK_COLS; c++) {
            sBrick[r][c] = rect(area, OX + c * (BRK_W + BRK_GAP), OY + r * (BRK_H + BRK_GAP), BRK_W, BRK_H, lv_color_hex(ROWCOL[r]), 5);
            lv_obj_set_style_shadow_width(sBrick[r][c], 4, 0);
            lv_obj_set_style_shadow_color(sBrick[r][c], lv_color_hex(ROWCOL[r]), 0);
            lv_obj_set_style_border_width(sBrick[r][c], 1, 0);
            lv_obj_set_style_border_color(sBrick[r][c], lv_color_white(), 0);
            lv_obj_set_style_border_opa(sBrick[r][c], LV_OPA_40, 0);
        }
    }

    // High-tech rounded paddle with cyan glow
    sPaddle = rect(area, 0, PY, PW, PH, lv_color_hex(0x00E5FF), 5);
    lv_obj_set_style_shadow_width(sPaddle, 8, 0);
    lv_obj_set_style_shadow_color(sPaddle, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(sPaddle, 1, 0);
    lv_obj_set_style_border_color(sPaddle, lv_color_white(), 0);

    // Glowing metallic ball with specular glint
    sBall = disc(area, 0, 0, BALL, lv_color_hex(0xFFD60A));
    lv_obj_set_style_shadow_width(sBall, 6, 0);
    lv_obj_set_style_shadow_color(sBall, lv_color_hex(0xFFD60A), 0);
    sBallGlint = disc(sBall, 2, 2, 4, lv_color_white());
    lv_obj_set_style_bg_opa(sBallGlint, LV_OPA_90, 0);
}

void start() {
    lives = 3;
    level = 1;
    score = 0;
    speed = 230.0f;
    over = false;
    px = (AREA_X0 + AREA_X1) / 2.0f;
    resetBricks();
    serve();
    status();
    setScore(0);
}

void tick(uint32_t dtMs, const Input& in) {
    if (over) return;
    float dt = dtMs / 1000.0f;
    px += in.tx * 480.0f * dt;
    if (px < AREA_X0 + PW / 2) px = AREA_X0 + PW / 2;
    if (px > AREA_X1 - PW / 2) px = AREA_X1 - PW / 2;
    place(sPaddle, (int)(px - PW / 2), PY);

    if (serving) {
        bx = px;
        by = PY - BALL;
        if (in.tapped || in.pressed) serving = false;
        place(sBall, (int)(bx - BALL / 2), (int)(by - BALL / 2));
        return;
    }

    bx += vx * dt;
    by += vy * dt;
    const float r = BALL / 2.0f;
    if (bx - r < AREA_X0) { bx = AREA_X0 + r; vx = -vx; rumble(HAPTIC_TICK); }
    if (bx + r > AREA_X1) { bx = AREA_X1 - r; vx = -vx; rumble(HAPTIC_TICK); }
    if (by - r < AREA_Y0 + 8) { by = AREA_Y0 + 8 + r; vy = -vy; rumble(HAPTIC_TICK); }

    // paddle
    if (vy > 0 && by + r >= PY && by + r <= PY + PH + 6 && bx >= px - PW / 2 - r && bx <= px + PW / 2 + r) {
        float hit = (bx - px) / (PW / 2.0f);                  // -1 .. 1
        float a = hit * 60.0f * (float)M_PI / 180.0f;
        vx = sinf(a) * speed;
        vy = -cosf(a) * speed;
        by = PY - r;
        rumble(HAPTIC_TICK);
    }

    // bricks
    int c = (int)floorf((bx - OX) / (BRK_W + BRK_GAP)), rr = (int)floorf((by - OY) / (BRK_H + BRK_GAP));
    if (c >= 0 && c < BRK_COLS && rr >= 0 && rr < BRK_ROWS && alive[rr][c]) {
        float lx = bx - (OX + c * (BRK_W + BRK_GAP)), ly = by - (OY + rr * (BRK_H + BRK_GAP));
        if (lx >= -r && lx <= BRK_W + r && ly >= -r && ly <= BRK_H + r) {
            alive[rr][c] = false;
            setHidden(sBrick[rr][c], true);
            left--;
            score += 10;
            setScore(score);
            beep(660 + rr * 80, 25);
            // bounce on the side that was hit
            float ox = fminf(lx, BRK_W - lx), oy = fminf(ly, BRK_H - ly);
            if (ox < oy) vx = -vx; else vy = -vy;
            if (left == 0) {
                level++;
                score += 50;
                speed *= 1.15f;
                resetBricks();
                serve();
                status();
                rumble(HAPTIC_SUCCESS);
            }
        }
    }

    if (by - r > AREA_Y1) {
        lives--;
        status();
        rumble(HAPTIC_ALERT_SHORT);
        if (lives <= 0) { over = true; gameOver(score); return; }
        serve();
    }
    place(sBall, (int)lroundf(bx - r), (int)lroundf(by - r));
}

}  // namespace

namespace games {
const Game GAME_BREAKOUT = {"BREAKOUT", ICO_WIDGETS, 0x0A84FF, "NEIGEN BEWEGT DEN SCHLÄGER • TIPPEN STARTET",
                            false, build, start, tick, nullptr};
}
