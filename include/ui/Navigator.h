#pragma once
// ============================================================================
//  Navigator: page model, gesture recognition and composited transitions.
//
//  Information architecture (Wear OS / iOS inspired):
//    carousel  LAUNCHER <- FACE -> AI -> ACTIVITY -> AGENDA -> POSITION -> LORA -> SYSTEM
//    sheets    FACE: swipe down = CONTROL centre, swipe up = NOTIFICATIONS
//    apps      opened from the launcher, stacked; left-edge swipe = back,
//              bottom-edge swipe up = home; side key = home / launcher
//
//  Stack model: `base` is always a carousel page. Sheets and apps are pushed
//  on top of it in any order (an app opened from a sheet returns to the
//  sheet). The interaction mode is derived from the page type on top.
//
//  Every transition is rendered by the Compositor from cached run-length
//  encoded page surfaces (60 fps, vsync locked) and follows the finger 1:1,
//  settling with a critically damped spring.
// ============================================================================
#include <lvgl.h>
#include <stdint.h>

namespace nav {

enum PageId : uint8_t {
    // carousel, left -> right
    PG_LAUNCHER = 0, PG_FACE, PG_AI_LIMITS, PG_ACTIVITY, PG_AGENDA, PG_POSITION, PG_LORA, PG_SYSTEM,
    // sheets over the watch face
    PG_CONTROL, PG_NOTIFS,
    // apps
    PG_APP_CALENDAR, PG_APP_NAVI, PG_APP_STOPWATCH, PG_APP_LORA, PG_APP_CONNECT, PG_APP_SETTINGS,
    PG_APP_FACES, PG_APP_POWER, PG_APP_MEDIA, PG_APP_CALIB,
    PG_COUNT
};

// bottom edge zone (px) in which an upward swipe inside an app means "home"
constexpr int HOME_EDGE = 56;
// left edge zone (px) in which a rightward swipe inside an app means "back"
constexpr int BACK_EDGE = 30;

struct PageDef {
    lv_obj_t* root = nullptr;                 // full-screen container (ui::pageRoot)
    void (*refresh)(bool force) = nullptr;    // update dynamic content
    uint16_t refreshMs = 1000;                // while current; 0 = every frame
    uint32_t ttlMs = 15000;                   // cached surface lifetime (0 = until marked dirty)
    void (*onShow)() = nullptr;
    void (*onHide)() = nullptr;
    // optional: may a vertical sheet-close gesture start at (x, y)? (e.g. not on a slider)
    bool (*allowSheetGesture)(int x, int y, int dy) = nullptr;
    // optional: may a horizontal carousel swipe start at (x, y)? nullptr = always
    // (e.g. a map canvas keeps horizontal drags for panning)
    bool (*allowHorizontalGesture)(int x, int y) = nullptr;
};

void registerPage(PageId id, const PageDef& def);
void setRefreshInterval(PageId id, uint16_t ms);  // runtime override of PageDef::refreshMs
void begin(PageId initial);

// boot: render every page once (warms glyph caches + fills the surface cache),
// reporting progress 0..1, then reveal `initial` over whatever is on the glass
void warmup(void (*progress)(float));
void bootReveal(PageId initial);
// every UI loop pass. refreshPages = false (low power): only pending actions
// and touch tracking are serviced, no page refresh / background prerender.
void update(uint32_t now, bool refreshPages = true);

PageId current();
bool transitioning();
bool isCarousel(PageId id);

void go(PageId id);                           // animated navigation to any page
void openApp(PageId id);                      // push an app (slides up)
void back();                                  // pop app / close sheet
void home();                                  // back to the watch face
void sideKey(bool longPress);                 // hardware key

void markDirty(PageId id);                    // cached surface must be re-rendered
void markAllDirty();

void setGesturesEnabled(bool en);             // e.g. off while an overlay owns the screen
}
