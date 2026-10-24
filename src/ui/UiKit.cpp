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

lv_color_t accent() {
    uint16_t c = SettingsStorage::get().accentColor;
    uint8_t r = ((c >> 11) & 0x1F) << 3; r |= r >> 5;
    uint8_t g = ((c >> 5) & 0x3F) << 2;  g |= g >> 6;
    uint8_t b = (c & 0x1F) << 3;         b |= b >> 5;
    return lv_color_make(r, g, b);
}

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
static AccRef sAcc[128];
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

// ---------------------------------------------------------------- setters
void setText(lv_obj_t* l, const char* s) {
    if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}
void setTextColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part) {
    if (!lv_color_eq(lv_obj_get_style_text_color(o, part), c)) lv_obj_set_style_text_color(o, c, part);
}
void setBgColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part) {
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, part), c)) lv_obj_set_style_bg_color(o, c, part);
}
void setArcColorIf(lv_obj_t* o, lv_color_t c, lv_part_t part) {
    if (!lv_color_eq(lv_obj_get_style_arc_color(o, part), c)) lv_obj_set_style_arc_color(o, c, part);
}
void setBorderWidthIf(lv_obj_t* o, int32_t w) {
    if (lv_obj_get_style_border_width(o, LV_PART_MAIN) != w) lv_obj_set_style_border_width(o, w, 0);
}
void setArc(lv_obj_t* a, float frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int v = (int)(frac * 1000.0f + 0.5f);
    if (lv_arc_get_value(a) != v) lv_arc_set_value(a, v);
}
void setHidden(lv_obj_t* o, bool hidden) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------- misc
void haptic(HapticPattern p) { MotionHaptic::triggerHaptic(p); }

void ago(char* out, size_t n, time_t t) {
    if (t <= 0) { snprintf(out, n, "never"); return; }
    long d = (long)(time(NULL) - t);
    if (d < 90)          snprintf(out, n, "just now");
    else if (d < 3600)   snprintf(out, n, "%ld min ago", d / 60);
    else if (d < 172800) snprintf(out, n, "%ld h ago", d / 3600);
    else                 snprintf(out, n, "%ld d ago", d / 86400);
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

}  // namespace ui
