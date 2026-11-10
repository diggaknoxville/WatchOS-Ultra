// ============================================================================
//  CalculatorPage - Scientific / Advanced Calculator for WatchOS Ultra
// ============================================================================
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "MotionHaptic.h"
#include "DisplayEngine.h"
#include "CalcParser.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ui;

namespace pages {

static lv_obj_t* sCalcExprLbl = nullptr;
static lv_obj_t* sCalcResLbl = nullptr;
static lv_obj_t* sCalcDegRadPill = nullptr;
static lv_obj_t* sCalcMemBadge = nullptr;
static lv_obj_t* sCalcMemValLbl = nullptr;
static lv_obj_t* sCalcGridStandard = nullptr;
static lv_obj_t* sCalcGridScientific = nullptr;
static lv_obj_t* sCalcGridMemory = nullptr;
static lv_obj_t* sCalcTabStd = nullptr;
static lv_obj_t* sCalcTabSci = nullptr;
static lv_obj_t* sCalcTabMem = nullptr;

static char sExprBuf[128] = "";
static double sLastAns = 0.0;
static double sMemoryVal = 0.0;
static bool sDegMode = true; // true = DEG, false = RAD
static int sCalcMode = 0;   // 0 = Standard, 1 = Scientific, 2 = Memory

// Expression parser: include/CalcParser.h (host-tested, see test/test_calc)
static double evaluateFormula(const char* formula, bool* ok = nullptr) {
    double res = 0.0;
    bool good = calc::evaluate(formula, sDegMode, sLastAns, res);
    if (ok) *ok = good;
    return res;
}

static void updateDisplay(bool evalNow = false) {
    if (sCalcExprLbl) {
        setText(sCalcExprLbl, sExprBuf[0] ? sExprBuf : "0");
    }
    if (sCalcResLbl) {
        if (sExprBuf[0] == 0) {
            setText(sCalcResLbl, "0");
        } else {
            bool ok = false;
            double res = evaluateFormula(sExprBuf, &ok);
            char b[32];
            if (!ok) {
                snprintf(b, sizeof(b), "Fehler");
            } else {
                if (fabs(res) < 1e-12) res = 0.0;
                if (fabs(res) >= 1e9 || (fabs(res) > 0.0 && fabs(res) < 1e-4)) {
                    snprintf(b, sizeof(b), "%.5e", res);
                } else if (res == floor(res) && fabs(res) < 1e9) {
                    snprintf(b, sizeof(b), "%.0f", res);
                } else {
                    snprintf(b, sizeof(b), "%.6g", res);
                }
                if (evalNow) sLastAns = res;
            }
            setText(sCalcResLbl, b);
        }
    }
    if (sCalcMemValLbl) {
        char mb[32];
        if (fabs(sMemoryVal) < 1e-12) sMemoryVal = 0.0;
        if (sMemoryVal == floor(sMemoryVal) && fabs(sMemoryVal) < 1e9) snprintf(mb, sizeof(mb), "%.0f", sMemoryVal);
        else snprintf(mb, sizeof(mb), "%.6g", sMemoryVal);
        setText(sCalcMemValLbl, mb);
    }
    if (sCalcMemBadge) {
        setHidden(sCalcMemBadge, fabs(sMemoryVal) < 1e-12);
    }
}

static void handleKey(const char* key) {
    haptic(HAPTIC_TICK);
    if (!key) return;

    if (strcmp(key, "AC") == 0) {
        sExprBuf[0] = 0;
        updateDisplay();
        return;
    }
    if (strcmp(key, "DEL") == 0) {
        size_t len = strlen(sExprBuf);
        if (len > 0) sExprBuf[len - 1] = 0;
        updateDisplay();
        return;
    }
    if (strcmp(key, "=") == 0) {
        bool ok = false;
        double res = evaluateFormula(sExprBuf, &ok);
        if (ok) {
            sLastAns = res;
            if (fabs(res) < 1e-12) res = 0.0;
            if (res == floor(res) && fabs(res) < 1e9) snprintf(sExprBuf, sizeof(sExprBuf), "%.0f", res);
            else snprintf(sExprBuf, sizeof(sExprBuf), "%.6g", res);
        } else {
            sExprBuf[0] = 0;
        }
        updateDisplay(true);
        haptic(HAPTIC_CLICK);
        return;
    }
    if (strcmp(key, "+/-") == 0) {
        if (sExprBuf[0] == '-') {
            memmove(sExprBuf, sExprBuf + 1, strlen(sExprBuf));
        } else if (strlen(sExprBuf) + 2 < sizeof(sExprBuf)) {
            memmove(sExprBuf + 1, sExprBuf, strlen(sExprBuf) + 1);
            sExprBuf[0] = '-';
        }
        updateDisplay();
        return;
    }
    if (strcmp(key, "Ans") == 0) {
        char b[32];
        if (sLastAns == floor(sLastAns) && fabs(sLastAns) < 1e9) snprintf(b, sizeof(b), "%.0f", sLastAns);
        else snprintf(b, sizeof(b), "%.6g", sLastAns);
        if (strlen(sExprBuf) + strlen(b) < sizeof(sExprBuf)) strcat(sExprBuf, b);
        updateDisplay();
        return;
    }
    if (strcmp(key, "MC") == 0) {
        sMemoryVal = 0.0;
        updateDisplay();
        haptic(HAPTIC_DOUBLE_CLICK);
        return;
    }
    if (strcmp(key, "MR") == 0) {
        char b[32];
        if (sMemoryVal == floor(sMemoryVal) && fabs(sMemoryVal) < 1e9) snprintf(b, sizeof(b), "%.0f", sMemoryVal);
        else snprintf(b, sizeof(b), "%.6g", sMemoryVal);
        if (strlen(sExprBuf) + strlen(b) < sizeof(sExprBuf)) strcat(sExprBuf, b);
        updateDisplay();
        return;
    }
    if (strcmp(key, "M+") == 0) {
        bool ok = false;
        double res = sExprBuf[0] ? evaluateFormula(sExprBuf, &ok) : sLastAns;
        if (ok || sExprBuf[0] == 0) {
            sMemoryVal += res;
            updateDisplay();
            haptic(HAPTIC_SUCCESS);
        }
        return;
    }
    if (strcmp(key, "M-") == 0) {
        bool ok = false;
        double res = sExprBuf[0] ? evaluateFormula(sExprBuf, &ok) : sLastAns;
        if (ok || sExprBuf[0] == 0) {
            sMemoryVal -= res;
            updateDisplay();
            haptic(HAPTIC_SUCCESS);
        }
        return;
    }

    // Append key string
    size_t el = strlen(sExprBuf);
    size_t kl = strlen(key);
    if (el + kl < sizeof(sExprBuf)) {
        strcat(sExprBuf, key);
        updateDisplay();
    }
}

static void selectCalcMode(int m) {
    sCalcMode = m;
    setHidden(sCalcGridStandard, m != 0);
    setHidden(sCalcGridScientific, m != 1);
    setHidden(sCalcGridMemory, m != 2);

    auto setTabStyle = [](lv_obj_t* btn, bool active) {
        if (!btn) return;
        setBgColorIf(btn, active ? accent() : cSurf());
        setTextColorIf(lv_obj_get_child(btn, 0), active ? cBg() : cText2());
    };
    setTabStyle(sCalcTabStd, m == 0);
    setTabStyle(sCalcTabSci, m == 1);
    setTabStyle(sCalcTabMem, m == 2);
    haptic(HAPTIC_TICK);
}

void buildCalculator(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);

    // Header bar
    lv_obj_t* hdr = box(p, SCR_W, 50);
    lv_obj_set_pos(hdr, 0, 10);

    lv_obj_t* backBtn = box(hdr, 96, 44);
    lv_obj_set_pos(backBtn, 14, 3);
    lv_obj_set_style_bg_color(backBtn, cSurf(), 0);
    lv_obj_set_style_bg_opa(backBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(backBtn, 22, 0);
    lv_obj_set_style_border_width(backBtn, 1, 0);
    lv_obj_set_style_border_color(backBtn, cStroke(), 0);
    pressable(backBtn);
    lv_obj_t* bLbl = label(backBtn, F.small, accent(), ICO_CHEVRON_LEFT " Apps");
    accReg(bLbl, ACC_TEXT);
    lv_obj_center(bLbl);
    lv_obj_add_event_cb(backBtn, [](lv_event_t*) {
        nav::home();
        haptic(HAPTIC_TICK);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* titleLbl = label(hdr, F.title, cText(), "RECHNER");
    lv_obj_align(titleLbl, LV_ALIGN_CENTER, 0, 0);

    sCalcDegRadPill = pill(hdr, sDegMode ? "DEG" : "RAD", accent());
    accReg(sCalcDegRadPill, ACC_TEXT);
    lv_obj_align(sCalcDegRadPill, LV_ALIGN_RIGHT_MID, -14, 0);
    pressable(sCalcDegRadPill);                     // 28 px pill, 56 px hit region
    lv_obj_add_event_cb(sCalcDegRadPill, [](lv_event_t*) {
        sDegMode = !sDegMode;
        setPill(sCalcDegRadPill, sDegMode ? "DEG" : "RAD", accent());
        haptic(HAPTIC_TICK);
        updateDisplay();
    }, LV_EVENT_CLICKED, NULL);

    // Display Box (Expression + Result)
    lv_obj_t* dispCard = card(p, CW, 80);
    lv_obj_set_pos(dispCard, M, 64);
    lv_obj_set_style_radius(dispCard, 18, 0);
    lv_obj_set_style_pad_hor(dispCard, 16, 0);
    lv_obj_set_style_pad_ver(dispCard, 8, 0);

    sCalcMemBadge = pill(dispCard, "M", lv_color_hex(0x0A84FF));
    lv_obj_align(sCalcMemBadge, LV_ALIGN_TOP_LEFT, 0, 4);
    setHidden(sCalcMemBadge, true);

    sCalcExprLbl = label(dispCard, F.small, cText3(), "0");
    lv_obj_align(sCalcExprLbl, LV_ALIGN_TOP_RIGHT, 0, 4);

    sCalcResLbl = label(dispCard, F.numM, cText(), "0");
    lv_obj_align(sCalcResLbl, LV_ALIGN_BOTTOM_RIGHT, 0, -4);

    // Mode Selector Tab Bar: [ 123 ] [ fx ] [ MEM ]
    lv_obj_t* modeRow = box(p, CW, 40);
    lv_obj_set_pos(modeRow, M, 150);
    lv_obj_set_flex_flow(modeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modeRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeTabBtn = [&](const char* title, int mode) -> lv_obj_t* {
        lv_obj_t* b = box(modeRow, (CW - 16) / 3, 40);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, mode == 0 ? accent() : cSurf(), 0);
        pressable(b);
        lv_obj_t* l = label(b, F.small, mode == 0 ? cBg() : cText2(), title);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            int m = (int)(intptr_t)lv_event_get_user_data(e);
            selectCalcMode(m);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)mode);
        return b;
    };

    sCalcTabStd = makeTabBtn("123 Basis", 0);
    sCalcTabSci = makeTabBtn("fx Mathe", 1);
    sCalcTabMem = makeTabBtn("Speicher", 2);

    // -----------------------------------------------------------------------
    //  Keypad Grid Builder Helper
    // -----------------------------------------------------------------------
    const int KW = (CW - 24) / 4; // ~85px
    const int KH = 54;

    auto createGrid = [&](lv_obj_t* parent) -> lv_obj_t* {
        lv_obj_t* g = box(parent, CW, 290);
        lv_obj_set_pos(g, M, 196);
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(g, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(g, 6, 0);
        return g;
    };

    auto addKey = [&](lv_obj_t* g, const char* labelStr, lv_color_t bgCol, lv_color_t fgCol, int wSpan = 1) {
        int w = (wSpan == 1) ? KW : (KW * 2 + 8);
        lv_obj_t* btn = box(g, w, KH);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, bgCol, 0);
        pressable(btn);
        lv_obj_t* l = label(btn, (strlen(labelStr) > 3) ? F.cap : F.title, fgCol, labelStr);
        lv_obj_center(l);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            const char* k = (const char*)lv_event_get_user_data(e);
            handleKey(k);
        }, LV_EVENT_CLICKED, (void*)labelStr);
        return btn;
    };

    // -----------------------------------------------------------------------
    //  1. Standard Keypad
    // -----------------------------------------------------------------------
    sCalcGridStandard = createGrid(p);
    lv_color_t cOp = lv_color_hex(0xFF9F0A); // Warm amber
    lv_color_t cNum = cSurf2();
    lv_color_t cFn = lv_color_hex(0x232B3B);

    // Row 1: AC, DEL, %, ÷
    addKey(sCalcGridStandard, "AC", cFn, cCrit());
    addKey(sCalcGridStandard, "DEL", cFn, cText2());
    addKey(sCalcGridStandard, "%", cFn, cText());
    addKey(sCalcGridStandard, " / ", cOp, cBg());

    // Row 2: 7, 8, 9, ×
    addKey(sCalcGridStandard, "7", cNum, cText());
    addKey(sCalcGridStandard, "8", cNum, cText());
    addKey(sCalcGridStandard, "9", cNum, cText());
    addKey(sCalcGridStandard, " * ", cOp, cBg());

    // Row 3: 4, 5, 6, -
    addKey(sCalcGridStandard, "4", cNum, cText());
    addKey(sCalcGridStandard, "5", cNum, cText());
    addKey(sCalcGridStandard, "6", cNum, cText());
    addKey(sCalcGridStandard, " - ", cOp, cBg());

    // Row 4: 1, 2, 3, +
    addKey(sCalcGridStandard, "1", cNum, cText());
    addKey(sCalcGridStandard, "2", cNum, cText());
    addKey(sCalcGridStandard, "3", cNum, cText());
    addKey(sCalcGridStandard, " + ", cOp, cBg());

    // Row 5: ±, 0, ., =
    addKey(sCalcGridStandard, "+/-", cNum, cText());
    addKey(sCalcGridStandard, "0", cNum, cText());
    addKey(sCalcGridStandard, ".", cNum, cText());
    lv_obj_t* eqBtn = addKey(sCalcGridStandard, "=", accent(), cBg());
    accReg(eqBtn, ACC_BG);

    // -----------------------------------------------------------------------
    //  2. Scientific Keypad (fx)
    // -----------------------------------------------------------------------
    sCalcGridScientific = createGrid(p);
    lv_color_t cSci = lv_color_hex(0x182438);

    // Row 1: sin, cos, tan, pi
    addKey(sCalcGridScientific, "sin(", cSci, lv_color_hex(0x00E5FF));
    addKey(sCalcGridScientific, "cos(", cSci, lv_color_hex(0x00E5FF));
    addKey(sCalcGridScientific, "tan(", cSci, lv_color_hex(0x00E5FF));
    addKey(sCalcGridScientific, "pi", cSci, lv_color_hex(0xFFD60A));

    // Row 2: asin, acos, atan, e
    addKey(sCalcGridScientific, "asin(", cSci, lv_color_hex(0x00E5FF));
    addKey(sCalcGridScientific, "acos(", cSci, lv_color_hex(0x00E5FF));
    addKey(sCalcGridScientific, "atan(", cSci, lv_color_hex(0x00E5FF));
    addKey(sCalcGridScientific, "e", cSci, lv_color_hex(0xFFD60A));

    // Row 3: x², xʸ, √x, 1/x
    addKey(sCalcGridScientific, "^2", cSci, cText());
    addKey(sCalcGridScientific, "^", cSci, cText());
    addKey(sCalcGridScientific, "sqrt(", cSci, cText());
    addKey(sCalcGridScientific, "^(-1)", cSci, cText());

    // Row 4: ln, log, (, )
    addKey(sCalcGridScientific, "ln(", cSci, cText2());
    addKey(sCalcGridScientific, "log(", cSci, cText2());
    addKey(sCalcGridScientific, "(", cSci, cText());
    addKey(sCalcGridScientific, ")", cSci, cText());

    // Row 5: n!, abs, Ans, =
    addKey(sCalcGridScientific, "!", cSci, cText2());
    addKey(sCalcGridScientific, "abs(", cSci, cText2());
    addKey(sCalcGridScientific, "Ans", cSci, accent());
    lv_obj_t* eqSci = addKey(sCalcGridScientific, "=", accent(), cBg());
    accReg(eqSci, ACC_BG);
    setHidden(sCalcGridScientific, true);

    // -----------------------------------------------------------------------
    //  3. Memory Keypad
    // -----------------------------------------------------------------------
    sCalcGridMemory = createGrid(p);
    lv_color_t cMem = lv_color_hex(0x282038);

    addKey(sCalcGridMemory, "MC", cMem, cCrit());
    addKey(sCalcGridMemory, "MR", cMem, cOk());
    addKey(sCalcGridMemory, "M+", cMem, lv_color_hex(0x0A84FF));
    addKey(sCalcGridMemory, "M-", cMem, lv_color_hex(0x0A84FF));

    // Memory status label inside the memory view
    lv_obj_t* memBox = card(sCalcGridMemory, CW, 140);
    lv_obj_set_style_radius(memBox, 16, 0);
    lv_obj_t* mHead = caption(memBox, "GESPEICHERTER WERT (M)", accent());
    accReg(mHead, ACC_TEXT);
    lv_obj_align(mHead, LV_ALIGN_TOP_MID, 0, 14);
    sCalcMemValLbl = label(memBox, F.title, cText(), "0");
    lv_obj_align(sCalcMemValLbl, LV_ALIGN_CENTER, 0, 10);

    lv_obj_add_event_cb(sCalcGridMemory, [](lv_event_t*) {}, LV_EVENT_CLICKED, NULL);

    setHidden(sCalcGridMemory, true);

    nav::PageDef d;
    d.root = p;
    d.refreshMs = 0xFFFF; // event driven
    d.ttlMs = 0;
    d.onShow = [] {
        updateDisplay();
    };
    nav::registerPage(nav::PG_APP_CALC, d);
}

} // namespace pages

