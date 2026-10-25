#include "AudioEngine.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include <LilyGoLib.h>
#include <math.h>

#define SAMPLE_RATE     22050
#define AMPLITUDE_FULL  12850.0f    // 100 % volume; 70 % ~= 9000 (the former fixed amplitude)

bool AudioEngine::ready = false;

struct ToneReq { uint16_t freq; uint16_t ms; };   // freq 0 = rest
static QueueHandle_t sQueue = nullptr;
static volatile bool sStop = false;
static volatile uint8_t sVolumePct = 70;

static void push(uint16_t f, uint16_t ms) {
    if (!sQueue) return;
    ToneReq r = {f, ms};
    xQueueSend(sQueue, &r, 0);
}

// Sine + soft 2nd harmonic with a short attack/release envelope (no clicks).
static void renderTone(const ToneReq& r) {
    const uint32_t total = (uint32_t)SAMPLE_RATE * r.ms / 1000;
    const uint32_t ramp = SAMPLE_RATE / 200;                  // 5 ms
    const float amplitude = AMPLITUDE_FULL * (float)sVolumePct / 100.0f;
    int16_t buf[256];                                          // 128 stereo frames
    float phase = 0, inc = 2.0f * (float)M_PI * r.freq / SAMPLE_RATE;
    uint32_t n = 0;
    while (n < total && !sStop) {
        uint32_t frames = min<uint32_t>(128, total - n);
        for (uint32_t i = 0; i < frames; i++, n++) {
            float s = 0;
            if (r.freq) {
                float env = 1.0f;
                if (n < ramp) env = (float)n / ramp;
                else if (total - n < ramp) env = (float)(total - n) / ramp;
                s = (sinf(phase) + 0.25f * sinf(2 * phase)) * 0.8f * env * amplitude;
                phase += inc;
                if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            }
            buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
        }
        instance.player.write((const uint8_t*)buf, frames * 4);
    }
}

// Switch the amplifier rail. powerControl() is a read-modify-write on an
// AXP2101 register shared with the display/radio/GPS/sensor rails, so it must
// run under the global I2C lock. Returns false if the bus was busy (the caller
// retries on the next loop iteration).
static bool setAmpRail(bool on) {
    if (!PowerManager::lockI2C(100)) return false;
    instance.powerControl(POWER_SPEAK, on);
    PowerManager::unlockI2C();
    return true;
}

static void audioTask(void*) {
    bool powered = false;
    bool havePending = false;
    ToneReq pending = {0, 0};
    TickType_t idleSince = xTaskGetTickCount();
    for (;;) {
        if (!havePending) {
            if (xQueueReceive(sQueue, &pending, pdMS_TO_TICKS(100)) == pdTRUE) havePending = true;
        }
        if (havePending) {
            if (!powered) {
                if (!setAmpRail(true)) {
                    vTaskDelay(pdMS_TO_TICKS(10));     // I2C busy: keep the request, retry
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(15));
                powered = true;
            }
            sStop = false;
            renderTone(pending);
            havePending = false;
            idleSince = xTaskGetTickCount();
        } else if (powered && xTaskGetTickCount() - idleSince > pdMS_TO_TICKS(400)) {
            // flush silence through the DMA, then cut the amplifier rail
            ToneReq silence = {0, 40};
            renderTone(silence);
            if (setAmpRail(false)) powered = false;    // else retry next iteration
        }
    }
}

bool AudioEngine::init() {
    ready = instance.player.configureTX(SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    setAmpRail(false);                                  // setup(): lock is uncontended
    sQueue = xQueueCreate(32, sizeof(ToneReq));
    xTaskCreatePinnedToCore(audioTask, "Audio", 4096, NULL, 1, NULL, 0);
    Serial.printf("[AUDIO] MAX98357A I2S %s @ %d Hz, volume %u%%\n",
                  ready ? "ready" : "FAILED", SAMPLE_RATE, (unsigned)sVolumePct);
    return ready;
}

bool AudioEngine::isReady() { return ready; }

void AudioEngine::setVolumePct(uint8_t pct) { sVolumePct = pct > 100 ? 100 : pct; }
uint8_t AudioEngine::getVolumePct() { return sVolumePct; }

void AudioEngine::playTone(uint16_t freqHz, uint16_t durationMs) {
    if (!ready || !SettingsStorage::get().soundEnabled || sVolumePct == 0) return;
    push(freqHz, durationMs);
}

void AudioEngine::playSoundFx(SoundFx fx) {
    if (!ready || !SettingsStorage::get().soundEnabled || sVolumePct == 0) return;
    switch (fx) {
        case SFX_BOOT:      push(523, 90); push(659, 90); push(784, 90); push(1047, 220); break;
        case SFX_CHIME:     push(880, 80); push(1319, 160); break;
        case SFX_CLICK:     push(2400, 12); break;
        case SFX_NOTIFY:    push(1175, 70); push(0, 40); push(1568, 120); break;
        case SFX_ALERT:     push(1200, 110); push(0, 60); push(1200, 160); break;
        case SFX_COUNTDOWN: push(1000, 60); break;
        case SFX_SUCCESS:   push(659, 90); push(880, 90); push(1319, 200); break;
        case SFX_ALARM:     for (int i = 0; i < 4; i++) { push(1760, 110); push(0, 70); push(1760, 110); push(0, 300); } break;
        case SFX_SOS:       // ... --- ...
            for (int k = 0; k < 3; k++) { push(880, 90);  push(0, 90); }  push(0, 180);
            for (int k = 0; k < 3; k++) { push(880, 270); push(0, 90); }  push(0, 180);
            for (int k = 0; k < 3; k++) { push(880, 90);  push(0, 90); }
            break;
    }
}

void AudioEngine::stop() {
    sStop = true;
    if (sQueue) xQueueReset(sQueue);
}
