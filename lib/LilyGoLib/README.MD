> **Fork notice (WatchOS Ultra)**
>
> This is a vendored copy of LilyGoLib 0.1.0 with three local patches. Do not
> replace it with an upstream release without re-applying them:
>
> 1. `src/LilyGoDispInterface.h/.cpp`: `pushColorsAsync()` / `waitPushComplete()`
>    (non-blocking QSPI DMA push used by the compositor).
> 2. `src/lv_conf.h`: `LV_DEF_REFR_PERIOD 10` (100 Hz refresh + touch sampling).
> 3. `src/LV_Helper_v9.cpp`: flush instrumentation (`g_kronosFlushUs`,
>    `g_kronosFlushPx`, read by the UI performance monitor).
>
> Version is reported as `0.1.0-watchos.1`; `library.json` dependencies are
> aligned with the project's `platformio.ini` (RadioLib 7.4.0, lvgl 9.4.0,
> XPowersLib 0.3.3, SensorLib 0.3.3). Upstream: https://github.com/Xinyuan-LilyGO/LilyGoLib

<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGoLib🌟</h1>

### `1` Get Started

- 🔧 **[T-LoRa-Pager Arduino IDE Quick Start](./docs/lilygo-t-lora-pager.md)**
- 🔧 **[T-Watch-Ultra Arduino IDE Quick Start](./docs/lilygo-t-watch-ultra.md)**
- 🔧 **[T-Watch-S3 Arduino IDE Quick Start](./docs/lilygo-t-watch-s3.md)**
- 🔧 **[T-Watch-S3-Plus Arduino IDE Quick Start](./docs/lilygo-t-watch-s3-plus.md)**

### `2` Hardware Information

- 🔧 **[T-LoRa-Pager Hardware Information](./docs/hardware/lilygo-t-lora-pager.md)**
- 🔧 **[T-Watch-Ultra Hardware Information](./docs/hardware/lilygo-t-watch-ultra.md)**
- 🔧 **[T-Watch-S3 Hardware Information](./docs/hardware/lilygo-t-watch-s3.md)**
- 🔧 **[T-Watch-S3-Plus Hardware Information](./docs/hardware/lilygo-t-watch-s3-plus.md)**

### `3` GPS AssistNow Usage Guide

- 🔧 **[GPS AssistNow Usage Guide](./docs/assistNow/assistNow.md)**

### `4` Unable to flash again

- ⚠️ **[Flash Steps Guide](./firmware/README.md)**
