#include "ui/Navigator.h"
#include "ui/UiKit.h"
#include "Compositor.h"
#include "KronosDisplay.h"
#include "PowerManager.h"
#include <LilyGoLib.h>
#include <math.h>

namespace nav {

using Compositor::Surface;
using Compositor::Layer;

static const int W = Compositor::W;
static const int H = Compositor::H;
static const int SLOP = 10;                 // px before a drag is classified
static const int EDGE = 30;                 // edge zone for back / home gestures
static const float SPRING_RESPONSE = 0.18f; // s, snappy critically damped spring
static const float PROJECTION_S = 0.18f;    // velocity projection for commit decisions

enum Mode : uint8_t { M_CAROUSEL, M_SHEET_TOP, M_SHEET_BOTTOM, M_APP };

enum Gesture : uint8_t {
    G_NONE, G_PAGE, G_SHEET_TOP_OPEN, G_SHEET_TOP_CLOSE, G_SHEET_BOT_OPEN, G_SHEET_BOT_CLOSE,
    G_BACK, G_HOME, G_APP_OPEN
};

static const PageId CAROUSEL[] = {PG_LAUNCHER, PG_FACE, PG_AI_LIMITS, PG_ACTIVITY, PG_AGENDA, PG_POSITION, PG_LORA, PG_SYSTEM};
static const int N_CAROUSEL = sizeof(CAROUSEL) / sizeof(CAROUSEL[0]);

static PageDef sPages[PG_COUNT];
static Surface sSurf[PG_COUNT];
static bool sDirty[PG_COUNT];
static uint32_t sLastRefresh[PG_COUNT];

static Mode sMode = M_CAROUSEL;
static PageId sCur = PG_FACE;
static PageId sStack[8];                    // app stack (pages under the current app)
static int sDepth = 0;
static PageId sBase = PG_FACE;              // page underneath a sheet / app stack
static bool sBusy = false;
static bool sGestures = true;
static Surface sGlass;                      // capture of the glass at gesture start

// gesture tracking (fed by LVGL's touch sampling)
static bool sTracking = false;
static bool sTriggered = false;
static int16_t sX0 = 0, sY0 = 0;
static int16_t sLx = 0, sLy = 0;
static uint32_t sT0 = 0;
static uint32_t sLastTouchMs = 0, sLastPrerender = 0;

// ------------------------------------------------------------------ helpers
static bool registered(PageId id) { return id < PG_COUNT && sPages[id].root; }

bool isCarousel(PageId id) {
    for (int i = 0; i < N_CAROUSEL; i++) if (CAROUSEL[i] == id) return true;
    return false;
}

static int carouselIndex(PageId id) {
    for (int i = 0; i < N_CAROUSEL; i++) if (CAROUSEL[i] == id) return i;
    return -1;
}

static PageId neighbour(PageId id, int dir) {           // dir +1 = right, -1 = left
    int i = carouselIndex(id);
    if (i < 0) return PG_COUNT;
    for (i += dir; i >= 0 && i < N_CAROUSEL; i += dir) if (registered(CAROUSEL[i])) return CAROUSEL[i];
    // Seamless wrap-around from the last carousel screen (PG_SYSTEM) forward (+1) to the first main screen (PG_FACE),
    // while NOT wrapping backwards (-1) from PG_FACE/PG_LAUNCHER so the App Drawer stays on the left.
    if (dir > 0 && id == CAROUSEL[N_CAROUSEL - 1] && registered(PG_FACE)) {
        return PG_FACE;
    }
    return PG_COUNT;
}

static void refreshPage(PageId id, bool force) {
    if (registered(id) && sPages[id].refresh) sPages[id].refresh(force);
    sLastRefresh[id] = millis();
}

static bool surfaceFresh(PageId id) {
    const Surface& s = sSurf[id];
    if (!s.valid || sDirty[id]) return false;
    uint32_t ttl = sPages[id].ttlMs;
    return ttl == 0 || millis() - s.renderedAt < ttl;
}

static PageId sJobPage = PG_COUNT;                   // page being rendered in the background

static bool ensureSurface(PageId id) {
    if (!registered(id)) return false;
    if (sJobPage == id && Compositor::renderBusy()) {     // finish the background job
        while (!Compositor::renderStep()) {}
        sJobPage = PG_COUNT;
        sDirty[id] = false;
        return sSurf[id].valid;
    }
    if (surfaceFresh(id)) return true;
    if (Compositor::renderBusy()) { Compositor::renderCancel(); sJobPage = PG_COUNT; }
    refreshPage(id, true);                               // objects up to date before rendering
    bool ok = Compositor::render(sPages[id].root, sSurf[id]);
    if (ok) sDirty[id] = false;
    return ok;
}

// Make `id` the only visible page without letting LVGL redraw: the glass
// already shows exactly this page (last composited frame).
static void commitVisible(PageId id, const Surface* glass) {
    lv_display_t* d = lv_display_get_default();
    lv_display_enable_invalidation(d, false);
    for (int i = 0; i < PG_COUNT; i++) {
        if (registered((PageId)i)) ui::setHidden(sPages[i].root, i != id);
    }
    lv_display_enable_invalidation(d, true);
    if (glass && glass->valid) Compositor::setMirror(*glass);
}

static void setCurrent(PageId id, const Surface* glass) {
    PageId old = sCur;
    if (old != id && registered(old) && sPages[old].onHide) sPages[old].onHide();
    sCur = id;
    commitVisible(id, glass);
    if (registered(id) && sPages[id].onShow) sPages[id].onShow();
    refreshPage(id, true);                               // only real changes invalidate now
}

// ------------------------------------------------------------------ scenes
struct Scene {
    Gesture g;
    const Surface* from;     // what was on the glass when the gesture started
    const Surface* to;       // destination / the page being revealed
    int dir;                 // G_PAGE: +1 towards the right neighbour, -1 left; 0 = rubber band
    float full;              // travel distance for a complete transition
};

static int buildLayers(const Scene& sc, float pos, Layer* L) {
    float p = sc.full > 0 ? pos / sc.full : 0;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    int n = 0;
    switch (sc.g) {
        case G_PAGE: {
            int off = (int)lroundf(-sc.dir * pos);       // dir +1: content moves left
            if (sc.dir == 0) off = (int)lroundf(pos);
            L[n++] = {sc.from, off, 0, 256};
            if (sc.to && sc.dir) L[n++] = {sc.to, off + sc.dir * W, 0, 256};
            break;
        }
        case G_SHEET_TOP_OPEN:
            L[n++] = {sc.from, 0, 0, (uint16_t)(256 - 150 * p)};
            L[n++] = {sc.to, 0, (int)lroundf(pos) - H, 256};
            break;
        case G_SHEET_TOP_CLOSE:
            L[n++] = {sc.to, 0, 0, (uint16_t)(106 + 150 * p)};
            L[n++] = {sc.from, 0, -(int)lroundf(pos), 256};
            break;
        case G_SHEET_BOT_OPEN:
            L[n++] = {sc.from, 0, 0, (uint16_t)(256 - 150 * p)};
            L[n++] = {sc.to, 0, H - (int)lroundf(pos), 256};
            break;
        case G_SHEET_BOT_CLOSE:
            L[n++] = {sc.to, 0, 0, (uint16_t)(106 + 150 * p)};
            L[n++] = {sc.from, 0, (int)lroundf(pos), 256};
            break;
        case G_BACK:                                       // iOS-style parallax
            L[n++] = {sc.to, (int)lroundf(-0.3f * W * (1 - p)), 0, (uint16_t)(150 + 106 * p)};
            L[n++] = {sc.from, (int)lroundf(pos), 0, 256};
            break;
        case G_HOME:
            L[n++] = {sc.to, 0, 0, (uint16_t)(150 + 106 * p)};
            L[n++] = {sc.from, 0, -(int)lroundf(pos), 256};
            break;
        case G_APP_OPEN:
            L[n++] = {sc.from, 0, 0, (uint16_t)(256 - 120 * p)};
            L[n++] = {sc.to, 0, H - (int)lroundf(pos), 256};
            break;
        default:
            L[n++] = {sc.from, 0, 0, 256};
    }
    return n;
}

// transition statistics (printed after each transition)
static uint32_t sStatFrames = 0, sStatMaxUs = 0, sStatStartMs = 0;

static void frame(const Scene& sc, float pos) {
    Layer L[3];
    int n = buildLayers(sc, pos, L);
    Compositor::present(L, n, true);
    uint32_t f = Compositor::lastFrameUs();
    if (!sStatFrames) sStatStartMs = millis();
    sStatFrames++;
    if (f > sStatMaxUs) sStatMaxUs = f;
}

static void statsReport(const char* what, uint32_t prepUs) {
    uint32_t el = millis() - sStatStartMs;
    if (sStatFrames > 1 && el)
        Serial.printf("[NAV] %s: %lu frames %.1f fps, slowest %lu us, prep %lu us -> page %d\n", what,
                      (unsigned long)sStatFrames, sStatFrames * 1000.0f / el, (unsigned long)sStatMaxUs,
                      (unsigned long)prepUs, (int)sCur);
    sStatFrames = sStatMaxUs = 0;
}

// critically damped spring from (pos, vel) to target; vel in px/s
static void settle(const Scene& sc, float pos, float vel, float target) {
    const float w = 2.0f * (float)M_PI / SPRING_RESPONSE;
    float x0 = pos - target;
    if (fabsf(x0) < 2.0f) {
        frame(sc, target);
        return;
    }
    uint32_t t0 = micros();
    bool seenRelease = !KronosDisplay::lastTouch().pressed;
    for (;;) {
        float t = (micros() - t0) / 1e6f;
        float e = expf(-w * t);
        float x = (x0 + (vel + w * x0) * t) * e;
        // Stop immediately when within 3 px of target or upon crossing target (overshoot is clamped)
        if (fabsf(x) < 3.0f || (x0 * x <= 0.0f) || t > 0.30f) break;
        // Allow user to interrupt settle tail with a new touch for chained swipes
        KronosDisplay::Touch tc = KronosDisplay::pollTouch();
        if (!tc.pressed) seenRelease = true;
        else if (seenRelease && target > 0 && fabsf(x) < sc.full * 0.65f) break;
        frame(sc, target + x);
    }
    frame(sc, target);
}

// rubber band for pages without a neighbour: asymptotic, max ~90 px
static float rubber(float raw) {
    float s = raw < 0 ? -1.0f : 1.0f;
    return s * 90.0f * (1.0f - expf(-fabsf(raw) / 180.0f));
}

static float computeScenePos(const Scene& sc, float raw) {
    float pos = 0;
    switch (sc.g) {
        case G_PAGE:
            if (sc.dir == 0) pos = rubber(raw);
            else {
                pos = -sc.dir * raw;                        // progress towards the neighbour
                if (pos < 0) pos = -rubber(-pos) * 0.5f;    // dragging back past the start
                if (pos > W) pos = W;
            }
            break;
        case G_SHEET_TOP_OPEN: case G_SHEET_BOT_CLOSE: case G_BACK:
            pos = raw; break;
        case G_SHEET_TOP_CLOSE: case G_SHEET_BOT_OPEN: case G_HOME:
            pos = -raw; break;
        default: break;
    }
    if (sc.g != G_PAGE || sc.dir != 0) pos = pos < 0 ? 0 : (pos > sc.full ? sc.full : pos);
    return pos;
}

// Drive a gesture with the finger until release, then settle. Returns commit.
static bool interactive(Scene& sc, bool horizontal, int x0, int y0) {
    struct Sample { uint32_t ms; float pos; } hist[8];
    int hn = 0;
    // Seed history with touch-down (sT0, 0) and current finger position (sLx, sLy)
    // so fast flicks that release within 1-2 scan frames still have accurate velocity!
    hist[hn++] = {sT0, 0.0f};
    float raw0 = horizontal ? (float)(sLx - x0) : (float)(sLy - y0);
    float pos = computeScenePos(sc, raw0);
    uint32_t now0 = millis();
    if (now0 <= sT0) now0 = sT0 + 8;
    hist[hn++] = {now0, pos};

    float lastRenderedPos = -9999.0f;
    for (;;) {
        KronosDisplay::Touch t = KronosDisplay::pollTouch();
        if (!t.pressed) break;
        float raw = horizontal ? (float)(t.x - x0) : (float)(t.y - y0);
        pos = computeScenePos(sc, raw);
        if (fabsf(pos - hist[hn - 1].pos) >= 0.5f || (t.ms > hist[hn - 1].ms + 20)) {
            if (hn == 8) { memmove(hist, hist + 1, sizeof(Sample) * 7); hn = 7; }
            hist[hn++] = {t.ms > hist[hn - 1].ms ? t.ms : millis(), pos};
        }
        if (fabsf(pos - lastRenderedPos) >= 1.0f) {
            lastRenderedPos = pos;
            frame(sc, pos);
        } else {
            delay(1);
        }
    }
    // Release velocity: peak of the recent segment velocities. Averaging would
    // under-estimate flicks because the finger decelerates while lifting off.
    float vel = 0;
    for (int k = hn - 1; k > 0; k--) {
        uint32_t age = hist[hn - 1].ms - hist[k].ms;
        if (age > 120) break;
        int j = k - 1;
        while (j > 0 && hist[k].ms - hist[j].ms < 6) j--;
        uint32_t dt = hist[k].ms - hist[j].ms;
        if (dt == 0) dt = 8;
        float v = (hist[k].pos - hist[j].pos) * 1000.0f / dt;
        if (fabsf(v) > fabsf(vel)) vel = v;
    }
    // iOS-like commit: 22 % of the way, or a flick in the right direction
    bool commit = false;
    if (!(sc.g == G_PAGE && sc.dir == 0)) {
        float projected = pos + vel * PROJECTION_S;
        if (vel < -350.0f) commit = false;                            // flicked back
        else commit = pos > sc.full * 0.22f || (vel > 180.0f && pos > 5.0f) || projected > sc.full * 0.38f;
    }
    settle(sc, pos, vel, commit ? sc.full : 0);
    return commit;
}

// ------------------------------------------------------------------ actions
static void finishPage(PageId to, const Surface* glass) {
    sMode = M_CAROUSEL;
    setCurrent(to, glass);
}

static bool prepareGlass() {
    instance.waitPushComplete();
    bool ok = Compositor::captureMirror(sGlass);
    // Keep the cached surface of the page we are leaving 100% up-to-date in PSRAM
    if (ok && sCur < PG_COUNT && sGlass.valid && sGlass.words > 0) {
        Surface& dst = sSurf[sCur];
        if (dst.words < sGlass.words) {
            if (dst.data) heap_caps_free(dst.data);
            dst.data = (uint16_t*)heap_caps_malloc(sGlass.words * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
            dst.words = dst.data ? sGlass.words : 0;
        }
        if (dst.data) {
            memcpy(dst.data, sGlass.data, sGlass.words * sizeof(uint16_t));
            dst.valid = true;
            dst.renderedAt = millis();
            sDirty[sCur] = false;
        }
    }
    return ok;
}

// Take the touch away from LVGL: the pressed widget gets INDEV_RESET (no click)
// and loses its pressed look, and LVGL ignores the finger until release.
static void claimTouch(int x0, int y0) {
    lv_point_t pt = {(int32_t)x0, (int32_t)y0};
    for (lv_obj_t* o = lv_indev_search_obj(lv_screen_active(), &pt); o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_state(o, LV_STATE_PRESSED)) lv_obj_remove_state(o, LV_STATE_PRESSED);
    }
    lv_indev_t* in = KronosDisplay::touch();
    lv_indev_reset(in, NULL);
    lv_indev_wait_release(in);
}

static void runGesture(Gesture g, int dir, int x0, int y0, bool horizontal) {
    sBusy = true;
    uint32_t tPrep = micros();
    claimTouch(x0, y0);
    prepareGlass();
    Scene sc = {g, &sGlass, nullptr, dir, horizontal ? (float)W : (float)H};
    PageId target = PG_COUNT;
    switch (g) {
        case G_PAGE:
            target = dir ? neighbour(sCur, dir) : PG_COUNT;
            if (target != PG_COUNT && ensureSurface(target)) sc.to = &sSurf[target];
            else sc.dir = 0;
            break;
        case G_SHEET_TOP_OPEN:  target = PG_CONTROL; ensureSurface(target); sc.to = &sSurf[target]; break;
        case G_SHEET_BOT_OPEN:  target = PG_NOTIFS;  ensureSurface(target); sc.to = &sSurf[target]; break;
        case G_SHEET_TOP_CLOSE:
        case G_SHEET_BOT_CLOSE: target = sBase;      ensureSurface(target); sc.to = &sSurf[target]; break;
        case G_HOME:            target = PG_FACE;    ensureSurface(target); sc.to = &sSurf[target]; break;
        case G_BACK:
            target = sDepth > 0 ? sStack[sDepth - 1] : sBase;
            ensureSurface(target);
            sc.to = &sSurf[target];
            break;
        default: break;
    }
    tPrep = micros() - tPrep;
    bool commit = interactive(sc, horizontal, x0, y0);
    if (commit && target != PG_COUNT) {
        ui::haptic(HAPTIC_TICK);
        switch (g) {
            case G_PAGE:            finishPage(target, sc.to); break;
            case G_SHEET_TOP_OPEN:  sMode = M_SHEET_TOP; sBase = sCur; setCurrent(PG_CONTROL, sc.to); break;
            case G_SHEET_BOT_OPEN:  sMode = M_SHEET_BOTTOM; sBase = sCur; setCurrent(PG_NOTIFS, sc.to); break;
            case G_SHEET_TOP_CLOSE:
            case G_SHEET_BOT_CLOSE: finishPage(sBase, sc.to); break;
            case G_HOME:            sDepth = 0; finishPage(PG_FACE, sc.to); break;
            case G_BACK:
                if (sDepth > 0) { sDepth--; sMode = isCarousel(target) ? M_CAROUSEL : M_APP; }
                if (isCarousel(target)) sMode = M_CAROUSEL;
                setCurrent(target, sc.to);
                break;
            default: break;
        }
        sSurf[target].renderedAt = millis();                // keep target surface valid for fast chained swipes
    } else {
        Compositor::setMirror(sGlass);                      // glass back to the original page
    }
    statsReport(commit ? "gesture" : "gesture (cancelled)", tPrep);
    sBusy = false;
    sTracking = false;
    sTriggered = false;
    // If finger is already touching the screen (chained swipe), immediately begin tracking new gesture
    KronosDisplay::Touch tc = KronosDisplay::pollTouch();
    if (tc.pressed) {
        sTracking = true;
        sX0 = sLx = tc.x;
        sY0 = sLy = tc.y;
        sT0 = millis();
    }
}

// programmatic transition (no finger): animate 0 -> full with the spring
static void animate(Scene& sc) {
    settle(sc, 0, 0, sc.full);
    statsReport("animation", 0);
}

static void doOpenApp(PageId id);
static void doHome();

static void doGo(PageId id) {
    if (!registered(id) || sBusy || id == sCur) return;
    if (id == PG_CONTROL || id == PG_NOTIFS) {
        PageId under = (sMode == M_CAROUSEL || sMode == M_APP) ? sCur : sBase;
        sBusy = true;
        prepareGlass();
        ensureSurface(id);
        Scene sc = {id == PG_CONTROL ? G_SHEET_TOP_OPEN : G_SHEET_BOT_OPEN, &sGlass, &sSurf[id], 0, (float)H};
        animate(sc);
        sMode = id == PG_CONTROL ? M_SHEET_TOP : M_SHEET_BOTTOM;
        sBase = under;
        setCurrent(id, &sSurf[id]);
        sBusy = false;
        return;
    }
    if (!isCarousel(id)) { doOpenApp(id); return; }
    if (sMode != M_CAROUSEL) {                               // leave sheets/apps first
        sDepth = 0;
        sBusy = true;
        prepareGlass();
        ensureSurface(id);
        Scene sc = {G_HOME, &sGlass, &sSurf[id], 0, (float)H};
        animate(sc);
        finishPage(id, &sSurf[id]);
        sBusy = false;
        return;
    }
    int ci = carouselIndex(sCur), ti = carouselIndex(id);
    int dir = (sCur == CAROUSEL[N_CAROUSEL - 1] && id == PG_FACE) ? 1 : (ti > ci ? 1 : -1);
    sBusy = true;
    prepareGlass();
    ensureSurface(id);
    Scene sc = {G_PAGE, &sGlass, &sSurf[id], dir, (float)W};
    animate(sc);
    finishPage(id, &sSurf[id]);
    sBusy = false;
}

static void doOpenApp(PageId id) {
    if (!registered(id) || sBusy) return;
    sBusy = true;
    prepareGlass();
    ensureSurface(id);
    Scene sc = {G_APP_OPEN, &sGlass, &sSurf[id], 0, (float)H};
    animate(sc);
    if (sMode == M_APP) { if (sDepth < 8) sStack[sDepth++] = sCur; }
    else { sBase = sCur; sDepth = 0; }
    sMode = M_APP;
    setCurrent(id, &sSurf[id]);
    sBusy = false;
}

static void doBack() {
    if (sBusy) return;
    if (sMode == M_SHEET_TOP || sMode == M_SHEET_BOTTOM) {
        sBusy = true;
        prepareGlass();
        ensureSurface(sBase);
        Scene sc = {sMode == M_SHEET_TOP ? G_SHEET_TOP_CLOSE : G_SHEET_BOT_CLOSE, &sGlass, &sSurf[sBase], 0, (float)H};
        animate(sc);
        finishPage(sBase, &sSurf[sBase]);
        sBusy = false;
    } else if (sMode == M_APP) {
        PageId target = sDepth > 0 ? sStack[sDepth - 1] : sBase;
        sBusy = true;
        prepareGlass();
        ensureSurface(target);
        Scene sc = {G_BACK, &sGlass, &sSurf[target], 0, (float)W};
        animate(sc);
        if (sDepth > 0) sDepth--;
        sMode = (sDepth == 0 && isCarousel(target)) ? M_CAROUSEL : M_APP;
        setCurrent(target, &sSurf[target]);
        sBusy = false;
    }
}

static void doHome() {
    if (sBusy || sCur == PG_FACE) return;
    if (sMode == M_CAROUSEL) { doGo(PG_FACE); return; }
    sBusy = true;
    prepareGlass();
    ensureSurface(PG_FACE);
    Scene sc = {G_HOME, &sGlass, &sSurf[PG_FACE], 0, (float)H};
    animate(sc);
    sDepth = 0;
    finishPage(PG_FACE, &sSurf[PG_FACE]);
    sBusy = false;
}

static void doSideKey(bool longPress) {
    if (longPress) { doOpenApp(PG_APP_POWER); return; }
    if (sCur == PG_FACE) doGo(PG_LAUNCHER);
    else doHome();
}

// Defer programmatic navigation calls so they never run re-entrantly inside
// an LVGL event callback (e.g. LV_EVENT_CLICKED inside lv_timer_handler).
enum PendingAction : uint8_t {
    ACT_NONE = 0,
    ACT_GO,
    ACT_OPEN_APP,
    ACT_BACK,
    ACT_HOME,
    ACT_SIDE_SHORT,
    ACT_SIDE_LONG
};
static volatile PendingAction sPendingAction = ACT_NONE;
static volatile PageId sPendingPage = PG_FACE;

void go(PageId id)            { sPendingPage = id; sPendingAction = ACT_GO; }
void openApp(PageId id)       { sPendingPage = id; sPendingAction = ACT_OPEN_APP; }
void back()                   { sPendingAction = ACT_BACK; }
void home()                   { sPendingAction = ACT_HOME; }
void sideKey(bool longPress)  { sPendingAction = longPress ? ACT_SIDE_LONG : ACT_SIDE_SHORT; }

static void flushPendingAction() {
    if (sBusy || sPendingAction == ACT_NONE) return;
    PendingAction act = sPendingAction;
    PageId page = sPendingPage;
    sPendingAction = ACT_NONE;
    switch (act) {
        case ACT_GO:         doGo(page); break;
        case ACT_OPEN_APP:   doOpenApp(page); break;
        case ACT_BACK:       doBack(); break;
        case ACT_HOME:       doHome(); break;
        case ACT_SIDE_SHORT: doSideKey(false); break;
        case ACT_SIDE_LONG:  doSideKey(true); break;
        default: break;
    }
}

// ------------------------------------------------------------------ gestures
static Gesture classify(bool horizontal, int dx, int dy, int x0, int y0, int* dir) {
    *dir = 0;
    switch (sMode) {
        case M_CAROUSEL:
            if (horizontal) {
                // On PG_POSITION (Map), drag inside the map is reserved for smooth 2D panning!
                // Only edge swipes (near bezels x0 < 36 or x0 > W - 36) or outside the map card switch pages.
                if (sCur == PG_POSITION) {
                    bool insideMap = (y0 >= 72 && y0 <= 72 + 242 && x0 >= 14 && x0 <= W - 14);
                    if (insideMap && (x0 >= 36 && x0 <= W - 36)) {
                        return G_NONE; // Pass directly to map canvas drag & drop!
                    }
                }
                *dir = dx < 0 ? 1 : -1;
                if (neighbour(sCur, *dir) == PG_COUNT) *dir = 0;   // rubber band
                return G_PAGE;
            }
            if (sCur == PG_FACE) return dy > 0 ? G_SHEET_TOP_OPEN : G_SHEET_BOT_OPEN;
            // Allow pulling down Quick Menu (PG_CONTROL) from the top of EVERY screen!
            if (dy > 0) {
                int maxTopY = (sCur == PG_POSITION || sCur == PG_LAUNCHER) ? 72 : 160;
                if (y0 <= maxTopY) return G_SHEET_TOP_OPEN;
            }
            return G_NONE;                                          // let LVGL scroll content
        case M_SHEET_TOP:
            if (!horizontal && dy < 0) {
                const PageDef& p = sPages[PG_CONTROL];
                if (!p.allowSheetGesture || p.allowSheetGesture(x0, y0, dy)) return G_SHEET_TOP_CLOSE;
            }
            return G_NONE;
        case M_SHEET_BOTTOM:
            if (!horizontal && dy > 0) {
                const PageDef& p = sPages[PG_NOTIFS];
                if (!p.allowSheetGesture || p.allowSheetGesture(x0, y0, dy)) return G_SHEET_BOT_CLOSE;
            }
            return G_NONE;
        case M_APP:
            if (horizontal && dx > 0 && x0 < EDGE) return G_BACK;
            if (!horizontal && dy > 0 && y0 <= 72) return G_SHEET_TOP_OPEN;
            if (!horizontal && dy < 0 && y0 > H - EDGE - 10) return G_HOME;
            return G_NONE;
    }
    return G_NONE;
}

// Programmatic fallback for flicks that ended before the drag was classified.
static void flick(int dx, int dy) {
    bool horizontal = abs(dx) >= abs(dy);
    int dir;
    Gesture g = classify(horizontal, dx, dy, sX0, sY0, &dir);
    switch (g) {
        case G_PAGE:            if (dir) doGo(neighbour(sCur, dir)); break;
        case G_SHEET_TOP_OPEN:  doGo(PG_CONTROL); break;
        case G_SHEET_BOT_OPEN:  doGo(PG_NOTIFS); break;
        case G_SHEET_TOP_CLOSE:
        case G_SHEET_BOT_CLOSE:
        case G_BACK:            doBack(); break;
        case G_HOME:            doHome(); break;
        default: break;
    }
}

static void trackGestures() {
    KronosDisplay::Touch t = KronosDisplay::lastTouch();
    if (millis() - t.ms >= 8) {
        t = KronosDisplay::pollTouch();
    }
    if (t.pressed) {
        sLastTouchMs = millis();
        PowerManager::handleActivity();
    }
    if (!t.pressed) {
        // finger lifted before any gesture ran: a quick flick still navigates
        if (sTracking && !sTriggered && sGestures && !sBusy && millis() - sT0 < 450) {
            int dx = sLx - sX0, dy = sLy - sY0;
            if (abs(dx) > 42 || abs(dy) > 42) { sTriggered = true; flick(dx, dy); }
        }
        sTracking = false;
        sTriggered = false;
        return;
    }
    if (!sTracking) {
        sTracking = true;
        sTriggered = false;
        sX0 = sLx = t.x;
        sY0 = sLy = t.y;
        sT0 = millis();
        return;
    }
    sLx = t.x;
    sLy = t.y;
    if (sTriggered || !sGestures || sBusy) return;
    int dx = t.x - sX0, dy = t.y - sY0;
    int adx = abs(dx), ady = abs(dy), dist = adx > ady ? adx : ady;
    if (dist < SLOP) return;

    // Axis with tolerance: wrist swipes are rarely straight. The carousel takes
    // anything up to ~55 deg off horizontal; on the face, where both axes have
    // a meaning, ambiguous diagonals wait for more travel before deciding.
    bool horizontal;
    if (sMode == M_CAROUSEL && sCur == PG_FACE) {
        if (adx * 10 >= ady * 8) horizontal = true;
        else if (ady * 10 > adx * 13) horizontal = false;
        else if (dist < 40) return;
        else horizontal = adx > ady;
    } else {
        horizontal = adx * 10 >= ady * 7;
    }
    int dir;
    Gesture g = classify(horizontal, dx, dy, sX0, sY0, &dir);
    if (g == G_NONE) return;                 // keep evaluating while the finger moves
    sTriggered = true;
    runGesture(g, dir, sX0, sY0, horizontal);
}

// ------------------------------------------------------------------ prerender
// Neighbour pages are rendered in the background one strip per update pass, so
// a swipe can start at any moment without the UI task being blocked.
static void prerender(uint32_t now) {
    if (sBusy || KronosDisplay::lastTouch().pressed) return;
    if (Compositor::renderBusy()) {
        if (Compositor::renderStep()) {
            sDirty[sJobPage] = false;
            sJobPage = PG_COUNT;
        }
        return;
    }
    if (now - sLastTouchMs < 120 || now - sLastPrerender < 20) return;
    PageId want[6];
    int n = 0;
    if (sMode == M_CAROUSEL) {
        want[n++] = neighbour(sCur, 1);
        want[n++] = neighbour(sCur, -1);
        want[n++] = PG_CONTROL;
        if (sCur == PG_FACE) { want[n++] = PG_NOTIFS; }
    } else {
        want[n++] = PG_FACE;
        if (sMode == M_APP) want[n++] = sDepth > 0 ? sStack[sDepth - 1] : sBase;
    }
    for (int i = 0; i < n; i++) {
        PageId id = want[i];
        if (id >= PG_COUNT || !registered(id) || id == sCur || surfaceFresh(id)) continue;
        refreshPage(id, true);
        Compositor::renderBegin(sPages[id].root, sSurf[id]);
        sJobPage = id;
        sLastPrerender = now;
        return;
    }
}

// ------------------------------------------------------------------ public
void registerPage(PageId id, const PageDef& def) {
    if (id >= PG_COUNT) return;
    sPages[id] = def;
    // Keep all page surfaces permanently cached in PSRAM unless explicitly
    // invalidated via markDirty() or updated when leaving the active page.
    sPages[id].ttlMs = 0;
    sDirty[id] = true;
}

void begin(PageId initial) {
    sCur = initial;
    sMode = isCarousel(initial) ? M_CAROUSEL : M_APP;
    for (int i = 0; i < PG_COUNT; i++)
        if (registered((PageId)i)) ui::setHidden(sPages[i].root, i != initial);
    if (registered(initial) && sPages[initial].onShow) sPages[initial].onShow();
    refreshPage(initial, true);
}

void update(uint32_t now) {
    flushPendingAction();
    trackGestures();
    flushPendingAction();
    const PageDef& p = sPages[sCur];
    if (registered(sCur) && p.refresh && (p.refreshMs == 0 || now - sLastRefresh[sCur] >= p.refreshMs)) {
        p.refresh(false);
        sLastRefresh[sCur] = now;
    }
    prerender(now);
}

void warmup(void (*progress)(float)) {
    int total = 0, done = 0;
    for (int i = 0; i < PG_COUNT; i++) if (registered((PageId)i)) total++;
    for (int i = 0; i < PG_COUNT; i++) {
        if (!registered((PageId)i)) continue;
        refreshPage((PageId)i, true);
        if (Compositor::render(sPages[i].root, sSurf[i])) sDirty[i] = false;
        done++;
        if (progress) progress((float)done / total);
    }
}

void bootReveal(PageId initial) {
    sBusy = true;
    prepareGlass();
    ensureSurface(initial);
    Scene sc = {G_APP_OPEN, &sGlass, &sSurf[initial], 0, (float)H};
    animate(sc);
    sMode = isCarousel(initial) ? M_CAROUSEL : M_APP;
    setCurrent(initial, &sSurf[initial]);
    sBusy = false;
}

PageId current() { return sCur; }
bool transitioning() { return sBusy; }
void markDirty(PageId id) { if (id < PG_COUNT) sDirty[id] = true; }
void markAllDirty() { for (int i = 0; i < PG_COUNT; i++) sDirty[i] = true; }
void setGesturesEnabled(bool en) { sGestures = en; }

}  // namespace nav
