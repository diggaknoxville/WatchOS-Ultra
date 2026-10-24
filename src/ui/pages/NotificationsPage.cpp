// Notification centre sheet (swipe up on the watch face): persistent history,
// newest first. Swipe down (with the list at the top) to close.
#include "ui/Pages.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "NotificationStore.h"

using namespace ui;

namespace pages {

static lv_obj_t *nList, *nEmpty, *nClear, *nCount;
static uint32_t nRev = 0;

static void rebuild() {
    lv_obj_clean(nList);
    uint8_t n = NotificationStore::count();
    setHidden(nEmpty, n != 0);
    setHidden(nClear, n == 0);
    char b[32];
    snprintf(b, sizeof(b), n == 1 ? "1 NOTIFICATION" : "%u NOTIFICATIONS", n);
    setText(nCount, n ? b : "");
    for (uint8_t i = 0; i < n; i++) {
        NotificationItem it;
        if (!NotificationStore::get(i, it)) break;
        lv_obj_t* c = card(nList, CW, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_set_style_pad_left(c, 62, 0);
        lv_obj_t* ic = label(c, F.icoS, accent(), it.icon[0] ? it.icon : ICO_NOTIFICATIONS);
        lv_obj_set_pos(ic, -44, 2);
        lv_obj_t* t = label(c, F.body, cText(), it.title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, CW - 62 - 16 - 70);
        lv_obj_set_pos(t, 0, -4);
        char a[24];
        ago(a, sizeof(a), it.epoch);
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
}

static void refresh(bool force) {
    if (force || NotificationStore::revision() != nRev) rebuild();
}

static bool allowGesture(int, int, int) { return lv_obj_get_scroll_y(nList) <= 0; }

void buildNotifications(lv_obj_t* scr) {
    lv_obj_t* p = pageRoot(scr);

    lv_obj_t* grab = box(p, 40, 5);                       // swipe-down affordance
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, cText3(), 0);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 10);

    title(p, "NOTIFICATIONS");
    nCount = caption(p, "", cText3());
    lv_obj_set_pos(nCount, M, 80);

    nClear = lv_button_create(p);
    lv_obj_set_size(nClear, 96, 34);
    lv_obj_align(nClear, LV_ALIGN_TOP_RIGHT, -M, 26);
    lv_obj_set_style_radius(nClear, 17, 0);
    lv_obj_set_style_shadow_width(nClear, 0, 0);
    lv_obj_set_style_bg_color(nClear, cSurf2(), 0);
    lv_obj_t* cl = caption(nClear, "CLEAR", cText2());
    lv_obj_center(cl);
    lv_obj_add_event_cb(nClear, [](lv_event_t*) {
        NotificationStore::clear();
        haptic(HAPTIC_CLICK);
    }, LV_EVENT_CLICKED, NULL);

    nList = box(p, SCR_W, SCR_H - 108);
    lv_obj_set_pos(nList, 0, 108);
    lv_obj_add_flag(nList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(nList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(nList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(nList, 10, 0);
    lv_obj_set_style_pad_bottom(nList, 24, 0);

    nEmpty = box(p, SCR_W, 200);
    lv_obj_align(nEmpty, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t* ei = label(nEmpty, F.icoXL, cText3(), ICO_NOTIFICATIONS_OFF);
    lv_obj_align(ei, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t* et = label(nEmpty, F.body, cText2(), "You're all caught up");
    lv_obj_align(et, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_t* es = caption(nEmpty, "NO NOTIFICATIONS", cText3());
    lv_obj_align(es, LV_ALIGN_TOP_MID, 0, 116);

    nav::PageDef d;
    d.root = p;
    d.refresh = refresh;
    d.refreshMs = 1000;
    d.ttlMs = 60000;
    d.onShow = [] { NotificationStore::markAllRead(); };
    d.allowSheetGesture = allowGesture;
    nav::registerPage(nav::PG_NOTIFS, d);
    rebuild();
}

}  // namespace pages
