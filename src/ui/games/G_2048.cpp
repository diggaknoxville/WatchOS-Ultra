// 2048: swipe to slide the tiles, equal tiles merge. Score = merge sum.
#include "GameKit.h"
#include <string.h>

using namespace games;
using namespace ui;

namespace {

const int N = 4, TS = 74, GAP = 8;
const int OX = AREA_X0 + (AREA_W - (N * TS + (N - 1) * GAP)) / 2;
const int OY = AREA_Y0 + (AREA_H - (N * TS + (N - 1) * GAP)) / 2;

uint16_t g[N][N];
lv_obj_t* tile[N][N];
lv_obj_t* lbl[N][N];
uint32_t score;
bool over;

uint32_t colorFor(uint16_t v) {
    switch (v) {
        case 2: return 0x2A2F3A; case 4: return 0x3A4252; case 8: return 0xC46A2A;
        case 16: return 0xD8842E; case 32: return 0xE06A3C; case 64: return 0xE54A2A;
        case 128: return 0xC9A227; case 256: return 0xD4AD1C; case 512: return 0xE0B70F;
        case 1024: return 0xEBC000; case 2048: return 0xFFD60A; default: return 0x30D158;
    }
}

void draw() {
    char b[8];
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            uint16_t v = g[r][c];
            tint(tile[r][c], v ? lv_color_hex(colorFor(v)) : lv_color_hex(0x151922));
            if (v) {
                snprintf(b, sizeof(b), "%u", (unsigned)v);
                setText(lbl[r][c], b);
                lv_obj_set_style_text_font(lbl[r][c], v >= 1000 ? F.small : F.numS, 0);
                setTextColorIf(lbl[r][c], v <= 4 ? cText2() : cText());
                lv_obj_center(lbl[r][c]);
                setHidden(lbl[r][c], false);
            } else {
                setHidden(lbl[r][c], true);
            }
        }
}

void spawn() {
    int freeN = 0;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (!g[r][c]) freeN++;
    if (!freeN) return;
    int k = rnd(freeN);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!g[r][c] && k-- == 0) { g[r][c] = rnd(10) < 9 ? 2 : 4; return; }
}

// slide one line of 4 towards index 0; returns true if anything moved
bool slide(uint16_t* v[N]) {
    uint16_t out[N] = {0, 0, 0, 0};
    int o = 0;
    bool moved = false;
    for (int i = 0; i < N; i++) {
        if (!*v[i]) continue;
        if (o > 0 && out[o - 1] == *v[i] && !(out[o - 1] & 0x8000)) {
            out[o - 1] = (uint16_t)(out[o - 1] * 2) | 0x8000;     // merged flag
            score += out[o - 1] & 0x7FFF;
            moved = true;
        } else {
            out[o++] = *v[i];
        }
    }
    for (int i = 0; i < N; i++) {
        uint16_t nv = out[i] & 0x7FFF;
        if (*v[i] != nv) moved = true;
        *v[i] = nv;
    }
    return moved;
}

bool move(Swipe s) {
    bool moved = false;
    for (int k = 0; k < N; k++) {
        uint16_t* line[N];
        for (int i = 0; i < N; i++) {
            switch (s) {
                case SW_LEFT:  line[i] = &g[k][i]; break;
                case SW_RIGHT: line[i] = &g[k][N - 1 - i]; break;
                case SW_UP:    line[i] = &g[i][k]; break;
                default:       line[i] = &g[N - 1 - i][k]; break;
            }
        }
        moved |= slide(line);
    }
    return moved;
}

bool canMove() {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!g[r][c]) return true;
            if (c + 1 < N && g[r][c] == g[r][c + 1]) return true;
            if (r + 1 < N && g[r][c] == g[r + 1][c]) return true;
        }
    return false;
}

void build(lv_obj_t* area) {
    rect(area, OX - GAP, OY - GAP, N * TS + (N + 1) * GAP, N * TS + (N + 1) * GAP, lv_color_hex(0x0B0E14), 12);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            tile[r][c] = rect(area, OX + c * (TS + GAP), OY + r * (TS + GAP), TS, TS, lv_color_hex(0x151922), 8);
            lbl[r][c] = text(tile[r][c], F.numS, cText(), "");
            lv_obj_center(lbl[r][c]);
        }
}

void start() {
    memset(g, 0, sizeof(g));
    score = 0;
    over = false;
    spawn();
    spawn();
    draw();
    setScore(0);
}

void tick(uint32_t, const Input& in) {
    if (over || in.swipe == SW_NONE) return;
    if (move(in.swipe)) {
        spawn();
        draw();
        setScore(score);
        rumble(HAPTIC_TICK);
        if (!canMove()) { over = true; gameOver(score); }
    }
}

}  // namespace

namespace games {
const Game GAME_2048 = {"2048", ICO_GRID_VIEW, 0xFFD60A, "WISCHEN SCHIEBT • GLEICHE KACHELN VERSCHMELZEN",
                        false, build, start, tick, nullptr};
}
