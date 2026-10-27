// Breakout: tilt left / right to move the paddle, clear the bricks.
#include "GameKit.h"
#include <math.h>

using namespace games;
using namespace ui;

namespace {

const int NCOL = 6, NROW = 5, BRW = 50, BRH = 16, GAP = 4;
const int OX = AREA_X0 + (AREA_W - (NCOL * (BRW + GAP) - GAP)) / 2;
const int OY = 92;
const int PW = 72, PH = 10, PY = AREA_Y1 - 22;
const int BALL = 12;
const uint32_t ROWCOL[NROW] = {0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x64D2FF};

lv_obj_t* sBrick[NROW][NCOL];
lv_obj_t* sPaddle;
lv_obj_t* sBall;
bool alive[NROW][NCOL];
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
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) { alive[r][c] = true; setHidden(sBrick[r][c], false); left++; }
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
    rect(area, AREA_X0, AREA_Y0 + 6, AREA_W, 2, cStroke());
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++)
            sBrick[r][c] = rect(area, OX + c * (BRW + GAP), OY + r * (BRH + GAP), BRW, BRH, lv_color_hex(ROWCOL[r]), 4);
    sPaddle = rect(area, 0, PY, PW, PH, cText(), 5);
    sBall = disc(area, 0, 0, BALL, accent());
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
    if (bx - r < AREA_X0) { bx = AREA_X0 + r; vx = -vx; }
    if (bx + r > AREA_X1) { bx = AREA_X1 - r; vx = -vx; }
    if (by - r < AREA_Y0 + 8) { by = AREA_Y0 + 8 + r; vy = -vy; }

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
    int c = (int)floorf((bx - OX) / (BRW + GAP)), rr = (int)floorf((by - OY) / (BRH + GAP));
    if (c >= 0 && c < NCOL && rr >= 0 && rr < NROW && alive[rr][c]) {
        float lx = bx - (OX + c * (BRW + GAP)), ly = by - (OY + rr * (BRH + GAP));
        if (lx >= -r && lx <= BRW + r && ly >= -r && ly <= BRH + r) {
            alive[rr][c] = false;
            setHidden(sBrick[rr][c], true);
            left--;
            score += 10;
            setScore(score);
            beep(660 + rr * 80, 25);
            // bounce on the side that was hit
            float ox = fminf(lx, BRW - lx), oy = fminf(ly, BRH - ly);
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
