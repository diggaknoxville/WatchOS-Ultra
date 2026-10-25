#include "ui/UiKit.h"
#include "SettingsStorage.h"
#include "../fonts/font_orbitron.h"
#include "../fonts/font_rajdhani.h"
#include "../fonts/font_icons.h"

namespace ui {

Fonts F;

const char* WD_S[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* WD_L[7] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
const char* MO_S[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const char* MO_L[12] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST",
                        "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
const char* WD_DE_S[7] = {"SO", "MO", "DI", "MI", "DO", "FR", "SA"};
const char* WD_DE_L[7] = {"SONNTAG", "MONTAG", "DIENSTAG", "MITTWOCH", "DONNERSTAG", "FREITAG", "SAMSTAG"};
const char* MO_DE_S[12] = {"JAN", "FEB", "MÄR", "APR", "MAI", "JUN", "JUL", "AUG", "SEP", "OKT", "NOV", "DEZ"};
const char* MO_DE_L[12] = {"JANUAR", "FEBRUAR", "MÄRZ", "APRIL", "MAI", "JUNI", "JULI", "AUGUST",
                           "SEPTEMBER", "OKTOBER", "NOVEMBER", "DEZEMBER"};

lv_color_t colorFrom565(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F) << 3; r |= r >> 5;
    uint8_t g = ((c >> 5) & 0x3F) << 2;  g |= g >> 6;
    uint8_t b = (c & 0x1F) << 3;         b |= b >> 5;
    return lv_color_make(r, g, b);
}

lv_color_t accent() { return colorFrom565(SettingsStorage::get().accentColor); }

void initFonts() {
    F.hero  = lv_tiny_ttf_create_data(orbitron_ttf, orbitron_ttf_len, 112);
    F.giant = lv_tiny_ttf_create_data(orbitron_ttf, orbitron_ttf_len, 160);
    F.numL  = lv_tiny_ttf_create_data(orbitron_ttf, orbitron_ttf_len, 52);
    F.numM  = lv_tiny_ttf_create_data(orbitron_ttf, orbitron_ttf_len, 30);
    F.numS  = lv_tiny_ttf_create_data(orbitron_ttf, orbitron_ttf_len, 20);
    F.title = lv_tiny_ttf_create_data(rajdhani_ttf, rajdhani_ttf_len, 34);
    F.body  = lv_tiny_ttf_create_data(rajdhani_ttf, rajdhani_ttf_len, 26);
    F.small = lv_tiny_ttf_create_data(rajdhani_ttf, rajdhani_ttf_len, 21);
    F.cap   = lv_tiny_ttf_create_data(rajdhani_ttf, rajdhani_ttf_len, 17);
    F.icon  = &lv_font_montserrat_24;
    F.iconS = &lv_font_montserrat_20;
    F.iconL = &lv_font_montserrat_36;
    F.icoS  = lv_tiny_ttf_create_data(icons_ttf, icons_ttf_len, 24);
    F.icoM  = lv_tiny_ttf_create_data(icons_ttf, icons_ttf_len, 32);
    F.icoL  = lv_tiny_ttf_create_data(icons_ttf, icons_ttf_len, 44);
    F.icoXL = lv_tiny_ttf_create_data(icons_ttf, icons_ttf_len, 64);
}

// ---------------------------------------------------------------- accent
struct AccRef { lv_obj_t* o; AccKind k; lv_part_t part; };
static AccRef sAcc[160];
static int sAccN = 0;

void accReg(lv_obj_t* o, AccKind k, lv_part_t part) {
    if (sAccN < (int)(sizeof(sAcc) / sizeof(sAcc[0]))) sAcc[sAccN++] = {o, k, part};
}

void applyAccent() {
    lv_color_t a = accent();
    for (int i = 0; i < sAccN; i++) {
        AccRef& r = sAcc[i];
        switch (r.k) {
            case ACC_TEXT:   lv_obj_set_style_text_color(r.o, a, r.part); break;
            case ACC_BG:     lv_obj_set_style_bg_color(r.o, a, r.part); break;
            case ACC_ARC:    lv_obj_set_style_arc_color(r.o, a, r.part); break;
            case ACC_LINE:   lv_obj_set_style_line_color(r.o, a, r.part); break;
            case ACC_BORDER: lv_obj_set_style_border_color(r.o, a, r.part); break;
        }
    }
}

// ---------------------------------------------------------------- blocks
lv_obj_t* label(lv_obj_t* p, const lv_font_t* f, lv_color_t c, const char* t) {
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    return l;
}

lv_obj_t* caption(lv_obj_t* p, const char* t, lv_color_t c) {
    lv_obj_t* l = label(p, F.cap, c, t);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

static void plain(lv_obj_t* o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* box(lv_obj_t* p, int w, int h) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    plain(o);
    return o;
}

lv_obj_t* card(lv_obj_t* p, int w, int h) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, cSurf(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, R_CARD, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, cStroke(), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* ring(lv_obj_t* p, int size, int w, lv_color_t col) {
    lv_obj_t* a = lv_arc_create(p);
    lv_obj_set_size(a, size, size);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, col, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    return a;
}

lv_obj_t* pageRoot(lv_obj_t* parent) {
    lv_obj_t* p = box(parent, SCR_W, SCR_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, cBg(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

void title(lv_obj_t* page, const char* text) {
    lv_obj_t* t = label(page, F.title, cText(), text);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_set_pos(t, M, 22);
    lv_obj_t* u = box(page, 28, 4);
    lv_obj_set_style_radius(u, 2, 0);
    lv_obj_set_style_bg_opa(u, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(u, accent(), 0);
    lv_obj_set_pos(u, M, 66);
    accReg(u, ACC_BG);
}

lv_obj_t* kvCard(lv_obj_t* page, int y, int rows) {
    lv_obj_t* c = card(page, CW, rows * 42 + 24);
    lv_obj_set_pos(c, M, y);
    lv_obj_set_style_pad_hor(c, 20, 0);
    lv_obj_set_style_pad_ver(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 0, 0);
    return c;
}

lv_obj_t* kvRow(lv_obj_t* c, const char* key, bool last) {
    lv_obj_t* r = box(c, LV_PCT(100), 42);
    if (!last) {
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, cStroke(), 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    }
    lv_obj_t* k = caption(r, key, cText3());
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* v = label(r, F.body, cText(), "");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

lv_obj_t* iconText(lv_obj_t* btn, const char* icon, const char* text, lv_color_t col) {
    lv_obj_t* row = box(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    label(row, F.icoS, col, icon);
    lv_obj_t* t = label(row, F.body, col, text);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_center(row);
    return t;
}

// ---------------------------------------------------------------- components
void pressable(lv_obj_t* o) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(o, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(o, LV_OPA_40, LV_STATE_DISABLED);
}

lv_obj_t* pill(lv_obj_t* parent, const char* text, lv_color_t color) {
    lv_obj_t* p = box(parent, LV_SIZE_CONTENT, PILL_H);
    lv_obj_set_style_pad_hor(p, 12, 0);
    lv_obj_set_style_radius(p, PILL_H / 2, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(p, color, 0);
    lv_obj_t* l = caption(p, text, color);
    lv_obj_center(l);
    return p;
}

lv_obj_t* pillLabel(lv_obj_t* p) { return lv_obj_get_child(p, 0); }

void setPill(lv_obj_t* p, const char* text, lv_color_t color) {
    if (!p) return;
    lv_obj_t* l = pillLabel(p);
    setBgColorIf(p, color);
    if (l) {
        setTextColorIf(l, color);
        setText(l, text);
    }
}

lv_obj_t* emptyState(lv_obj_t* parent, const char* icon, const char* titleTxt, const char* hint) {
    lv_obj_t* c = box(parent, LV_PCT(100), 170);
    lv_obj_center(c);
    lv_obj_t* ic = label(c, F.icoXL, cText3(), icon);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t* t = label(c, F.body, cText2(), titleTxt);
    lv_obj_set_width(t, LV_PCT(90));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_t* h = caption(c, hint ? hint : "", cText3());
    lv_obj_set_width(h, LV_PCT(90));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 122);
    return c;
}

lv_obj_t* sectionHeader(lv_obj_t* parent, int y, const char* text) {
    lv_obj_t* l = caption(parent, text, cText3());
    if (y >= 0) lv_obj_set_pos(l, M, y);
    return l;
}

static lv_obj_t* rowBase(lv_obj_t* parent, int y, const char* icon, const char* text, lv_event_cb_t cb, void* user) {
    lv_obj_t* r = card(parent, CW, BTN_H);
    lv_obj_set_style_radius(r, R_TILE, 0);
    if (y >= 0) lv_obj_set_pos(r, M, y);
    pressable(r);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* ic = label(r, F.icoS, accent(), icon);
    accReg(ic, ACC_TEXT);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t* t = label(r, F.small, cText(), text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 52, 0);
    return r;
}

lv_obj_t* toggleRow(lv_obj_t* parent, int y, const char* icon, const char* text, bool state,
                    lv_event_cb_t cb, void* user) {
    lv_obj_t* r = rowBase(parent, y, icon, text, cb, user);
    lv_obj_t* track = box(r, 46, 26);                 // child 2
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(track, 13, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_t* knob = box(track, 20, 20);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(knob, 10, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(knob, cText(), 0);
    setToggleRow(r, state);
    return r;
}

void setToggleRow(lv_obj_t* r, bool state) {
    if (!r) return;
    lv_obj_t* track = lv_obj_get_child(r, 2);
    if (!track) return;
    lv_obj_t* knob = lv_obj_get_child(track, 0);
    setBgColorIf(track, state ? accent() : cStroke());
    if (knob) lv_obj_align(knob, state ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, state ? -3 : 3, 0);
}

lv_obj_t* valueRow(lv_obj_t* parent, int y, const char* icon, const char* text, const char* value,
                   lv_event_cb_t cb, void* user) {
    lv_obj_t* r = rowBase(parent, y, icon, text, cb, user);
    lv_obj_t* v = label(r, F.small, accent(), value);       // child 2
    accReg(v, ACC_TEXT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -16, 0);
    return r;
}

void setValueRow(lv_obj_t* r, const char* value) {
    if (!r) return;
    lv_obj_t* v = lv_obj_get_child(r, 2);
    if (v) setText(v, value);
}

static lv_obj_t* buttonBase(lv_obj_t* parent, int w, int h, const char* icon, const char* text,
                            lv_color_t bg, lv_color_t fg, bool outline) {
    lv_obj_t* b = box(parent, w, h);
    lv_obj_set_style_radius(b, h >= 48 ? R_TILE : h / 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    if (outline) {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, cStroke(), 0);
    }
    pressable(b);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 8, 0);
    if (icon && icon[0]) {
        lv_obj_t* ic = label(b, F.icoS, fg, icon);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* t = label(b, F.small, fg, text ? text : "");
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    if (!text || !text[0]) lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    return b;
}

lv_obj_t* primaryButton(lv_obj_t* parent, int w, int h, const char* icon, const char* text) {
    lv_obj_t* b = buttonBase(parent, w, h, icon, text, accent(), cBg(), false);
    accReg(b, ACC_BG);
    return b;
}

lv_obj_t* secondaryButton(lv_obj_t* parent, int w, int h, const char* icon, const char* text) {
    return buttonBase(parent, w, h, icon, text, cSurf2(), cText(), true);
}

lv_obj_t* dangerButton(lv_obj_t* parent, int w, int h, const char* icon, const char* text) {
    return buttonBase(parent, w, h, icon, text, cCrit(), cText(), false);
}

lv_obj_t* iconButton(lv_obj_t* parent, int size, const char* icon, lv_color_t bg, lv_color_t fg) {
    lv_obj_t* b = box(parent, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    pressable(b);
    lv_obj_t* ic = label(b, size >= 80 ? F.icoL : F.icoM, fg, icon);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(ic);
    return b;
}

lv_obj_t* buttonLabel(lv_obj_t* btn) {
    if (!btn) return nullptr;
    uint32_t n = lv_obj_get_child_count(btn);
    return n ? lv_obj_get_child(btn, (int32_t)n - 1) : nullptr;
}

void setButtonText(lv_obj_t* btn, const char* text) {
    lv_obj_t* l = buttonLabel(btn);
    if (!l) return;
    setText(l, text);
    setHidden(l, !text || !text[0]);
}

void setButtonEnabled(lv_obj_t* btn, bool enabled) {
    if (!btn) return;
    bool isEnabled = !lv_obj_has_state(btn, LV_STATE_DISABLED);
    if (isEnabled == enabled) return;
    if (enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t* tabBar(lv_obj_t* parent, int x, int y, int w, const char* const* names, int n, int active,
                 lv_event_cb_t cb) {
    lv_obj_t* bar = box(parent, w, 32);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_style_radius(bar, 12, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, cSurf2(), 0);
    lv_obj_set_style_pad_all(bar, 3, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 3, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t* t = box(bar, 10, 26);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        pressable(t);
        if (cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* l = caption(t, names[i], cText());
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(l);
    }
    setTabActive(bar, active);
    return bar;
}

void setTabActive(lv_obj_t* bar, int active) {
    if (!bar) return;
    uint32_t n = lv_obj_get_child_count(bar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* t = lv_obj_get_child(bar, (int32_t)i);
        bool on = ((int)i == active);
        setBgColorIf(t, on ? accent() : cSurf2());
        lv_obj_t* l = lv_obj_get_child(t, 0);
        if (l) setTextColorIf(l, on ? cBg() : cText2());
    }
}

// ---------------------------------------------------------------- dialog
static lv_obj_t *sDlgBack = nullptr, *sDlgCard = nullptr, *sDlgTitle = nullptr, *sDlgMsg = nullptr;
static lv_obj_t *sDlgOk = nullptr, *sDlgCancel = nullptr;
static ConfirmCb sDlgCb = nullptr;
static void* sDlgUser = nullptr;

static void dialogAnswer(bool ok) {
    ConfirmCb cb = sDlgCb;
    void* user = sDlgUser;
    closeDialog();
    if (cb) cb(ok, user);
}

static void buildDialog() {
    sDlgBack = box(lv_layer_top(), SCR_W, SCR_H);
    lv_obj_set_pos(sDlgBack, 0, 0);
    lv_obj_set_style_bg_opa(sDlgBack, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(sDlgBack, cBg(), 0);
    lv_obj_add_flag(sDlgBack, LV_OBJ_FLAG_CLICKABLE);          // swallow touches
    sDlgCard = card(sDlgBack, CW, 260);
    lv_obj_set_style_bg_color(sDlgCard, cSurf2(), 0);
    lv_obj_center(sDlgCard);
    sDlgTitle = label(sDlgCard, F.body, cText(), "");
    lv_obj_set_width(sDlgTitle, CW - 40);
    lv_label_set_long_mode(sDlgTitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(sDlgTitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sDlgTitle, LV_ALIGN_TOP_MID, 0, 24);
    sDlgMsg = label(sDlgCard, F.small, cText2(), "");
    lv_obj_set_width(sDlgMsg, CW - 40);
    lv_label_set_long_mode(sDlgMsg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(sDlgMsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sDlgMsg, LV_ALIGN_TOP_MID, 0, 70);
    const int bw = (CW - 40 - 12) / 2;
    sDlgCancel = secondaryButton(sDlgCard, bw, 52, nullptr, "Abbrechen");
    lv_obj_align(sDlgCancel, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_add_event_cb(sDlgCancel, [](lv_event_t*) { haptic(HAPTIC_TICK); dialogAnswer(false); }, LV_EVENT_CLICKED, NULL);
    sDlgOk = primaryButton(sDlgCard, bw, 52, nullptr, "OK");
    lv_obj_align(sDlgOk, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_add_event_cb(sDlgOk, [](lv_event_t*) { haptic(HAPTIC_CLICK); dialogAnswer(true); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(sDlgBack, LV_OBJ_FLAG_HIDDEN);
}

void confirmDialog(const char* titleTxt, const char* msg, const char* okText, const char* cancelText,
                   ConfirmCb cb, void* user, bool danger) {
    if (!sDlgBack) buildDialog();
    sDlgCb = cb;
    sDlgUser = user;
    lv_label_set_text(sDlgTitle, titleTxt ? titleTxt : "");
    lv_label_set_text(sDlgMsg, msg ? msg : "");
    setButtonText(sDlgOk, okText ? okText : "OK");
    setButtonText(sDlgCancel, cancelText ? cancelText : "Abbrechen");
    setBgColorIf(sDlgOk, danger ? cCrit() : accent());
    lv_obj_t* okl = buttonLabel(sDlgOk);
    if (okl) setTextColorIf(okl, danger ? cText() : cBg());
    lv_obj_move_foreground(sDlgBack);
    setHidden(sDlgBack, false);
}

void closeDialog() {
    sDlgCb = nullptr;
    sDlgUser = nullptr;
    if (sDlgBack) setHidden(sDlgBack, true);
}

bool dialogOpen() { return sDlgBack && !lv_obj_has_flag(sDlgBack, LV_OBJ_FLAG_HIDDEN); }

// ---------------------------------------------------------------- setters
void setText(lv_obj_t* l, const char* s) {
    if (!l || !s) return;
    if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}
void setTextColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part) {
    if (o && !lv_color_eq(lv_obj_get_style_text_color(o, part), c)) lv_obj_set_style_text_color(o, c, part);
}
void setBgColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part) {
    if (o && !lv_color_eq(lv_obj_get_style_bg_color(o, part), c)) lv_obj_set_style_bg_color(o, c, part);
}
void setBgOpaIf(lv_obj_t* o, lv_opa_t opa, lv_part_t part) {
    if (o && lv_obj_get_style_bg_opa(o, part) != opa) lv_obj_set_style_bg_opa(o, opa, part);
}
void setArcColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part) {
    if (o && !lv_color_eq(lv_obj_get_style_arc_color(o, part), c)) lv_obj_set_style_arc_color(o, c, part);
}
void setBorderColorIf(lv_obj_t* o, lv_color_t c) {
    if (o && !lv_color_eq(lv_obj_get_style_border_color(o, LV_PART_MAIN), c)) lv_obj_set_style_border_color(o, c, 0);
}
void setBorderWidthIf(lv_obj_t* o, int32_t w) {
    if (o && lv_obj_get_style_border_width(o, LV_PART_MAIN) != w) lv_obj_set_style_border_width(o, w, 0);
}
void setArc(lv_obj_t* a, float frac) {
    if (!a) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int v = (int)(frac * 1000.0f + 0.5f);
    if (lv_arc_get_value(a) != v) lv_arc_set_value(a, v);
}
void setBar(lv_obj_t* bar, int32_t v) {
    if (bar && lv_bar_get_value(bar) != v) lv_bar_set_value(bar, v, LV_ANIM_OFF);
}
void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------- misc
void haptic(HapticPattern p) { MotionHaptic::triggerHaptic(p); }

void ago(char* out, size_t n, time_t t) {
    if (t <= 0) { snprintf(out, n, "nie"); return; }
    long d = (long)(time(NULL) - t);
    if (d < 0) d = 0;
    if (d < 60)          snprintf(out, n, "gerade eben");
    else if (d < 3600)   snprintf(out, n, "vor %ld min", d / 60);
    else if (d < 172800) snprintf(out, n, "vor %ld h", d / 3600);
    else                 snprintf(out, n, "vor %ld d", d / 86400);
}

void agoMs(char* out, size_t n, uint32_t sinceMs) {
    if (sinceMs == 0) { snprintf(out, n, "nie"); return; }
    uint32_t d = (millis() - sinceMs) / 1000;
    if (d < 60)        snprintf(out, n, "vor %lu s", (unsigned long)d);
    else if (d < 3600) snprintf(out, n, "vor %lu min", (unsigned long)(d / 60));
    else               snprintf(out, n, "vor %lu h", (unsigned long)(d / 3600));
}

const char* batterySymbol(int pct, bool chg) {
    if (chg) return ICO_BATTERY_CHARGING_FULL;
    if (pct <= 15) return ICO_BATTERY_ALERT;
    return ICO_BATTERY_FULL;
}

lv_color_t batteryColor(int pct, bool chg) {
    if (chg) return cOk();
    if (pct <= 15) return cCrit();
    if (pct <= 30) return cWarn();
    return cOk();
}

int daysInMonth(int year, int mon0) {
    static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = ((mon0 % 12) + 12) % 12;
    int d = md[m];
    if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) d = 29;
    return d;
}

}  // namespace ui
