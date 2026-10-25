# Third-party notices

WatchOS Ultra itself is licensed under the MIT License (see `LICENSE`).
It links against or embeds the following third-party components. Each
component remains under its own license; the license texts are available in
the respective upstream repositories and, for the vendored copies, in `lib/`.

| Component | Version used | License | Source |
| --- | --- | --- | --- |
| LilyGoLib (vendored fork, see `lib/LilyGoLib/README.MD`) | 0.1.0-watchos.1 | MIT | https://github.com/Xinyuan-LilyGO/LilyGoLib |
| LVGL | 9.4.0 | MIT | https://github.com/lvgl/lvgl |
| RadioLib | 7.4.0 | MIT | https://github.com/jgromes/RadioLib |
| XPowersLib | 0.3.3 | MIT | https://github.com/lewisxhe/XPowersLib |
| SensorLib | 0.3.3 | MIT | https://github.com/lewisxhe/SensorLib |
| NimBLE-Arduino | 2.5.2 | Apache-2.0 | https://github.com/h2zero/NimBLE-Arduino |
| TinyGPSPlus (vendored, required by LilyGoLib) | 1.1.0 | LGPL-2.1 | https://github.com/mikalhart/TinyGPSPlus |
| ST25R3916 / RFAL NFC driver (vendored, required by the T-Watch Ultra board variant) | LilyGo fork | see `lib/NFC-RFAL-fork/LICENSE`, `lib/ST25R3916-fork` | https://github.com/Xinyuan-LilyGO |
| Arduino core for ESP32 (pioarduino) | 3.3.0 (ESP-IDF 5.5) | LGPL-2.1 / Apache-2.0 (ESP-IDF) | https://github.com/pioarduino/platform-espressif32 |

## Fonts (embedded as LVGL `tiny_ttf` subsets in `src/fonts/`)

| Font | License | Source |
| --- | --- | --- |
| Orbitron | SIL Open Font License 1.1 | https://github.com/theleagueof/orbitron |
| Rajdhani | SIL Open Font License 1.1 | https://github.com/itfoundry/rajdhani |
| Material Symbols Rounded | Apache License 2.0 | https://github.com/google/material-design-icons |

The font sources and the subsetting scripts are in `tools/fonts/`. Per the
SIL OFL, the Orbitron and Rajdhani subsets are redistributed under the OFL;
the Reserved Font Names are not used for the subsets.

## Meshtastic

The LoRa mesh implementation speaks the Meshtastic packet format (protobuf
schema from https://github.com/meshtastic/protobufs, GPL-3.0 for the
firmware, the schema definitions are re-implemented here without copying
code). "Meshtastic" is a registered trademark of Meshtastic LLC.
