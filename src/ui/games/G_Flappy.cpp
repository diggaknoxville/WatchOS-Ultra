// Flappy: tap to flap, fly through the gaps. Score = pipes passed.
#include "GameKit.h"
#include <math.h>

using namespace games;
using namespace ui;

namespace {

const int BX = AREA_X0 + 70, BW = 26, BH = 20;
const int NP = 3, PW = 46, GAPH = 124, SPACING = 170;
const int SKY0 = AREA_Y0 + 4, GROUND = AREA_Y1 - 6;

lv_obj_t* sBird;
lv_obj_t* sTop[NP];
lv_obj_t* sBot[NP];
lv_obj_t* sGround;
float by, vy;
float px[NP];
int gapY[NP];
bool passed[NP];
float speed;
uint32_t score;
bool flying, over;

void placePipe(int i) {
    int x = (int)lroundf(px[i]);
    int topH = gapY[i] - SKY0;
    lv_obj_set_height(sTop[i], topH < 1 ? 1 : topH);
    place(sTop[i], x, SKY0);
    int botY = gapY[i] + GAPH;
    lv_obj_set_height(sBot[i], GROUND - botY < 1 ? 1 : GROUND - botY);
    place(sBot[i], x, botY);
}

void newGap(int i) {
    gapY[i] = SKY0 + 30 + rnd(GROUND - SKY0 - GAPH - 60);
    passed[i] = false;
}

void build(lv_obj_t* area) {
    lv_color_t pc = lv_color_hex(0x30D158);
    for (int i = 0; i < NP; i++) {
        sTop[i] = rect(area, 0, SKY0, PW, 10, pc, 4);
        sBot[i] = rect(area, 0, GROUND, PW, 10, pc, 4);
    }
    sGround = rect(area, AREA_X0, GROUND, AREA_W, 3, cStroke());
    sBird = rect(area, BX, 0, BW, BH, lv_color_hex(0xFFD60A), 8);
}

void start() {
    by = (SKY0 + GROUND) / 2.0f;
    vy = 0;
    speed = 130.0f;
    score = 0;
    flying = false;
    over = false;
    for (int i = 0; i < NP; i++) {
        px[i] = AREA_X1 + 60 + i * SPACING;
        newGap(i);
        placePipe(i);
    }
    place(sBird, BX, (int)by - BH / 2);
    setScore(0);
}

void tick(uint32_t dtMs, const Input& in) {
    if (over) return;
    float dt = dtMs / 1000.0f;
    if (in.pressed) {
        flying = true;
        vy = -330.0f;
        rumble(HAPTIC_TICK);
    }
    if (!flying) return;
    vy += 950.0f * dt;
    by += vy * dt;
    if (by - BH / 2 < SKY0) { by = SKY0 + BH / 2; vy = 0; }
    place(sBird, BX, (int)lroundf(by - BH / 2));

    for (int i = 0; i < NP; i++) {
        px[i] -= speed * dt;
        if (px[i] + PW < AREA_X0) {
            px[i] += NP * SPACING;
            newGap(i);
        }
        placePipe(i);
        // scoring
        if (!passed[i] && px[i] + PW < BX) {
            passed[i] = true;
            score++;
            setScore(score);
            beep(880, 20);
            if (score % 5 == 0) speed += 12.0f;
        }
        // collision (AABB with a little forgiveness)
        float bl = BX + 3, br = BX + BW - 3, bt = by - BH / 2 + 2, bb = by + BH / 2 - 2;
        if (br > px[i] && bl < px[i] + PW && (bt < gapY[i] || bb > gapY[i] + GAPH)) {
            over = true;
            gameOver(score);
            return;
        }
    }
    if (by + BH / 2 >= GROUND) { over = true; gameOver(score); }
}

}  // namespace

namespace games {
const Game GAME_FLAPPY = {"FLAPPY", ICO_CLOUD, 0x64D2FF, "TIPPEN HÄLT DEN VOGEL IN DER LUFT",
                          false, build, start, tick, nullptr};
}
