// Labyrinth: tilt the watch to roll the ball through a random maze to the
// green exit. Score = time (lower is better).
#include "GameKit.h"
#include <math.h>
#include <string.h>

using namespace games;
using namespace ui;

namespace {

const int COLS = 9, ROWS = 11, CS = 32, WT = 3;
const int OX = AREA_X0 + (AREA_W - COLS * CS) / 2;    // 61
const int OY = AREA_Y0 + (AREA_H - ROWS * CS) / 2;    // 80
const int BALL = 18;
const float R = BALL / 2.0f + 1.5f;

bool wallV[ROWS][COLS + 1];       // wall on the left edge of cell (r, c); c == COLS: right border
bool wallH[ROWS + 1][COLS];       // wall on the top edge of cell (r, c); r == ROWS: bottom border
lv_obj_t* objV[ROWS][COLS + 1];
lv_obj_t* objH[ROWS + 1][COLS];
lv_obj_t* sBall;
lv_obj_t* sGoal;
float bx, by, vx, vy;
uint32_t sElapsed;
bool sDone;

void generate() {
    memset(wallV, 1, sizeof(wallV));
    memset(wallH, 1, sizeof(wallH));
    bool seen[ROWS][COLS] = {};
    struct Cell { int8_t r, c; } stack[ROWS * COLS];
    int sp = 0;
    stack[sp++] = {0, 0};
    seen[0][0] = true;
    while (sp > 0) {
        Cell cur = stack[sp - 1];
        int8_t nr[4], nc[4];
        int n = 0;
        const int8_t dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
        for (int k = 0; k < 4; k++) {
            int r = cur.r + dr[k], c = cur.c + dc[k];
            if (r >= 0 && r < ROWS && c >= 0 && c < COLS && !seen[r][c]) { nr[n] = r; nc[n] = c; n++; }
        }
        if (!n) { sp--; continue; }
        int k = rnd(n);
        int r = nr[k], c = nc[k];
        if (r == cur.r - 1) wallH[cur.r][cur.c] = false;
        else if (r == cur.r + 1) wallH[r][c] = false;
        else if (c == cur.c - 1) wallV[cur.r][cur.c] = false;
        else wallV[r][c] = false;
        seen[r][c] = true;
        stack[sp++] = {(int8_t)r, (int8_t)c};
    }
}

void showWalls() {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c <= COLS; c++) setHidden(objV[r][c], !wallV[r][c]);
    for (int r = 0; r <= ROWS; r++)
        for (int c = 0; c < COLS; c++) setHidden(objH[r][c], !wallH[r][c]);
}

void build(lv_obj_t* area) {
    lv_color_t wc = lv_color_hex(0x3A4252);
    rect(area, OX - WT, OY - WT, COLS * CS + 2 * WT, ROWS * CS + 2 * WT, lv_color_hex(0x0B0E14), 6);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c <= COLS; c++)
            objV[r][c] = rect(area, OX + c * CS - WT / 2, OY + r * CS - WT / 2, WT, CS + WT, wc);
    for (int r = 0; r <= ROWS; r++)
        for (int c = 0; c < COLS; c++)
            objH[r][c] = rect(area, OX + c * CS - WT / 2, OY + r * CS - WT / 2, CS + WT, WT, wc);
    sGoal = disc(area, OX + (COLS - 1) * CS + CS / 2, OY + (ROWS - 1) * CS + CS / 2, 20, cOk());
    sBall = disc(area, 0, 0, BALL, accent());
}

void start() {
    generate();
    showWalls();
    bx = OX + CS / 2.0f;
    by = OY + CS / 2.0f;
    vx = vy = 0;
    sElapsed = 0;
    sDone = false;
    place(sBall, (int)(bx - BALL / 2), (int)(by - BALL / 2));
}

// move one axis, stopping at walls of every row (or column) the ball touches
void moveX(float dx) {
    float nx = bx + dx;
    int r0 = (int)((by - R) - OY) / CS, r1 = (int)((by + R) - OY) / CS;
    if (r0 < 0) r0 = 0;
    if (r1 >= ROWS) r1 = ROWS - 1;
    int c = (int)((bx - OX) / CS);
    if (c < 0) c = 0;
    if (c >= COLS) c = COLS - 1;
    if (dx > 0) {
        float edge = OX + (c + 1) * CS;
        bool wall = false;
        for (int r = r0; r <= r1; r++) wall |= wallV[r][c + 1];
        if (wall && nx + R > edge) { nx = edge - R; vx = -vx * 0.25f; }
    } else if (dx < 0) {
        float edge = OX + c * CS;
        bool wall = false;
        for (int r = r0; r <= r1; r++) wall |= wallV[r][c];
        if (wall && nx - R < edge) { nx = edge + R; vx = -vx * 0.25f; }
    }
    bx = nx;
}

void moveY(float dy) {
    float ny = by + dy;
    int c0 = (int)((bx - R) - OX) / CS, c1 = (int)((bx + R) - OX) / CS;
    if (c0 < 0) c0 = 0;
    if (c1 >= COLS) c1 = COLS - 1;
    int r = (int)((by - OY) / CS);
    if (r < 0) r = 0;
    if (r >= ROWS) r = ROWS - 1;
    if (dy > 0) {
        float edge = OY + (r + 1) * CS;
        bool wall = false;
        for (int c = c0; c <= c1; c++) wall |= wallH[r + 1][c];
        if (wall && ny + R > edge) { ny = edge - R; vy = -vy * 0.25f; }
    } else if (dy < 0) {
        float edge = OY + r * CS;
        bool wall = false;
        for (int c = c0; c <= c1; c++) wall |= wallH[r][c];
        if (wall && ny - R < edge) { ny = edge + R; vy = -vy * 0.25f; }
    }
    by = ny;
}

void tick(uint32_t dtMs, const Input& in) {
    if (sDone) return;
    float dt = dtMs / 1000.0f;
    sElapsed += dtMs;
    const float ACC = 900.0f, DAMP = 0.90f, VMAX = 260.0f;
    vx += in.tx * ACC * dt;
    vy += in.ty * ACC * dt;
    vx *= powf(DAMP, dt * 30.0f);
    vy *= powf(DAMP, dt * 30.0f);
    if (vx > VMAX) vx = VMAX;
    if (vx < -VMAX) vx = -VMAX;
    if (vy > VMAX) vy = VMAX;
    if (vy < -VMAX) vy = -VMAX;
    // sub-steps keep the ball from tunnelling through 3 px walls
    int steps = 1 + (int)(fmaxf(fabsf(vx), fabsf(vy)) * dt / 6.0f);
    for (int i = 0; i < steps; i++) {
        moveX(vx * dt / steps);
        moveY(vy * dt / steps);
    }
    place(sBall, (int)lroundf(bx - BALL / 2), (int)lroundf(by - BALL / 2));
    setScore(sElapsed / 1000);
    int gc = COLS - 1, gr = ROWS - 1;
    float gx = OX + gc * CS + CS / 2.0f, gy = OY + gr * CS + CS / 2.0f;
    if (fabsf(bx - gx) < 8 && fabsf(by - gy) < 8) {
        sDone = true;
        gameOver(sElapsed, true);
    }
}

}  // namespace

namespace games {
const Game GAME_LABYRINTH = {"LABYRINTH", ICO_EXPLORE, 0xFF9F0A, "NEIGEN: KUGEL ZUM GRÜNEN AUSGANG ROLLEN",
                             true, build, start, tick, nullptr};
}
