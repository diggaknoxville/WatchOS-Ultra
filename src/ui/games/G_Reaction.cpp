// Reaktion: tap as soon as the disc turns green. Five rounds, score = average
// reaction time in ms (lower is better).
#include "GameKit.h"

using namespace games;
using namespace ui;

namespace {

const int CX = SCR_W / 2, CY = (AREA_Y0 + AREA_Y1) / 2 - 10;
lv_obj_t* sDisc;
lv_obj_t* sWord;
lv_obj_t* sLast;
enum Phase : uint8_t { P_IDLE, P_WAIT, P_GO, P_RESULT, P_END } phase;
uint32_t phaseAt, goAt, waitMs;
int round_;
uint32_t sum, best;

void status() {
    char b[24];
    snprintf(b, sizeof(b), "RUNDE %d/5", round_ > 5 ? 5 : round_);
    setStatus(b);
}

void build(lv_obj_t* area) {
    disc(area, CX, CY, 246, lv_color_hex(0x1E293B));
    disc(area, CX, CY, 240, lv_color_hex(0x0A0D14));
    sDisc = disc(area, CX, CY, 218, cSurf2());
    sWord = text(area, F.title, cText(), "TIPPEN");
    lv_obj_set_style_text_letter_space(sWord, 3, 0);
    lv_obj_align(sWord, LV_ALIGN_CENTER, 0, CY - SCR_H / 2);
    sLast = text(area, F.cap, cText2(), "");
    lv_obj_set_width(sLast, CW);
    lv_obj_set_style_text_align(sLast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sLast, LV_ALIGN_TOP_MID, 0, AREA_Y1 - 4);
}

void arm(uint32_t now) {
    phase = P_WAIT;
    phaseAt = now;
    waitMs = 1200 + rnd(2600);
    tint(sDisc, lv_color_hex(0xFF1744));
    setText(sWord, "WARTEN");
}

void start() {
    round_ = 1;
    sum = 0;
    best = 0;
    phase = P_IDLE;
    tint(sDisc, cSurf2());
    setText(sWord, "TIPPEN");
    setText(sLast, "Tippen zum Starten");
    status();
    setScore(0);
}

void tick(uint32_t, const Input& in) {
    uint32_t now = millis();
    switch (phase) {
        case P_IDLE:
            if (in.pressed) arm(now);
            break;
        case P_WAIT:
            if (in.pressed) {                         // too early: the round restarts
                setText(sLast, "Zu früh!");
                rumble(HAPTIC_ALERT_SHORT);
                arm(now);
            } else if (now - phaseAt >= waitMs) {
                phase = P_GO;
                goAt = now;
                tint(sDisc, lv_color_hex(0x00E676));
                setText(sWord, "JETZT!");
            }
            break;
        case P_GO:
            if (in.pressed) {
                uint32_t rt = now - goAt;
                sum += rt;
                if (!best || rt < best) best = rt;
                char b[40];
                snprintf(b, sizeof(b), "%lu ms", (unsigned long)rt);
                setText(sWord, b);
                snprintf(b, sizeof(b), "Beste Runde %lu ms", (unsigned long)best);
                setText(sLast, b);
                setScore(rt);
                tint(sDisc, cSurf2());
                rumble(HAPTIC_TICK);
                beep(990, 30);
                phase = P_RESULT;
                phaseAt = now;
            } else if (now - goAt > 2000) {
                setText(sLast, "Verschlafen");
                arm(now);
            }
            break;
        case P_RESULT:
            if (now - phaseAt >= 900) {
                round_++;
                if (round_ > 5) {
                    phase = P_END;
                    gameOver(sum / 5, true);
                } else {
                    status();
                    arm(now);
                }
            }
            break;
        case P_END:
            break;
    }
}

}  // namespace

namespace games {
const Game GAME_REACTION = {"REAKTION", ICO_BOLT, 0xFF453A, "TIPPEN, SOBALD DIE SCHEIBE GRÜN WIRD",
                            true, build, start, tick, nullptr};
}
