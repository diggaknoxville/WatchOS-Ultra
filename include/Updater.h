#pragma once
#include <Arduino.h>

// Firmware updates from the GitHub releases of this project.
//
// The watch joins a stored WiFi network as a client (never while the hotspot
// is up), reads releases/latest/download/version.json, and offers a newer
// version on the watch. After the user accepts, firmware.bin is streamed into
// the inactive OTA slot (image sink in CompanionServer: SHA-256, signature when
// UpdateKey.h holds a key) and the watch restarts into it.
// Automatic checks run at most once a day and only on USB power.
// All network work happens on a short-lived worker task.
namespace Updater {
    enum State : uint8_t { IDLE, CHECKING, UP_TO_DATE, AVAILABLE, DOWNLOADING, FAILED };

    void init();                                   // setup(): load credentials
    void loop(unsigned long now);                  // background task: daily schedule
    void setWifi(const char* ssid, const char* pass);   // "" clears
    const char* ssid();                            // "" if none stored
    bool autoCheck();
    void setAutoCheck(bool on);
    void checkNow();                               // any task
    void install();                                // any task: download the available version
    State state();
    bool busy();                                   // WiFi client in use
    const char* availableVersion();                // valid in AVAILABLE / DOWNLOADING
    int progressPct();                             // DOWNLOADING: 0..100
    const char* statusText();                      // short German status for the UI
    bool takeOffer();                              // UI task: true once per newly found version
}
