// Würfel-Duell: shake the wrist (or tap) to roll two dice against the watch.
// Five rounds, the higher sum wins the round. Score = rounds won.
#include "GameKit.h"
#include <string.h>

using namespace games;
using namespace ui;

namespace {

const int D = 76;                           // die size
const int PIP = 12;
// pip positions per face (index into the 3x3 grid)
const uint8_t FACES[7][9] = {
    {0,0,0, 0,0,0, 0,0,0},
    {0,0,0, 0,1,0, 0,0,0},
    {1,0,0, 0,0,0, 0,0,1},
    {1,0,0, 0,1,0, 0,0,1},
    {1,0,1, 0,0,0, 1,0,1},
    {1,0,1, 0,1,0, 1,0,1},
    {1,0,1, 1,0,1, 1,0,1},
};

struct Die { lv_obj_t* body; lv_obj_t* pip[9]; uint8_t face; };
Die sDie[4];                                // 0,1 player; 2,3 watch
lv_obj_t* sMsg;
lv_obj_t* sYou;
lv_obj_t* sWatch;
enum Phase : uint8_t { P_WAIT, P_ROLL_P, P_ROLL_W, P_SHOW, P_END } phase;
uint32_t phaseMs, rollTick;
int round_, wins, losses;
uint32_t lastShakeMs;

void showFace(Die& d, int f) {
    d.face = f;
    for (int i = 0; i < 9; i++) setHidden(d.pip[i], !FACES[f][i]);
}

void makeDie(lv_obj_t* area, Die& d, int x, int y, lv_color_t col) {
    d.body = rect(area, x, y, D, D, col, 14);
    for (int i = 0; i < 9; i++) {
        int cx = x + D / 2 + ((i % 3) - 1) * 22, cy = y + D / 2 + ((i / 3) - 1) * 22;
        d.pip[i] = disc(area, cx, cy, PIP, lv_color_hex(0x101318));
    }
    showFace(d, 1);
}

void status() {
    char b[40];
    snprintf(b, sizeof(b), "RUNDE %d/5  •  %d : %d", round_ > 5 ? 5 : round_, wins, losses);
    setStatus(b);
}

void build(lv_obj_t* area) {
    const int cx = SCR_W / 2;
    sYou = text(area, F.cap, cText2(), "DU");
    lv_obj_set_style_text_letter_space(sYou, 2, 0);
    lv_obj_align(sYou, LV_ALIGN_TOP_MID, 0, 96);
    makeDie(area, sDie[0], cx - D - 12, 118, lv_color_hex(0xF4F6FA));
    makeDie(area, sDie[1], cx + 12, 118, lv_color_hex(0xF4F6FA));
    sMsg = text(area, F.body, cText(), "");
    lv_obj_set_width(sMsg, CW);
    lv_obj_set_style_text_align(sMsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sMsg, LV_ALIGN_TOP_MID, 0, 214);
    sWatch = text(area, F.cap, cText2(), "UHR");
    lv_obj_set_style_text_letter_space(sWatch, 2, 0);
    lv_obj_align(sWatch, LV_ALIGN_TOP_MID, 0, 262);
    makeDie(area, sDie[2], cx - D - 12, 284, lv_color_hex(0x9BA4B4));
    makeDie(area, sDie[3], cx + 12, 284, lv_color_hex(0x9BA4B4));
}

void start() {
    round_ = 1;
    wins = losses = 0;
    phase = P_WAIT;
    for (int i = 0; i < 4; i++) showFace(sDie[i], 1);
    setText(sMsg, "Schütteln oder tippen");
    status();
    setScore(0);
}

void tick(uint32_t dtMs, const Input& in) {
    uint32_t now = millis();
    switch (phase) {
        case P_WAIT:
            if (in.tapped || (in.shake > 0.9f && now - lastShakeMs > 600)) {
                lastShakeMs = now;
                phase = P_ROLL_P;
                phaseMs = now;
                rollTick = 0;
                setText(sMsg, "");
                rumble(HAPTIC_CLICK);
            }
            break;
        case P_ROLL_P:
        case P_ROLL_W: {
            int a = phase == P_ROLL_P ? 0 : 2;
            if (now - rollTick >= 70) {
                rollTick = now;
                showFace(sDie[a], 1 + rnd(6));
                showFace(sDie[a + 1], 1 + rnd(6));
                beep(300 + rnd(300), 15);
            }
            if (now - phaseMs >= 850) {
                rumble(HAPTIC_TICK);
                if (phase == P_ROLL_P) { phase = P_ROLL_W; phaseMs = now + 250; }
                else {
                    int p = sDie[0].face + sDie[1].face, w = sDie[2].face + sDie[3].face;
                    char b[48];
                    if (p > w) { wins++; snprintf(b, sizeof(b), "%d : %d  Runde gewonnen", p, w); rumble(HAPTIC_SUCCESS); }
                    else if (p < w) { losses++; snprintf(b, sizeof(b), "%d : %d  Runde verloren", p, w); rumble(HAPTIC_ALERT_SHORT); }
                    else snprintf(b, sizeof(b), "%d : %d  Unentschieden", p, w);
                    setText(sMsg, b);
                    setScore(wins);
                    status();
                    phase = P_SHOW;
                    phaseMs = now;
                }
            }
            break;
        }
        case P_SHOW:
            if (now - phaseMs >= 1400) {
                round_++;
                if (round_ > 5) {
                    phase = P_END;
                    gameOver(wins, wins > losses);
                } else {
                    phase = P_WAIT;
                    setText(sMsg, "Schütteln oder tippen");
                    status();
                }
            }
            break;
        case P_END:
            break;
    }
    (void)dtMs;
}

}  // namespace

namespace games {
const Game GAME_DICE = {"WÜRFEL", ICO_APPS, 0xBF5AF2, "SCHÜTTELN WÜRFELT • 5 RUNDEN GEGEN DIE UHR",
                        false, build, start, tick, nullptr};
}
