// ============================================================================
//  KRONOS UI facade: pages, overlays, hardware events, debug console.
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
static uint8_t sTorchPrevBrightness = 0;
static uint16_t sLastAccent = 0xFFFF;
static bool sReady = false;

// cross-task notification inbox (latest wins; history lives in NotificationStore)
static portMUX_TYPE sInboxMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool sInNotif = false;
static char sInTitle[40], sInMsg[112], sInIcon[4];
static uint32_t sInDuration = 4500;

static void bannerAnimY(void* o, int32_t v) { lv_obj_set_y((lv_obj_t*)o, v); }

static void bannerSlide(bool in) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sBanner);
    lv_anim_set_exec_cb(&a, bannerAnimY);
    lv_anim_set_values(&a, lv_obj_get_y(sBanner), in ? 16 : -180);
    lv_anim_set_duration(&a, in ? 340 : 260);
    lv_anim_set_path_cb(&a, in ? lv_anim_path_overshoot : lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void buildBanner() {
    sBanner = card(lv_layer_top(), CW, 124);
    lv_obj_set_pos(sBanner, M, -180);
    lv_obj_set_style_bg_color(sBanner, cSurf2(), 0);
    lv_obj_add_flag(sBanner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* disc = box(sBanner, 44, 44);
    lv_obj_set_pos(disc, 16, 16);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(disc, accent(), 0);
    accReg(disc, ACC_BG);
    sBannerIcon = label(disc, F.icoS, cBg(), ICO_NOTIFICATIONS);
    lv_obj_center(sBannerIcon);
    sBannerTitle = label(sBanner, F.body, cText(), "");
    lv_label_set_long_mode(sBannerTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sBannerTitle, CW - 92);
    lv_obj_set_pos(sBannerTitle, 74, 12);
    sBannerMsg = label(sBanner, F.small, cText2(), "");
    lv_label_set_long_mode(sBannerMsg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sBannerMsg, CW - 92);
    lv_obj_set_pos(sBannerMsg, 74, 46);
    lv_obj_add_event_cb(sBanner, [](lv_event_t*) {
        sBannerHideAt = 0;
        bannerSlide(false);
        nav::go(nav::PG_NOTIFS);
    }, LV_EVENT_CLICKED, NULL);
}

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
    lv_obj_t* th = caption(sTorch, "TAP TO TURN OFF", lv_color_hex(0x9AA0A8));
    lv_obj_align(th, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(sTorch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(sTorch, [](lv_event_t*) { DisplayEngine::setTorch(false); }, LV_EVENT_CLICKED, NULL);
}

static void refreshAod() {
    bool aod = PowerManager::getPowerState() == POWER_AOD;
    bool shown = !lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN);
    if (aod != shown) {
        setHidden(sAod, !aod);
        if (aod) {
            lv_obj_move_foreground(sAod);
        } else {
            // Immediately repaint the active screen when leaving AOD so the AOD clock
            // never lingers on the panel or in the compositor mirror.
            nav::markDirty(nav::current());
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(NULL);
        }
    }
    if (aod) {
        struct tm t;
        RtcManager::getLocalTimeInfo(&t);
        char b[32];
        snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
        setText(sAodTime, b);
        snprintf(b, sizeof(b), "%s %d %s", WD_S[t.tm_wday % 7], t.tm_mday, MO_S[t.tm_mon % 12]);
        setText(sAodDate, b);
    }
}

static void pollInbox() {
    if (sInNotif) {
        char title[40], msg[112], icon[4];
        uint32_t dur;
        portENTER_CRITICAL(&sInboxMux);
        memcpy(title, sInTitle, sizeof(title));
        memcpy(msg, sInMsg, sizeof(msg));
        memcpy(icon, sInIcon, sizeof(icon));
        dur = sInDuration;
        sInNotif = false;
        portEXIT_CRITICAL(&sInboxMux);
        lv_label_set_text(sBannerTitle, title);
        lv_label_set_text(sBannerMsg, msg);
        lv_label_set_text(sBannerIcon, icon[0] ? icon : ICO_NOTIFICATIONS);
        lv_obj_move_foreground(sBanner);
        bannerSlide(true);
        sBannerHideAt = millis() + dur;
        PowerManager::handleActivity();
        nav::markDirty(nav::PG_NOTIFS);
        nav::markDirty(nav::PG_FACE);
    }
    if (sBannerHideAt && millis() > sBannerHideAt) {
        sBannerHideAt = 0;
        bannerSlide(false);
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
    g_kronosFlushUs = g_kronosFlushPx = 0;
}

// Serial console (UI task):
//   S snapshot | G<page> go | F<n> face | T<epoch>,<tz>[,<zone>] time | N<title>|<msg>
//   P perf toggle | K / L side key click / long | B panel throughput
static void debugConsole() {
    static char line[96];
    static uint8_t len = 0;
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
                Serial.write(snap->data, (size_t)snap->header.stride * snap->header.h);
                Serial.print("\nEND\n");
                lv_draw_buf_destroy(snap);
                break;
            }
            case 'G':
                nav::go((nav::PageId)atoi(line + 1));
                Serial.printf("PAGE %d\n", (int)nav::current());
                break;
            case 'F':
                SettingsStorage::get().watchfaceIndex = atoi(line + 1) % FACE_COUNT;
                SettingsStorage::save();
                Serial.printf("FACE %d\n", SettingsStorage::get().watchfaceIndex);
                break;
            case 'T': {
                long ep = 0, tz = 0;
                char zone[40] = {0};
                sscanf(line + 1, "%ld,%ld,%39s", &ep, &tz, zone);
                RtcManager::syncFromEpoch((time_t)ep, (int32_t)tz, zone[0] ? zone : nullptr);
                RtcManager::noteSyncSource("USB");
                Serial.printf("TIME_SET %ld tz=%ld zone=%s\n", ep, tz, zone);
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
            case 'K': nav::sideKey(false); Serial.printf("KEY page %d\n", (int)nav::current()); break;
            case 'L': nav::sideKey(true);  Serial.printf("KEY page %d\n", (int)nav::current()); break;
            case 'I': {
                const NavStatus& st = NavEngine::status();
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
                RadioLoRa::injectDemoPeers();
                NavPlace pl[4];
                int n = NavEngine::searchPlaces(line + 1, pl, 4);
                if (n > 0) {
                    if (pl[0].rank == 0) NavEngine::setMode(NAV_MODE_MESH);
                    NavEngine::startNavigation(pl[0].name, pl[0].lat, pl[0].lon);
                    Serial.printf("NAV_OK %s %.4f,%.4f dist=%.1fkm eta=%umin\n",
                                  pl[0].name, pl[0].lat, pl[0].lon,
                                  NavEngine::status().totalDistKm, NavEngine::status().etaMinutes);
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
                            esp_rom_delay_us(25);
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

// ------------------------------------------------------------------ public
bool DisplayEngine::init() {
    Serial.println("[UI] Building KRONOS interface...");
    initFonts();

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, cBg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, F.body, 0);
    lv_obj_set_style_text_color(scr, cText(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- boot splash (shown while every page is rendered once) ----
    lv_obj_t* splash = box(scr, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(splash, cBg(), 0);
    lv_obj_t* logo = label(splash, F.numM, cText(), "KRONOS");
    lv_obj_set_style_text_letter_space(logo, 10, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 5, -18);
    lv_obj_t* sub = caption(splash, "ULTRA", accent());
    lv_obj_set_style_text_letter_space(sub, 8, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 4, 22);
    static lv_obj_t* sBootBar;
    sBootBar = lv_bar_create(splash);
    lv_obj_set_size(sBootBar, 120, 3);
    lv_obj_align(sBootBar, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_bar_set_range(sBootBar, 0, 100);
    lv_obj_set_style_bg_color(sBootBar, cStroke(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sBootBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sBootBar, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sBootBar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sBootBar, 2, LV_PART_INDICATOR);
    lv_refr_now(NULL);

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

    buildBanner();
    buildAodAndTorch();

    uint32_t tw = millis();
    nav::warmup([](float p) {
        lv_bar_set_value(sBootBar, (int)(p * 100), LV_ANIM_OFF);
        lv_refr_now(NULL);
    });
    Serial.printf("[UI] warm-up: all pages rendered in %lu ms\n", (unsigned long)(millis() - tw));
    nav::bootReveal(nav::PG_FACE);
    lv_display_t* dsp = lv_display_get_default();
    lv_display_enable_invalidation(dsp, false);
    lv_obj_delete(splash);                      // glass already shows the face
    lv_display_enable_invalidation(dsp, true);

    lv_display_t* disp = lv_display_get_default();
    lv_display_add_event_cb(disp, perfRenderEvent, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, perfRenderEvent, LV_EVENT_RENDER_READY, NULL);

    sLastAccent = SettingsStorage::get().accentColor;
    sReady = true;
    Serial.printf("[UI] KRONOS interface ready (PSRAM free %u KB)\n", (unsigned)(ESP.getFreePsram() / 1024));
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
        } else if (!wasSleeping) {
            if (key == 1) {
                nav::sideKey(false);
            } else if (key == 2) {
                nav::sideKey(true);
            } else if (key == 3) {
                // Action Button (GPIO 0) Single Click
                ui::haptic(HAPTIC_CLICK);
                if (nav::current() == nav::PG_FACE) {
                    nav::go(nav::PG_POSITION);
                } else if (nav::current() == nav::PG_POSITION) {
                    nav::go(nav::PG_CONTROL);
                } else {
                    nav::home();
                }
            } else if (key == 4) {
                // Action Button (GPIO 0) Double Click: Torch
                ui::haptic(HAPTIC_TICK);
                setTorch(!torchOn());
            } else if (key == 5) {
                // Action Button (GPIO 0) Long Press: Meshtastic Discovery Beacon
                ui::haptic(HAPTIC_ALERT_LONG);
                RadioLoRa::sendNodeInfoDiscovery();
                DisplayEngine::showNotification("Action Button", "Meshtastic Hub Discovery Beacon gesendet!", 2500, ICO_PODCASTS);
            }
        }
    }

    uint16_t acc = SettingsStorage::get().accentColor;
    if (acc != sLastAccent) {
        sLastAccent = acc;
        applyAccent();
        nav::markAllDirty();
    }

    static unsigned long lastAod = 0;
    if (now - lastAod >= 250) { lastAod = now; refreshAod(); }
    nav::setGesturesEnabled(lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN) && !torchOn());
    nav::update(now);
    PowerManager::applyBrightnessOnUiThread();
}

void DisplayEngine::showNotification(const String& title, const String& msg, uint32_t durationMs, const char* icon) {
    NotificationStore::add(title.c_str(), msg.c_str(), icon);
    portENTER_CRITICAL(&sInboxMux);
    strncpy(sInTitle, title.c_str(), sizeof(sInTitle) - 1);
    sInTitle[sizeof(sInTitle) - 1] = 0;
    strncpy(sInMsg, msg.c_str(), sizeof(sInMsg) - 1);
    sInMsg[sizeof(sInMsg) - 1] = 0;
    strncpy(sInIcon, icon ? icon : "", sizeof(sInIcon) - 1);
    sInIcon[sizeof(sInIcon) - 1] = 0;
    sInDuration = durationMs;
    sInNotif = true;
    portEXIT_CRITICAL(&sInboxMux);
    MotionHaptic::triggerHaptic(HAPTIC_ALERT_SHORT);
}

void DisplayEngine::setTorch(bool on) {
    if (on) {
        sTorchPrevBrightness = SettingsStorage::get().brightness;
        setHidden(sTorch, false);
        lv_obj_move_foreground(sTorch);
        PowerManager::setBrightness(255);
    } else {
        setHidden(sTorch, true);
        PowerManager::setBrightness(sTorchPrevBrightness ? sTorchPrevBrightness : 220);
    }
}

bool DisplayEngine::torchOn() { return sTorch && !lv_obj_has_flag(sTorch, LV_OBJ_FLAG_HIDDEN); }

bool DisplayEngine::isOverlayBlockingMirror() {
    return (sAod && !lv_obj_has_flag(sAod, LV_OBJ_FLAG_HIDDEN)) ||
           (sTorch && !lv_obj_has_flag(sTorch, LV_OBJ_FLAG_HIDDEN));
}

void DisplayEngine::setApp(AppId app) {
    switch (app) {
        case APP_FITNESS:        nav::go(nav::PG_ACTIVITY); break;
        case APP_LORA_MESH:      nav::go(nav::PG_LORA); break;
        case APP_SENSORS:        nav::go(nav::PG_SYSTEM); break;
        case APP_PHONE_SYNC:     nav::go(nav::PG_APP_CONNECT); break;
        case APP_QUICK_SETTINGS: nav::go(nav::PG_CONTROL); break;
        case APP_STOPWATCH:      nav::go(nav::PG_APP_STOPWATCH); break;
        default:                 nav::home(); break;
    }
}

AppId DisplayEngine::getCurrentApp() { return APP_WATCHFACE; }
void DisplayEngine::nextWatchface() { pages::faceNext(); }
bool DisplayEngine::isReady() { return sReady; }
