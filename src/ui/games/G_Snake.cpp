// Snake: swipe (or tap beside the head) to steer. Score = food eaten.
#include "GameKit.h"
#include <stdlib.h>

using namespace games;
using namespace ui;

namespace {

const int COLS = 16, ROWS = 18, CS = 20;
const int OX = AREA_X0 + (AREA_W - COLS * CS) / 2;
const int OY = AREA_Y0 + (AREA_H - ROWS * CS) / 2;
const int MAXLEN = 96;

struct P { int8_t x, y; };
P body[MAXLEN];
int len;
int dx, dy, ndx, ndy;
P food;
lv_obj_t* seg[MAXLEN];
lv_obj_t* sFood;
uint32_t acc, stepMs;
uint32_t score;
bool over;

void draw() {
    for (int i = 0; i < MAXLEN; i++) {
        if (i < len) {
            place(seg[i], OX + body[i].x * CS + 1, OY + body[i].y * CS + 1);
            setHidden(seg[i], false);
        } else {
            setHidden(seg[i], true);
        }
    }
    place(sFood, OX + food.x * CS + 3, OY + food.y * CS + 3);
}

void placeFood() {
    for (int tries = 0; tries < 200; tries++) {
        P f = {(int8_t)rnd(COLS), (int8_t)rnd(ROWS)};
        bool hit = false;
        for (int i = 0; i < len; i++) if (body[i].x == f.x && body[i].y == f.y) { hit = true; break; }
        if (!hit) { food = f; return; }
    }
}

void build(lv_obj_t* area) {
    rect(area, OX - 2, OY - 2, COLS * CS + 4, ROWS * CS + 4, lv_color_hex(0x0B0E14), 6);
    for (int i = 0; i < MAXLEN; i++) {
        seg[i] = rect(area, 0, 0, CS - 2, CS - 2, i == 0 ? cText() : accent(), 5);
    }
    sFood = rect(area, 0, 0, CS - 6, CS - 6, cCrit(), LV_RADIUS_CIRCLE);
}

void start() {
    len = 3;
    for (int i = 0; i < len; i++) body[i] = {(int8_t)(COLS / 2 - i), (int8_t)(ROWS / 2)};
    dx = ndx = 1; dy = ndy = 0;
    acc = 0;
    stepMs = 210;
    score = 0;
    over = false;
    placeFood();
    draw();
    setScore(0);
}

void steer(int x, int y) {
    if (x == -dx && y == -dy) return;             // no U-turn into the neck
    ndx = x; ndy = y;
}

void tick(uint32_t dtMs, const Input& in) {
    if (over) return;
    switch (in.swipe) {
        case SW_UP: steer(0, -1); break;
        case SW_DOWN: steer(0, 1); break;
        case SW_LEFT: steer(-1, 0); break;
        case SW_RIGHT: steer(1, 0); break;
        default: break;
    }
    if (in.tapped) {
        // tap beside the head: turn towards the finger
        int hx = OX + body[0].x * CS + CS / 2, hy = OY + body[0].y * CS + CS / 2;
        int ex = in.x - hx, ey = in.y - hy;
        if (abs(ex) > abs(ey)) steer(ex > 0 ? 1 : -1, 0);
        else steer(0, ey > 0 ? 1 : -1);
    }
    acc += dtMs;
    if (acc < stepMs) return;
    acc -= stepMs;
    dx = ndx; dy = ndy;
    P nh = {(int8_t)(body[0].x + dx), (int8_t)(body[0].y + dy)};
    if (nh.x < 0 || nh.x >= COLS || nh.y < 0 || nh.y >= ROWS) { over = true; gameOver(score); return; }
    bool grow = (nh.x == food.x && nh.y == food.y);
    int tail = grow ? len : len - 1;
    for (int i = 0; i < tail; i++)
        if (body[i].x == nh.x && body[i].y == nh.y) { over = true; gameOver(score); return; }
    if (grow && len < MAXLEN) len++;
    for (int i = len - 1; i > 0; i--) body[i] = body[i - 1];
    body[0] = nh;
    if (grow) {
        score++;
        setScore(score);
        if (stepMs > 90) stepMs -= 5;
        rumble(HAPTIC_TICK);
        beep(740, 20);
        placeFood();
    }
    draw();
}

}  // namespace

namespace games {
const Game GAME_SNAKE = {"SNAKE", ICO_ROUTE, 0x30D158, "WISCHEN ODER NEBEN DEN KOPF TIPPEN LENKT",
                         false, build, start, tick, nullptr};
}
