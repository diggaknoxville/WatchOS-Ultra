// Balance: hold the watch level and keep the ball inside the shrinking ring.
// Score = tenths of a second survived.
#include "GameKit.h"
#include <math.h>

using namespace games;
using namespace ui;

namespace {

const int CX = SCR_W / 2, CY = (AREA_Y0 + AREA_Y1) / 2;
const int BALL = 22;
const float R_START = 118.0f, R_MIN = 30.0f;

lv_obj_t* sRing;
lv_obj_t* sRingIn;
lv_obj_t* sBall;
lv_obj_t* sTime;
float bx, by, vx, vy, ringR;
uint32_t elapsed;
bool over;
int lastShown;

void shapeRing(float r) {
    int d = (int)(2 * r);
    if (lv_obj_get_width(sRing) != d) {
        lv_obj_set_size(sRing, d, d);
        lv_obj_set_pos(sRing, CX - d / 2, CY - d / 2);
        int di = d - 8;
        lv_obj_set_size(sRingIn, di, di);
        lv_obj_set_pos(sRingIn, CX - di / 2, CY - di / 2);
    }
}

void build(lv_obj_t* area) {
    disc(area, CX, CY, 240, lv_color_hex(0x161E2E));
    disc(area, CX, CY, 236, lv_color_hex(0x0A0D14));
    disc(area, CX, CY, 160, lv_color_hex(0x1C273B));
    disc(area, CX, CY, 156, lv_color_hex(0x0A0D14));
    disc(area, CX, CY, 80, lv_color_hex(0x223049));
    disc(area, CX, CY, 76, lv_color_hex(0x0A0D14));

    sRing = disc(area, CX, CY, (int)(2 * R_START), accent());
    sRingIn = disc(area, CX, CY, (int)(2 * R_START) - 8, cBg());
    rect(area, CX - 1, CY - 22, 2, 44, lv_color_hex(0x00E5FF));
    rect(area, CX - 22, CY - 1, 44, 2, lv_color_hex(0x00E5FF));
    sBall = disc(area, CX, CY, BALL, lv_color_hex(0xFFFFFF));
    sTime = text(area, F.numS, cText2(), "0,0 s");
    lv_obj_align(sTime, LV_ALIGN_TOP_MID, 0, AREA_Y1 - 2);
}

void start() {
    bx = CX; by = CY; vx = vy = 0;
    ringR = R_START;
    elapsed = 0;
    over = false;
    lastShown = -1;
    shapeRing(ringR);
    tint(sRing, accent());
    place(sBall, CX - BALL / 2, CY - BALL / 2);
    setText(sTime, "0,0 s");
}

void tick(uint32_t dtMs, const Input& in) {
    if (over) return;
    float dt = dtMs / 1000.0f;
    elapsed += dtMs;
    // the ring shrinks slowly, faster after 20 s
    float shrink = elapsed < 20000 ? 2.2f : 4.0f;
    ringR -= shrink * dt;
    if (ringR < R_MIN) ringR = R_MIN;
    shapeRing(ringR);

    vx += in.tx * 700.0f * dt;
    vy += in.ty * 700.0f * dt;
    vx *= powf(0.985f, dt * 60.0f);
    vy *= powf(0.985f, dt * 60.0f);
    bx += vx * dt;
    by += vy * dt;
    place(sBall, (int)lroundf(bx - BALL / 2), (int)lroundf(by - BALL / 2));

    float dist = sqrtf((bx - CX) * (bx - CX) + (by - CY) * (by - CY));
    float warn = dist / (ringR - BALL / 2);
    tint(sRing, warn > 0.8f ? cCrit() : (warn > 0.55f ? cWarn() : accent()));

    int tenths = elapsed / 100;
    if (tenths != lastShown) {
        lastShown = tenths;
        char b[16];
        snprintf(b, sizeof(b), "%d,%d s", tenths / 10, tenths % 10);
        setText(sTime, b);
        setScore(tenths / 10);
    }
    if (dist > ringR - BALL / 2) {
        over = true;
        gameOver(tenths);
    }
}

void fmt(uint32_t v, char* out, size_t n) { snprintf(out, n, "%lu,%lu s", (unsigned long)(v / 10), (unsigned long)(v % 10)); }

}  // namespace

namespace games {
const Game GAME_BALANCE = {"BALANCE", ICO_MY_LOCATION, 0x30D158, "WAAGRECHT HALTEN: KUGEL IM RING BEHALTEN",
                           false, build, start, tick, fmt};
}
