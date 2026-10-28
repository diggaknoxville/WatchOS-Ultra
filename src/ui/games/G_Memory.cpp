// Memory: find the eight pairs. Score = moves (lower is better).
#include "GameKit.h"

using namespace games;
using namespace ui;

namespace {

const int N = 4, CS = 72, GAP = 8;
const int OX = AREA_X0 + (AREA_W - (N * CS + (N - 1) * GAP)) / 2;
const int OY = AREA_Y0 + (AREA_H - (N * CS + (N - 1) * GAP)) / 2;
const char* const ICONS[8] = {ICO_STAR, ICO_FAVORITE, ICO_BOLT, ICO_CLOUD,
                              ICO_WB_SUNNY, ICO_WATER_DROP, ICO_LOCAL_FIRE_DEPARTMENT, ICO_NIGHTS_STAY};
const uint32_t COLORS[8] = {0xFFD60A, 0xFF375F, 0xFF9F0A, 0x64D2FF, 0xFFB340, 0x0A84FF, 0xFF453A, 0xBF5AF2};

lv_obj_t* card_[16];
lv_obj_t* icon_[16];
uint8_t kind[16];
bool up[16], done[16];
int first, second;
uint32_t hideAt;
uint32_t moves;
int pairs;
bool over;

void show(int i) {
    bool vis = up[i] || done[i];
    tint(card_[i], done[i] ? lv_color_hex(0x0E3A20) : (vis ? lv_color_hex(0x243248) : lv_color_hex(0x161E2E)));
    lv_obj_set_style_border_color(card_[i], done[i] ? lv_color_hex(0x2ECC71) : (vis ? accent() : lv_color_hex(0x2B3A52)), 0);
    setHidden(icon_[i], !vis);
}

void build(lv_obj_t* area) {
    for (int i = 0; i < 16; i++) {
        card_[i] = rect(area, OX + (i % N) * (CS + GAP), OY + (i / N) * (CS + GAP), CS, CS, lv_color_hex(0x161E2E), 12);
        lv_obj_set_style_border_width(card_[i], 2, 0);
        lv_obj_set_style_border_color(card_[i], lv_color_hex(0x2B3A52), 0);
        icon_[i] = text(card_[i], F.icoM, cText(), "");
        lv_obj_center(icon_[i]);
    }
}

void start() {
    for (int i = 0; i < 16; i++) kind[i] = i / 2;
    for (int i = 15; i > 0; i--) { int j = rnd(i + 1); uint8_t t = kind[i]; kind[i] = kind[j]; kind[j] = t; }
    for (int i = 0; i < 16; i++) {
        up[i] = done[i] = false;
        setText(icon_[i], ICONS[kind[i]]);
        setTextColorIf(icon_[i], lv_color_hex(COLORS[kind[i]]));
        lv_obj_center(icon_[i]);
        show(i);
    }
    first = second = -1;
    hideAt = 0;
    moves = 0;
    pairs = 0;
    over = false;
    setScore(0);
    setStatus("PAARE 0/8");
}

int cardAt(int x, int y) {
    int c = (x - OX) / (CS + GAP), r = (y - OY) / (CS + GAP);
    if (x < OX || y < OY || c >= N || r >= N) return -1;
    if ((x - OX) % (CS + GAP) >= CS || (y - OY) % (CS + GAP) >= CS) return -1;
    return r * N + c;
}

void tick(uint32_t, const Input& in) {
    if (over) return;
    uint32_t now = millis();
    if (hideAt && now >= hideAt) {
        hideAt = 0;
        up[first] = up[second] = false;
        show(first); show(second);
        first = second = -1;
    }
    if (!in.tapped || hideAt) return;
    int i = cardAt(in.x, in.y);
    if (i < 0 || up[i] || done[i]) return;
    up[i] = true;
    show(i);
    rumble(HAPTIC_TICK);
    if (first < 0) { first = i; return; }
    second = i;
    moves++;
    setScore(moves);
    if (kind[first] == kind[second]) {
        done[first] = done[second] = true;
        show(first); show(second);
        first = second = -1;
        pairs++;
        char b[16];
        snprintf(b, sizeof(b), "PAARE %d/8", pairs);
        setStatus(b);
        beep(880, 40);
        if (pairs == 8) { over = true; gameOver(moves, true); }
    } else {
        hideAt = now + 750;
    }
}

void fmt(uint32_t v, char* out, size_t n) { snprintf(out, n, "%lu Züge", (unsigned long)v); }

}  // namespace

namespace games {
const Game GAME_MEMORY = {"MEMORY", ICO_FAVORITE, 0xFF375F, "ZWEI KARTEN TIPPEN • ALLE PAARE FINDEN",
                          true, build, start, tick, fmt};
}
