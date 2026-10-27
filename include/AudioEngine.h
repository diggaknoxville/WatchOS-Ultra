#pragma once
#include "Config.h"

enum SoundFx {
    SFX_BOOT = 0,
    SFX_CHIME = 1,
    SFX_CLICK = 2,
    SFX_ALERT = 3,
    SFX_COUNTDOWN = 4,
    SFX_SUCCESS = 5,
    SFX_NOTIFY = 6,
    SFX_SOS = 7,
    SFX_ALARM = 8
};

// MAX98357A class-D amplifier via instance.player (I2S). Playback runs on a
// dedicated task fed by a queue, so callers never block. The amplifier rail
// (BLDO2, switched through the AXP2101 over I2C) is only powered while sound
// is playing; the PMU register write is done under PowerManager::lockI2C().
class AudioEngine {
public:
    static bool init();
    static void playTone(uint16_t freqHz, uint16_t durationMs);
    static void playSoundFx(SoundFx fx);
    static void stop();
    static bool isReady();
    static bool isBusy();                    // amplifier powered or tones queued (no light sleep)

    // Output volume 0..100 % (scales the synthesis amplitude). Default 70.
    // Not persisted; callers that want persistence store it themselves.
    static void setVolumePct(uint8_t pct);
    static uint8_t getVolumePct();

private:
    static bool ready;
};
