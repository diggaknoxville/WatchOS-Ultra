// Notification centre sheet (swipe up on the watch face): persistent history,
// newest first. Swipe down (with the list at the top) to close.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "NotificationStore.h"

using namespace ui;

namespace pages {

static lv_obj_t *nList, *nEmpty, *nClear, *nRead, *nCount;
static uint32_t nRev = 0;
static uint8_t nBuiltCount = 0;
static time_t nBuiltAt = 0;

static void rebuild() {
    lv_obj_clean(nList);
    uint8_t n = NotificationStore::count();
    uint8_t unread = NotificationStore::unread();
    setHidden(nEmpty, n != 0);
    setHidden(nClear, n == 0);
    setHidden(nRead, unread == 0);
    char b[40];
    if (n == 0) b[0] = 0;
    else if (unread) snprintf(b, sizeof(b), "%u MITTEILUNGEN · %u NEU", (unsigned)n, (unsigned)unread);
    else if (n == 1) snprintf(b, sizeof(b), "1 MITTEILUNG");
    else snprintf(b, sizeof(b), "%u MITTEILUNGEN", (unsigned)n);
    setText(nCount, b);
    for (uint8_t i = 0; i < n; i++) {
        NotificationItem it;
        if (!NotificationStore::get(i, it)) break;
        lv_obj_t* c = card(nList, CW, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(c, R_TILE, 0);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_set_style_pad_left(c, 62, 0);
        if (it.unread) lv_obj_set_style_border_color(c, accent(), 0);   // transient: no accReg (deleted on rebuild)
        lv_obj_t* ic = label(c, F.icoS, it.unread ? accent() : cText3(), it.icon[0] ? it.icon : ICO_NOTIFICATIONS);
        lv_obj_set_pos(ic, -44, 2);
        lv_obj_t* t = label(c, F.body, cText(), it.title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, CW - 62 - 16 - 90);
        lv_obj_set_pos(t, 0, -4);
        char a[24];
        ago(a, sizeof(a), (time_t)it.epoch);
        lv_obj_t* tm = caption(c, a, cText3());
        lv_obj_align(tm, LV_ALIGN_TOP_RIGHT, 0, 2);
        if (it.body[0]) {
            lv_obj_t* m = label(c, F.small, cText2(), it.body);
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(m, CW - 62 - 16);
            lv_obj_set_pos(m, 0, 28);
        }
    }
    nRev = NotificationStore::revision();
    nBuiltCount = n;
    nBuiltAt = time(NULL);
}

static void refresh(bool force) {
    // relative timestamps age: rebuild once a minute while there are entries
    bool stale = nBuiltCount && (time(NULL) - nBuiltAt >= 60);
    if (force || stale || NotificationStore::revision() != nRev) rebuild();
}

static bool allowGesture(int, int, int) { return lv_obj_get_scroll_y(nList) <= 0; }

void buildNotifications(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);

    lv_obj_t* grab = box(p, 40, 5);                       // swipe-down affordance
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, cText3(), 0);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 14);

    title(p, "MITTEILUNGEN");
    nCount = caption(p, "", cText3());
    lv_obj_set_pos(nCount, M, CONTENT_Y);

    nRead = secondaryButton(p, 110, 32, ICO_CHECK, "Gelesen");
    lv_obj_align(nRead, LV_ALIGN_TOP_RIGHT, -M_HEAD, 26);
    lv_obj_add_event_cb(nRead, [](lv_event_t*) {
        NotificationStore::markAllRead();
        haptic(HAPTIC_TICK);
        nav::markDirty(nav::PG_FACE);
        rebuild();
    }, LV_EVENT_CLICKED, NULL);

    nClear = secondaryButton(p, 90, 30, ICO_DELETE, "Leeren");
    lv_obj_align(nClear, LV_ALIGN_TOP_RIGHT, -M, CONTENT_Y - 6);
    lv_obj_add_event_cb(nClear, [](lv_event_t*) {
        NotificationStore::clear();
        haptic(HAPTIC_TICK);
        nav::markDirty(nav::PG_FACE);
        rebuild();
    }, LV_EVENT_CLICKED, NULL);

    nList = box(p, SCR_W, SCR_H - (CONTENT_Y + 32));
    lv_obj_set_pos(nList, 0, CONTENT_Y + 32);
    lv_obj_add_flag(nList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(nList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(nList, 10, 0);
    lv_obj_set_style_pad_bottom(nList, 24, 0);

    nEmpty = emptyState(p, ICO_NOTIFICATIONS_OFF, "Alles gelesen", "KEINE MITTEILUNGEN");
    lv_obj_align(nEmpty, LV_ALIGN_CENTER, 0, 30);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 60000;
    d.onShow = [] { NotificationStore::markAllRead(); nav::markDirty(nav::PG_FACE); };
    d.allowSheetGesture = allowGesture;
    nav::registerPage(nav::PG_NOTIFS, d);
    rebuild();
}

}  // namespace pages
