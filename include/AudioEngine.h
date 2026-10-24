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
// (BLDO2) is only powered while sound is playing.
class AudioEngine {
public:
    static bool init();
    static void playTone(uint16_t freqHz, uint16_t durationMs);
    static void playSoundFx(SoundFx fx);
    static void stop();
    static bool isReady();

private:
    static bool ready;
};
