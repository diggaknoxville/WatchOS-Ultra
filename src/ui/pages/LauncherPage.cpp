// App launcher (left of the watch face / side key): icon grid with labels.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "DisplayEngine.h"

using namespace ui;

namespace pages {

struct AppEntry {
    const char* name;
    const char* icon;
    uint32_t color;
    nav::PageId page;          // PG_COUNT = special action
    uint8_t action;            // 1 = torch
};

static const AppEntry APPS[] = {
    {"Zifferblatt",   ICO_WATCH,             0xFF9F0A, nav::PG_APP_FACES,     0},
    {"AI-Limits",     ICO_BOLT,              0x0A84FF, nav::PG_AI_LIMITS,     0},
    {"Kalender",      ICO_CALENDAR_MONTH,    0xFF453A, nav::PG_AGENDA,        0},
    {"Aktivität",     ICO_DIRECTIONS_RUN,    0x30D158, nav::PG_ACTIVITY,      0},
    {"Stoppuhr",      ICO_TIMER,             0xFF9F0A, nav::PG_APP_STOPWATCH, 0},
    {"Rechner",       ICO_GRID_VIEW,         0x30D158, nav::PG_APP_CALC,      0},
    {"Spiele",        ICO_SPORTS_SCORE,      0xFF2D55, nav::PG_APP_GAMES,     0},
    {"Position",      ICO_MY_LOCATION,       0x64D2FF, nav::PG_POSITION,      0},
    {"LoRa",          ICO_PODCASTS,          0xBF5AF2, nav::PG_LORA,          0},
    {"Mitteilungen",  ICO_NOTIFICATIONS,     0xFF375F, nav::PG_NOTIFS,        0},
    {"Medien",        ICO_PLAY_ARROW,        0xFF3B30, nav::PG_APP_MEDIA,     0},
    {"Verbinden",     ICO_SMARTPHONE,        0x5E5CE6, nav::PG_APP_CONNECT,   0},
    {"Taschenlampe",  ICO_FLASHLIGHT_ON,     0xFFD60A, nav::PG_COUNT,         1},
    {"System",        ICO_MEMORY,            0x8E8E93, nav::PG_SYSTEM,        0},
    {"Einstellungen", ICO_SETTINGS,          0x636366, nav::PG_APP_SETTINGS,  0},
    {"Kalibrierung",  ICO_TUNE,              0x00FFFF, nav::PG_APP_CALIB,     0},
    {"Energie",       ICO_POWER_SETTINGS_NEW,0xFF453A, nav::PG_APP_POWER,     0},
};
static const int N_APPS = sizeof(APPS) / sizeof(APPS[0]);

static void onApp(lv_event_t* e) {
    const AppEntry& a = APPS[(int)(intptr_t)lv_event_get_user_data(e)];
    haptic(HAPTIC_TICK);
    if (a.action == 1) { DisplayEngine::setTorch(true); return; }
    if (!nav::isCarousel(a.page) && a.page != nav::PG_NOTIFS && a.page != nav::PG_CONTROL) {
        nav::openApp(a.page);
        return;
    }
    nav::go(a.page);
}

void buildLauncher(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);
    title(p, "APPS");

    // Fixed viewport below the title; the tall grid inside it is moved by the
    // navigator's composited scroller (no LVGL scrolling, no per-frame redraw).
    lv_obj_t* view = box(p, SCR_W, SCR_H - CONTENT_Y);
    lv_obj_set_pos(view, 0, CONTENT_Y);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* grid = box(view, SCR_W, LV_SIZE_CONTENT);
    lv_obj_set_pos(grid, 0, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_hor(grid, 10, 0);
    lv_obj_set_style_pad_top(grid, 6, 0);
    lv_obj_set_style_pad_bottom(grid, 64, 0);

    for (int i = 0; i < N_APPS; i++) {
        const AppEntry& a = APPS[i];
        lv_obj_t* cell = box(grid, 120, 120);             // uniform tile
        pressable(cell);
        lv_obj_add_event_cb(cell, onApp, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* disc = box(cell, 84, 84);
        lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(disc, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(disc, lv_color_hex(a.color), 0);
        lv_obj_t* ic = label(disc, F.icoL, lv_color_white(), a.icon);
        lv_obj_center(ic);

        lv_obj_t* nm = label(cell, F.small, cText(), a.name);
        lv_obj_set_width(nm, 120);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    nav::PageDef d;
    d.root = p;
    d.refreshMs = 0xFFFF;
    d.ttlMs = 0;                 // static content
    d.scrollView = view;
    d.scrollContent = grid;
    nav::registerPage(nav::PG_LAUNCHER, d);
}

}  // namespace pages
