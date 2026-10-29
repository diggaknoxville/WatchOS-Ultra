// ============================================================================
//  WatchOS Ultra UI facade: pages, overlays, hardware events, debug console.
// ============================================================================
#include "DisplayEngine.h"
#include "ui/UiKit.h"
#include "ui/Navigator.h"
#include "ui/Pages.h"
#include "Compositor.h"
#include "KronosDisplay.h"
#include "NotificationStore.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "MotionHaptic.h"
#include "AudioEngine.h"
#include "NavEngine.h"
#include "RadioLoRa.h"
#include "CompanionServer.h"
#include "AiLimitsStore.h"
#include <LilyGoLib.h>
#include <SD.h>
#include "ff.h"
#include <errno.h>
#include <esp_heap_caps.h>
#include <sys/time.h>

using namespace ui;

// ------------------------------------------------------------------ overlays
static lv_obj_t *sBanner, *sBannerIcon, *sBannerTitle, *sBannerMsg;
static uint32_t sBannerHideAt = 0;
static lv_obj_t *sAod, *sAodTime, *sAodDate;
static lv_obj_t *sTorch;
static uint16_t sLastAccent = 0xFFFF;
static bool sReady = false;
static bool sDnd = false;
static bool sPanelAsleep = false;              // AMOLED in sleep-in (POWER_SLEEP)

// cross-task banner inbox (latest wins; history lives in NotificationStore)
static portMUX_TYPE sInboxMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool sInPending = false;
static bool sInStored = false;                 // true = notification, false = toast
static char sInTitle[40], sInMsg[112], sInIcon[4];
static uint32_t sInDuration = 4500;

static void bannerAnimY(void* o, int32_t v) { lv_obj_set_y((lv_obj_t*)o, v); }

static void bannerSlide(bool in) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sBanner);
    lv_anim_set_exec_cb(&a, bannerAnimY);
    lv_anim_set_values(&a, lv_obj_get_y(sBanner), in ? 28 : -180);
    lv_anim_set_duration(&a, in ? 340 : 260);
    lv_anim_set_path_cb(&a, in ? lv_anim_path_overshoot : lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void buildBanner() {
    sBanner = card(lv_layer_top(), CW_HEAD, 118);
    lv_obj_set_pos(sBanner, M_HEAD, -180);
    lv_obj_set_style_bg_color(sBanner, cSurf2(), 0);
    lv_obj_set_style_radius(sBanner, R_CARD, 0);
    lv_obj_add_flag(sBanner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* disc = box(sBanner, 40, 40);
    lv_obj_set_pos(disc, 14, 14);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(disc, accent(), 0);
    accReg(disc, ACC_BG);
    sBannerIcon = label(disc, F.icoS, cBg(), ICO_NOTIFICATIONS);
    lv_obj_center(sBannerIcon);
    sBannerTitle = label(sBanner, F.body, cText(), "");
    lv_label_set_long_mode(sBannerTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sBannerTitle, CW_HEAD - 76);
    lv_obj_set_pos(sBannerTitle, 66, 12);
    sBannerMsg = label(sBanner, F.small, cText2(), "");
    lv_label_set_long_mode(sBannerMsg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sBannerMsg, CW_HEAD - 76);
    lv_obj_set_pos(sBannerMsg, 66, 44);
    lv_obj_add_event_cb(sBanner, [](lv_event_t*) {
        sBannerHideAt = 0;
        bannerSlide(false);
        nav::go(nav::PG_NOTIFS);
    }, LV_EVENT_CLICKED, NULL);
}

// AOD pixel shift (burn-in): the clock wanders +-4 px every 5 minutes
static const int8_t AOD_SHIFT[][2] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {-4, 4}, {-4, 0}, {-4, -4}, {0, -4}, {4, -4}};
static const int AOD_SHIFT_N = sizeof(AOD_SHIFT) / sizeof(AOD_SHIFT[0]);

static void buildAodAndTorch() {
    sAod = box(lv_layer_top(), SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(sAod, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sAod, cBg(), 0);
    lv_obj_add_flag(sAod, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sAod, [](lv_event_t*) { PowerManager::handleActivity(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(sAod, [](lv_event_t*) { PowerManager::handleActivity(); }, LV_EVENT_PRESSED, NULL);
    sAodTime = label(sAod, F.hero, cText3(), "00:00");
    lv_obj_align(sAodTime, LV_ALIGN_CENTER, 0, -20);
    sAodDate = caption(sAod, "", cText3());
    lv_obj_align(sAodDate, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_flag(sAod, LV_OBJ_FLAG_HIDDEN);

    sTorch = box(lv_layer_top(), SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(sTorch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sTorch, lv_color_white(), 0);
    lv_obj_add_flag(sTorch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* th = caption(sTorch, "TIPPEN ZUM AUSSCHALTEN", lv_color_hex(0x9AA0A8));
    lv_obj_align(th, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(sTorch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(sTorch, [](lv_event_t*) { DisplayEngine::setTorch(false); }, LV_EVENT_CLICKED, NULL);
}

// AOD clock: text changes once per minute (plus on show), position every 5 min
static void refreshAod() {
    static int lastMin = -1;
    static uint32_t lastShiftMs = 0;
    static int shiftIdx = 0;
    bool aod = PowerManager::getPowerState() == POWER_AOD;
    bool shown = !lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN);
    if (aod != shown) {
        if (aod) setHidden(sAod, false);
        if (aod) {
            lv_obj_move_foreground(sAod);
            lastMin = -1;                                   // force a clock update on show
        } else {
            // Leaving AOD: hide the overlay without letting LVGL repaint the
            // screen strip by strip; the navigator presents the fresh page at once.
            lv_display_t* d = lv_display_get_default();
            lv_display_enable_invalidation(d, false);
            setHidden(sAod, true);
            lv_display_enable_invalidation(d, true);
            nav::repaint();
        }
    }
    if (!aod) return;
    struct tm t;
    RtcManager::getLocalTimeInfo(&t);
    uint32_t now = millis();
    bool shift = false;
    if (now - lastShiftMs >= 5UL * 60UL * 1000UL) {
        lastShiftMs = now;
        shiftIdx = (shiftIdx + 1) % AOD_SHIFT_N;
        shift = true;
    }
    if (t.tm_min == lastMin && !shift) return;
    lastMin = t.tm_min;
    char b[32];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    setText(sAodTime, b);
    snprintf(b, sizeof(b), "%s %d. %s", WD_DE_S[t.tm_wday % 7], t.tm_mday, MO_DE_S[t.tm_mon % 12]);
    setText(sAodDate, b);
    int dx = AOD_SHIFT[shiftIdx][0], dy = AOD_SHIFT[shiftIdx][1];
    lv_obj_align(sAodTime, LV_ALIGN_CENTER, dx, -20 + dy);
    lv_obj_align(sAodDate, LV_ALIGN_CENTER, dx, 70 + dy);
}

static void enqueueBanner(const char* title, const char* msg, uint32_t durationMs, const char* icon, bool stored) {
    portENTER_CRITICAL(&sInboxMux);
    strncpy(sInTitle, title ? title : "", sizeof(sInTitle) - 1);
    sInTitle[sizeof(sInTitle) - 1] = 0;
    strncpy(sInMsg, msg ? msg : "", sizeof(sInMsg) - 1);
    sInMsg[sizeof(sInMsg) - 1] = 0;
    strncpy(sInIcon, icon ? icon : "", sizeof(sInIcon) - 1);
    sInIcon[sizeof(sInIcon) - 1] = 0;
    sInDuration = durationMs;
    sInStored = stored;
    sInPending = true;
    portEXIT_CRITICAL(&sInboxMux);
}

static void pollInbox() {
    if (sInPending) {
        char title[40], msg[112], icon[4];
        uint32_t dur;
        bool stored;
        portENTER_CRITICAL(&sInboxMux);
        memcpy(title, sInTitle, sizeof(title));
        memcpy(msg, sInMsg, sizeof(msg));
        memcpy(icon, sInIcon, sizeof(icon));
        dur = sInDuration;
        stored = sInStored;
        sInPending = false;
        portEXIT_CRITICAL(&sInboxMux);
        if (stored) {
            nav::markDirty(nav::PG_NOTIFS);
            nav::markDirty(nav::PG_FACE);                   // unread badge
        }
        if (!(stored && sDnd)) {                            // DND: no banner for notifications
            lv_label_set_text(sBannerTitle, title);
            lv_label_set_text(sBannerMsg, msg);
            lv_label_set_text(sBannerIcon, icon[0] ? icon : ICO_NOTIFICATIONS);
            lv_obj_move_foreground(sBanner);
            bannerSlide(true);
            sBannerHideAt = millis() + dur;
            PowerManager::handleActivity();
        }
    }
    if (sBannerHideAt && millis() > sBannerHideAt) {
        sBannerHideAt = 0;
        bannerSlide(false);
    }
}

// ------------------------------------------------------------------ OTA confirm
static bool sOtaDialogShown = false;

static void otaAnswer(bool ok, void*) {
    sOtaDialogShown = false;
    CompanionServer::otaConfirm(ok);
    if (!ok) DisplayEngine::showToast("Firmware-Update", "Verworfen", 2000, ICO_UPDATE);
}

static void pollOta(unsigned long now) {
    static unsigned long last = 0;
    if (now - last < 500) return;
    last = now;
    char hash[72] = {0};
    uint32_t size = 0;
    bool pending = CompanionServer::otaPending(hash, sizeof(hash), &size);
    if (pending && !sOtaDialogShown && !dialogOpen()) {
        char prefix[12];
        strncpy(prefix, hash, 8);
        prefix[8] = 0;
        char msg[96];
        snprintf(msg, sizeof(msg), "SHA-256 %s...\n%lu KB empfangen. Die Uhr startet danach neu.",
                 prefix, (unsigned long)((size + 1023) / 1024));
        sOtaDialogShown = true;
        PowerManager::handleActivity();
        haptic(HAPTIC_ALERT_SHORT);
        confirmDialog("Firmware-Update installieren?", msg, "Installieren", "Abbrechen", otaAnswer, nullptr, false);
    } else if (!pending && sOtaDialogShown) {
        sOtaDialogShown = false;                            // timed out on the server side
        closeDialog();
        DisplayEngine::showToast("Firmware-Update", "Abgelaufen", 2000, ICO_UPDATE);
    }
}

// ------------------------------------------------------------------ perf / debug
extern volatile uint32_t g_kronosFlushUs;
extern volatile uint32_t g_kronosFlushPx;
static bool sPerfOn = false;
static uint32_t sPerfFrames = 0, sPerfCycleUs = 0, sPerfStartUs = 0, sPerfMaxUs = 0;

static void perfRenderEvent(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        sPerfStartUs = micros();
    } else {
        uint32_t d = micros() - sPerfStartUs;
        sPerfCycleUs += d;
        if (d > sPerfMaxUs) sPerfMaxUs = d;
        sPerfFrames++;
    }
}

static void perfReport(unsigned long now) {
    static unsigned long last = 0;
    if (!sPerfOn || now - last < 1000) return;
    uint32_t span = now - last;
    last = now;
    if (sPerfFrames) {
        uint32_t cyc = sPerfCycleUs / sPerfFrames, fl = g_kronosFlushUs / sPerfFrames;
        Serial.printf("[PERF] %2lu fps | frame %5lu us (max %6lu) | flush %5lu us | render %5lu us | %lu kpx/frame\n",
                      (unsigned long)(sPerfFrames * 1000 / span), (unsigned long)cyc, (unsigned long)sPerfMaxUs,
                      (unsigned long)fl, (unsigned long)(cyc > fl ? cyc - fl : 0),
                      (unsigned long)(g_kronosFlushPx / sPerfFrames / 1000));
    }
    sPerfFrames = sPerfCycleUs = sPerfMaxUs = 0;
    g_kronosFlushUs = 0;
    g_kronosFlushPx = 0;
}

// Serial console (UI task):
//   S snapshot | G<page> go | F<n> face | T<epoch>,<tz>[,<zone>] time | N<title>|<msg>
//   P perf toggle | K / L side key click / long | B panel throughput | M<place> navigate
//   A<json> push AI limits | T<ep,tz,zone> sync time
static void debugConsole() {
    static char line[512];
    static uint16_t len = 0;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch != '\n' && ch != '\r') { if (len < sizeof(line) - 1) line[len++] = ch; continue; }
        if (!len) continue;
        line[len] = 0;
        len = 0;
        PowerManager::handleActivity();
        refreshAod();
        switch (line[0]) {
            case 'S': {
                lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
                if (!snap) { Serial.println("SNAP_FAIL"); break; }
                Serial.printf("SNAP %u %u %u\n", (unsigned)snap->header.w, (unsigned)snap->header.h,
                              (unsigned)snap->header.stride);
                size_t total = (size_t)snap->header.stride * snap->header.h;
                const uint8_t* p = (const uint8_t*)snap->data;
                size_t sent = 0;
                unsigned long writeStart = millis();
                while (sent < total) {
                    int avail = Serial.availableForWrite();
                    if (avail <= 0) {
                        vTaskDelay(pdMS_TO_TICKS(2));
                        if (millis() - writeStart > 5000) break;
                        continue;
                    }
                    size_t chunk = (total - sent > (size_t)avail) ? (size_t)avail : (total - sent);
                    size_t n = Serial.write(p + sent, chunk);
                    if (n == 0) {
                        vTaskDelay(pdMS_TO_TICKS(2));
                        if (millis() - writeStart > 5000) break;
                        continue;
                    }
                    sent += n;
                    writeStart = millis();
                }
                Serial.print("\nEND\n");
                Serial.flush();
                lv_draw_buf_destroy(snap);
                break;
            }
            case 'G': {
                int pg = atoi(line + 1);
                if (pg >= 0 && pg < (int)nav::PG_COUNT) {
                    if (pg == (int)nav::PG_CONTROL || pg == (int)nav::PG_NOTIFS || !nav::isCarousel((nav::PageId)pg)) {
                        nav::openApp((nav::PageId)pg);
                    } else {
                        nav::go((nav::PageId)pg);
                    }
                }
                Serial.printf("PAGE %d\n", pg);
                Serial.flush();
                break;
            }
            case 'F': {
                int v = atoi(line + 1);
                if (v < 0) v = 0;
                v %= FACE_COUNT;
                pages::faceSelect(v);
                Serial.printf("FACE %d\n", v);
                Serial.flush();
                break;
            }
            case 'T': {
                long ep = 0, tz = 0;
                char zone[40] = {0};
                sscanf(line + 1, "%ld,%ld,%39s", &ep, &tz, zone);
                RtcManager::syncFromEpoch((time_t)ep, (int32_t)tz, zone[0] ? zone : nullptr);
                RtcManager::noteSyncSource("USB");
                Serial.printf("TIME_SET %ld tz=%ld zone=%s\n", ep, tz, zone);
                Serial.flush();
                break;
            }
            case 'A': {
                bool ok = AiLimitsStore::updateFromJson(line + 1);
                nav::markDirty(nav::PG_AI_LIMITS);
                Serial.printf("AI_LIMITS_SET ok=%d\n", (int)ok);
                Serial.flush();
                break;
            }
            case 'N': {
                char* sep = strchr(line + 1, '|');
                if (sep) { *sep = 0; DisplayEngine::showNotification(line + 1, sep + 1); }
                break;
            }
            case 'P':
                sPerfOn = !sPerfOn;
                Serial.printf("PERF %s\n", sPerfOn ? "on" : "off");
                break;
            case 'D': {   // D<x0>,<y0>,<x1>,<y1>,<ms>: synthetic swipe
                int x0, y0, x1, y1, ms;
                if (sscanf(line + 1, "%d,%d,%d,%d,%d", &x0, &y0, &x1, &y1, &ms) == 5) {
                    KronosDisplay::injectSwipe(x0, y0, x1, y1, ms);
                    Serial.println("SWIPE");
                }
                break;
            }
            case 'K': if (!pages::closeRingsDetailIfOpen()) nav::sideKey(false); Serial.printf("KEY page %d\n", (int)nav::current()); break;
            case 'L': nav::sideKey(true);  Serial.printf("KEY page %d\n", (int)nav::current()); break;
            case 'I': {
                NavStatus st = NavEngine::status();
                Serial.printf("SD_INFO ready=%d total=%luMB used=%luMB map=%d places=%lu nodes=%lu heap=%uKB\n",
                              (int)st.sdReady, (unsigned long)st.sdTotalMB, (unsigned long)st.sdUsedMB,
                              (int)st.mapLoaded, (unsigned long)st.mapPlaces, (unsigned long)st.mapRouteNodes,
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
                File root = SD.open("/");
                if (root) {
                    for (int n = 0; n < 32; n++) {
                        File f = root.openNextFile();
                        if (!f) break;
                        Serial.printf("  %s %s (%u B)\n", f.isDirectory() ? "DIR " : "FILE", f.path(), (unsigned)f.size());
                        f.close();
                    }
                    root.close();
                }
                break;
            }
            case 'M': { // M<city_or_@mesh>: start navigation to matching place or mesh node
                static NavPlace pl[4];
                int n = NavEngine::searchPlaces(line + 1, pl, 4);
                if (n > 0) {
                    if (pl[0].rank == 0) NavEngine::setMode(NAV_MODE_MESH);
                    bool accepted = NavEngine::startNavigation(pl[0].name, pl[0].lat, pl[0].lon);
                    Serial.printf("NAV_%s %s %.4f,%.4f (status via I)\n", accepted ? "QUEUED" : "BUSY",
                                  pl[0].name, pl[0].lat, pl[0].lon);
                } else {
                    Serial.println("NAV_NOT_FOUND");
                }
                break;
            }
            case 'U': { // U<total_bytes>: upload /kronos/germany.knav to SD in 4096-byte chunks
                uint32_t total = (uint32_t)strtoul(line + 1, nullptr, 10);
                if (total == 0 || !NavEngine::beginSdUpload("/kronos/germany.knav", total)) {
                    Serial.println("UP_ERR_OPEN");
                    break;
                }
                Serial.println("UP_READY");
                uint8_t* cbuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
                uint32_t recvd = 0;
                unsigned long lastByteAt = millis();
                bool ok = (cbuf != nullptr);
                while (ok && recvd < total && (millis() - lastByteAt < 5000)) {
                    uint32_t want = min<uint32_t>(4096, total - recvd);
                    uint32_t got = 0;
                    while (got < want && (millis() - lastByteAt < 5000)) {
                        int avail = Serial.available();
                        if (avail > 0) {
                            int rd = Serial.readBytes(cbuf + got, min<int>(avail, want - got));
                            if (rd > 0) {
                                got += (uint32_t)rd;
                                lastByteAt = millis();
                            }
                        } else {
                            vTaskDelay(1);
                        }
                    }
                    if (got != want || !NavEngine::writeSdUploadChunk(cbuf, got)) {
                        ok = false;
                        break;
                    }
                    recvd += got;
                    Serial.println("K");
                }
                if (cbuf) heap_caps_free(cbuf);
                bool mapOk = NavEngine::endSdUpload();
                Serial.printf("UP_DONE ok=%d recvd=%lu map=%d\n", (int)ok, (unsigned long)recvd, (int)mapOk);
                break;
            }
            case 'B': {
                const int rows = 48, Wd = 410, Hd = 502;
                uint16_t* buf = (uint16_t*)heap_caps_malloc(Wd * rows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                if (buf) {
                    memset(buf, 0, Wd * rows * 2);
                    uint32_t t0 = micros();
                    for (int y = 0; y < Hd; y += rows) instance.pushColorsAsync(0, y, Wd, min(rows, Hd - y), buf);
                    instance.waitPushComplete();
                    Serial.printf("BENCH full frame async DMA %lu us\n", (unsigned long)(micros() - t0));
                    heap_caps_free(buf);
                    lv_obj_invalidate(lv_screen_active());
                }
                break;
            }
            default: break;
        }
    }
}

// ------------------------------------------------------------------ boot splash
// The splash lives on lv_layer_top() and is opaque: nothing of the pages that
// are built and rendered behind it can ever show through. The panel stays dark
// until the first splash frame is on the glass, then fades in.
static lv_obj_t* sSplash = nullptr;
static lv_obj_t* sBootBar = nullptr;
static volatile float sBootSubsys = 0.0f;     // setup(): subsystem bring-up 0..1
static int sBootShown = -1;
static const float BOOT_SUBSYS_SHARE = 0.45f; // bar share of the bring-up, rest = page warm-up

static void setBootBar(float p) {
    int v = (int)(p * 100.0f);
    if (v > 100) v = 100;
    if (v == sBootShown || !sBootBar) return;
    sBootShown = v;
    lv_bar_set_value(sBootBar, v, LV_ANIM_OFF);
    lv_refr_now(NULL);
}

void DisplayEngine::bootProgress(float p) { sBootSubsys = p < 0 ? 0 : (p > 1 ? 1 : p); }

void DisplayEngine::beginSplash() {
    initFonts();
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, cBg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, F.body, 0);
    lv_obj_set_style_text_color(scr, cText(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    sSplash = box(lv_layer_top(), SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(sSplash, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sSplash, cBg(), 0);
    lv_obj_t* logo = label(sSplash, F.numM, cText(), "WATCHOS");
    lv_obj_set_style_text_letter_space(logo, 10, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 5, -18);
    lv_obj_t* sub = caption(sSplash, "ULTRA", accent());
    lv_obj_set_style_text_letter_space(sub, 8, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 4, 22);
    char ver[48];
    snprintf(ver, sizeof(ver), "VERSION %s  •  BUILD %s", OS_VERSION, OS_BUILD);
    lv_obj_t* vl = caption(sSplash, ver, cText3());
    lv_obj_align(vl, LV_ALIGN_BOTTOM_MID, 0, -96);
    sBootBar = lv_bar_create(sSplash);
    lv_obj_set_size(sBootBar, 120, 3);
    lv_obj_align(sBootBar, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_bar_set_range(sBootBar, 0, 100);
    lv_obj_set_style_bg_color(sBootBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sBootBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sBootBar, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sBootBar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sBootBar, 2, LV_PART_INDICATOR);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    instance.waitPushComplete();

    // fade in from black (the panel was dark since setup())
    const uint8_t target = PowerManager::hwBrightnessFor(SettingsStorage::get().brightness);
    for (int i = 1; i <= 12; i++) {
        instance.setBrightness((uint8_t)((uint32_t)target * i * i / 144));
        vTaskDelay(pdMS_TO_TICKS(22));
    }
}

void DisplayEngine::splashTick() { setBootBar(sBootSubsys * BOOT_SUBSYS_SHARE); }

// ------------------------------------------------------------------ public
bool DisplayEngine::init() {
    Serial.println("[UI] Building WatchOS Ultra interface...");
    if (!sSplash) beginSplash();
    lv_obj_t* scr = lv_screen_active();

    pages::buildFace(scr);
    pages::buildAiLimits(scr);
    pages::buildLauncher(scr);
    pages::buildActivity(scr);
    pages::buildAgenda(scr);
    pages::buildPosition(scr);
    pages::buildLora(scr);
    pages::buildSystem(scr);
    pages::buildControl(scr);
    pages::buildNotifications(scr);
    pages::buildConnect(scr);
    pages::buildStopwatch(scr);
    pages::buildPower(scr);
    pages::buildFaces(scr);
    pages::buildSettings(scr);
    pages::buildAppMedia(scr);
    pages::buildCalib(scr);
    pages::buildGames(scr);
    pages::buildCalculator(scr);

    buildBanner();
    buildAodAndTorch();

    // the overlays were created after the splash: keep the splash on top
    lv_obj_move_foreground(sSplash);

    uint32_t tw = millis();
    nav::warmup([](float p) { setBootBar(BOOT_SUBSYS_SHARE + p * (1.0f - BOOT_SUBSYS_SHARE)); });
    setBootBar(1.0f);
    Serial.printf("[UI] warm-up: all pages rendered in %lu ms\n", (unsigned long)(millis() - tw));

    // Delete splash overlay before reveal so it cannot linger or overlap behind LVGL
    lv_display_t* dsp = lv_display_get_default();
    lv_display_enable_invalidation(dsp, false);
    if (sSplash) {
        lv_obj_delete(sSplash);
        sSplash = nullptr;
        sBootBar = nullptr;
    }
    lv_display_enable_invalidation(dsp, true);

    nav::bootReveal(nav::PG_FACE);

    lv_display_add_event_cb(dsp, perfRenderEvent, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(dsp, perfRenderEvent, LV_EVENT_RENDER_READY, NULL);

    sLastAccent = SettingsStorage::get().accentColor;
    sReady = true;
    Serial.printf("[UI] WatchOS Ultra interface ready (PSRAM free %u KB)\n", (unsigned)(ESP.getFreePsram() / 1024));
    return true;
}

void DisplayEngine::update(unsigned long now) {
    debugConsole();
    perfReport(now);
    if (!sReady) return;
    pollInbox();

    int key = PowerManager::takeKeyEvent();
    if (key) {
        bool wasSleeping = (PowerManager::getPowerState() != POWER_ACTIVE);
        PowerManager::handleActivity();
        if (torchOn()) {
            setTorch(false);
        } else if (!wasSleeping && !dialogOpen()) {
            if (pages::closeRingsDetailIfOpen()) {
                haptic(HAPTIC_TICK);
            } else if (key == 1) {
                nav::sideKey(false);
            } else if (key == 2) {
                nav::sideKey(true);
            } else if (key == 3) {
                // Action button single click: face -> position -> control -> home
                haptic(HAPTIC_CLICK);
                if (nav::current() == nav::PG_FACE) {
                    nav::go(nav::PG_POSITION);
                } else if (nav::current() == nav::PG_POSITION) {
                    nav::go(nav::PG_CONTROL);
                } else {
                    nav::home();
                }
            } else if (key == 4) {
                // Action button double click: torch
                haptic(HAPTIC_TICK);
                setTorch(!torchOn());
            } else if (key == 5) {
                // Action button long press: Meshtastic discovery (rate-limited by RadioLoRa)
                bool sent = RadioLoRa::sendNodeInfoDiscovery();
                haptic(sent ? HAPTIC_ALERT_SHORT : HAPTIC_TICK);
                showToast("Mesh Discovery",
                          sent ? "NodeInfo-Anfrage gesendet"
                               : (RadioLoRa::isEnabled() ? "Bereits gesendet (Wartezeit)" : "LoRa ist aus"),
                          2500, ICO_PODCASTS);
            }
        }
    }

    // ---- panel power: wake before anything is drawn ----
    PowerState ps = PowerManager::getPowerState();
    if (sPanelAsleep && ps != POWER_SLEEP) {
        PowerManager::boostCpu();                       // render the wake frame at full speed
        if (PowerManager::lockI2C(50)) {
            instance.wakeupDisplay();                   // DISP_EN + SLPOUT, brightness still 0
            PowerManager::unlockI2C();
            sPanelAsleep = false;
            vTaskDelay(pdMS_TO_TICKS(6));               // SLPOUT -> first RAM write
            if (ps != POWER_AOD) nav::repaint();        // AOD: refreshAod() draws the clock
        }
    }

    uint16_t acc = SettingsStorage::get().accentColor;
    if (acc != sLastAccent) {
        sLastAccent = acc;
        applyAccent();
        nav::markAllDirty();
    }

    static unsigned long lastAod = 0;
    bool aodWanted = (ps == POWER_AOD);
    bool aodShown = !lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN);
    if (aodWanted != aodShown || now - lastAod >= 250) { lastAod = now; refreshAod(); }
    bool lowPower = PowerManager::isLowPowerState();
    nav::setGesturesEnabled(lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN) && !torchOn() && !dialogOpen());
    // AOD / sleep: only the AOD clock is maintained; pages are neither refreshed
    // nor pre-rendered (nav still services touch wake-up and pending actions).
    // Frame-rate governor for live pages: ~30 fps when active, 1 Hz when dimmed
    // (sweeping hands tick, nothing animates at full rate on a dim screen).
    nav::update(now, !lowPower, ps == POWER_DIM ? 1000 : 33);
    if (!lowPower) pollOta(now);
    PowerManager::applyBrightnessOnUiThread();

    // ---- panel power: sleep once the screen is dark ----
    if (!sPanelAsleep && ps == POWER_SLEEP && PowerManager::appliedHwBrightness() == 0 &&
        !nav::transitioning()) {
        instance.waitPushComplete();
        if (PowerManager::lockI2C(50)) {
            instance.sleepDisplay();                    // SLPIN + DISP_EN low
            PowerManager::unlockI2C();
            sPanelAsleep = true;
        }
    }
}

void DisplayEngine::showNotification(const String& title, const String& msg, uint32_t durationMs, const char* icon) {
    NotificationStore::add(title.c_str(), msg.c_str(), icon);
    enqueueBanner(title.c_str(), msg.c_str(), durationMs, icon, true);
    if (!sDnd) MotionHaptic::triggerHaptic(HAPTIC_ALERT_SHORT);
}

void DisplayEngine::showToast(const char* title, const char* msg, uint32_t durationMs, const char* icon) {
    enqueueBanner(title, msg, durationMs, icon, false);
}

void DisplayEngine::setDoNotDisturb(bool on) { sDnd = on; }
bool DisplayEngine::doNotDisturb() { return sDnd; }

void DisplayEngine::setTorch(bool on) {
    // PowerManager drives the panel to full brightness while the override is
    // set and restores the user brightness afterwards (settings untouched).
    if (on) {
        setHidden(sTorch, false);
        lv_obj_move_foreground(sTorch);
        PowerManager::setTorchOverride(true);
    } else {
        setHidden(sTorch, true);
        PowerManager::setTorchOverride(false);
    }
}

bool DisplayEngine::torchOn() { return sTorch && !lv_obj_has_flag(sTorch, LV_OBJ_FLAG_HIDDEN); }

bool DisplayEngine::isOverlayBlockingMirror() {
    return (sAod && !lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN)) ||
           (sTorch && !lv_obj_has_flag(sTorch, LV_OBJ_FLAG_HIDDEN));
}

void DisplayEngine::nextWatchface() { pages::faceNext(); }
bool DisplayEngine::isReady() { return sReady; }
