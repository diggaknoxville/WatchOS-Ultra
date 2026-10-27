# WatchOS Ultra

Firmware für die **LilyGO T-Watch Ultra** (ESP32-S3) auf Basis von Arduino-ESP32 3.3 (pioarduino), LVGL 9.4 und FreeRTOS. Zwei Tasks (UI auf Core 1, Hintergrunddienste auf Core 0), ein RLE-Compositor für das 410×502-AMOLED, Meshtastic-kompatibler LoRa-Funk, Offline-Karten von der microSD-Karte, GNSS, Schrittzähler und ein WLAN-Portal zur Konfiguration.

Version 1.2.0, Codename „Kronos“. Interne Bezeichner (`Kronos*`-Klassen, `kronos_*`-NVS-Namespaces) sind aus Kompatibilitätsgründen mit gespeicherten Daten beibehalten.

---

## Inhalt

- [Hardware](#hardware)
- [Architektur](#architektur)
- [Funktionen](#funktionen)
- [Sicherheit](#sicherheit)
- [Bauen & Flashen](#bauen--flashen)
- [Werkzeuge](#werkzeuge)
- [Projektstruktur](#projektstruktur)
- [Bekannte Einschränkungen](#bekannte-einschränkungen)
- [Lizenz](#lizenz)

---

## Hardware

Alle Pins stammen aus der Board-Variante `lilygo_twatch_ultra` des pioarduino Arduino-Cores (`pins_arduino.h`); die Firmware definiert keine eigenen Pins. Die Angaben unten sind daraus übernommen, wo sie dort eindeutig stehen.

| Komponente | Baustein | Anbindung |
| :--- | :--- | :--- |
| SoC | ESP32-S3, 2× Xtensa LX7 @ 240 MHz | 16 MB Flash, 8 MB PSRAM (QIO/QSPI) |
| Display | CO5300 AMOLED 1,75", 410×502 px | QSPI: D0 38, D1 39, D2 42, D3 45, SCK 40, CS 41, RST 37, TE 6 |
| Touch | CST9217 (kapazitiv) | I²C, INT GPIO 12, Reset über XL9555 |
| I²C-Hauptbus | – | SDA GPIO 3, SCL GPIO 2 (PMU, RTC, IMU, Haptik, Touch, Expander) |
| PMU | X-Powers AXP2101 (Lader, Fuel Gauge, Rails) | I²C |
| RTC | NXP PCF85063 (batteriegepuffert, läuft in UTC) | I²C |
| Sensor-Hub | Bosch BHI260AP (Schrittzähler, Aktivitätserkennung, Wrist-Tilt, Rotationsvektor) | I²C |
| Haptik | TI DRV2605 (LRA) | I²C |
| LoRa | Semtech SX1262, 868 MHz | SPI: CS 36, IRQ 14, RST 47, BUSY 48 (MISO/MOSI/SCK: siehe Board-Variante) |
| GNSS | u-blox (NMEA) | UART: TX 43, RX 44 |
| microSD | – | SPI (gemeinsam mit LoRa), CS 21 |
| Audio | MAX98357A Class-D | I²S (Pins: siehe Board-Variante) |
| NFC | ST25R3916 | SPI (siehe Board-Variante); wird von der Firmware nicht initialisiert (`NO_HW_NFC`) |
| I/O-Expander | XL9555 | I²C |

---

## Architektur

```
Core 1  UiCompositor (28 KB Stack, Prio 2)        Core 0  BgServices (16 KB Stack, Prio 1)
  LVGL 9.4 (einziger LVGL-Task)                      CompanionServer (WiFi-AP, HTTP, OTA)
  Compositor: RLE-Surfaces, 60 fps, TE-Sync           BleCompanion (NimBLE, Payload-Queue)
  Navigator: Gesten, Transitions                      PowerManager (AXP2101, Power-States)
  DisplayEngine: Seiten, Overlays, Debug-Konsole      MotionHaptic (BHI260AP, DRV2605)
                                                      GpsManager, RadioLoRa (SX1262, Meshtastic)
                                                      NavEngine::loop (asynchrones Routing)
                                                      SettingsStorage / NotificationStore (NVS-Flush)
                                                      RtcManager::periodicMaintenance (stündlich)
                                      AudioEngine: eigener Task mit Queue (Core 0)
```

- **UI-Task**: Erzeugt, ändert und löscht alle LVGL-Objekte. Seiten werden einmal gebaut und als RLE-komprimierte Surfaces gecacht; Übergänge rendert der Compositor aus diesen Surfaces und schiebt sie per QSPI-DMA (`pushColorsAsync`) TE-synchron auf das Panel. In AOD/SLEEP drosselt sich der Task auf 100 ms bzw. 250 ms.
- **Hintergrund-Task**: Alle Funk-, Sensor- und Speicherdienste. Das Routing der NavEngine läuft asynchron hier (`startNavigation()` kehrt sofort zurück, `NavStatus::routingBusy` zeigt den Fortschritt). Einstellungen und Benachrichtigungen werden mit Dirty-Flags verzögert in den NVS geschrieben (Benachrichtigungen slotweise, höchstens alle 3 s).
- **I²C**: Ein rekursiver Mutex (`PowerManager::lockI2C`) schützt den Bus zwischen beiden Cores, auch für den Verstärker-Rail des Audio-Tasks.
- **Zeit**: Die PCF85063 ist Master-Uhr in UTC; Lokalzeit über POSIX-TZ-Regeln inkl. Sommerzeit. Die Zeit gilt nur als gültig, wenn die RTC beim Start plausibel war oder seit dem Start eine echte Quelle (Telefon, GNSS, USB) synchronisiert hat.

---

## Funktionen

### Zifferblätter (14)

Modular, Chrono, Minimal, Wayfinder, Tourbillon, Casio, Bauhaus, Cyberpunk, Wortuhr (deutsch), Neon Horizon, Flieger, Diver, Radar, Lifetime. Auswahl über die App „Zifferblätter“, das Portal oder `watchctl.py face`.

### Navigation

Karussell (horizontal wischen), Reihenfolge wie in `include/ui/Navigator.h`:

`LAUNCHER ← ZIFFERBLATT → AI-LIMITS → AKTIVITÄT → AGENDA → POSITION → LORA → SYSTEM`

- **Spiele** (App-Drawer, Ordner mit 3 × 3 Kacheln): Labyrinth, Breakout, Würfel-Duell und Balance nutzen die Neigung bzw. das Schütteln des Handgelenks (BHI260AP), Snake und 2048 werden gewischt, Reaktion, Memory und Flappy getippt. Bestwerte liegen im NVS. Linke Kante nach rechts wischen = zurück zum Ordner.
- Vom Zifferblatt nach unten wischen: **Control Center** (Helligkeit, AOD, Wrist-Wake, Haptik, WLAN, Ton, Medien-Karte). Nach oben: **Mitteilungen**.
- Launcher-Apps: Kalender, Navi, Stoppuhr, LoRa, Connect, Einstellungen, Zifferblätter, Power, Media. Zurück per Wischen vom linken Rand, Home per Wischen vom unteren Rand oder Seitentaste.
- Action-Taste (GPIO 0): Klick = Position/Control, Doppelklick = Taschenlampe, lang = Meshtastic-NodeInfo senden.

### Aktivität

BHI260AP-Schrittzähler mit Plausibilitätsprüfung der Kadenz (Handbewegungen werden herausgefiltert), Hardware-Aktivitätserkennung (Gehen, Laufen, Rad, Fahrzeug), Schlaferkennung (unterdrückt Wrist-Wake), 7-Tage-Verlauf, Tagesziel. Distanz (0,75 m Schrittlänge) und Kalorien (0,042 kcal/Schritt) sind Schätzwerte ohne Nutzerprofil.

### Position & Karten

- u-blox-GNSS mit Fix-Filter (≥ 4 Satelliten, HDOP < 5), Satellitenliste, Zeitdisziplinierung der RTC aus GNSS-Zeit.
- Offline-Karte `/kronos/germany.knav` auf der microSD (Format in `include/MapFormat.h`): Autobahn bis Kreisstraße, Bahn, Gewässer deutschlandweit; Straßen-/Gebäudeebene nur im Raum Stuttgart. Erzeugt mit `tools/build_germany_map.cpp` aus einem OSM-PBF-Extrakt, Übertragung mit `tools/upload_map_sd.py` oder über das Portal.
- Routing (Auto/Fußgänger) auf einem rasterbasierten Graphen aus der Kartendatei, Turn-by-Turn-Hinweise; ohne Route wird ehrlich eine Luftlinie angezeigt (`LUFTLINIE`), keine erfundenen Abbiegehinweise.
- Positionsquelle mit Priorität: GNSS-Fix > eigener gespeicherter Fix > Mesh-Peer (< 30 min alt, als solcher gekennzeichnet) > „KEINE POSITION“.
- GPX-Aufzeichnung auf die SD-Karte, nur bei echtem GNSS-Fix.

### LoRa / Meshtastic

SX1262 auf EU868 LongFast (869,525 MHz, SF11, BW 250 kHz) mit dem Meshtastic-Standard-PSK (`AQ==`). Empfang und Senden von Textnachrichten, NodeInfo, Positions-Beacons (nur mit Fix) und SOS; AES-128-CTR wie bei Meshtastic. Peers werden mit RSSI/SNR und Alter angezeigt. Senden setzt voraus, dass LoRa in den Einstellungen aktiviert ist; kein Seitenaufruf schaltet den Funk ein.

### Media Remote (BLE)

Die Media-App ist eine Fernbedienung für das gekoppelte Telefon (Play/Pause, Titel, Lautstärke über BLE-Kommandos); Titel/Interpret werden vom Telefon gepusht. Es wird kein Audio auf der Uhr wiedergegeben.

### Kalender & AI-Limits (Companion-App)

Termine und die AI-Limits-Seite werden von einer Companion-App per BLE (JSON) geliefert; die App ist nicht Teil dieses Repos. Ohne Daten zeigen die Seiten „Noch nicht synchronisiert“.

### Weitere

- Benachrichtigungszentrale mit persistenter Historie (20 Einträge).
- Always-On-Display, Dimmen, Display-aus-Sleep mit Wrist-Wake; „Display aus“ im Power-Menü bleibt bis zur nächsten Berührung aktiv.
- Haptik-Muster (Queue, 4 Einträge) und Soundeffekte über I²S.
- Serielle Debug-Konsole (Screenshot, Seitenwechsel, Zeit setzen, synthetische Gesten; siehe `tools/watchctl.py`).

---

## Sicherheit

- **WLAN-AP**: startet nur auf Wunsch (Control Center oder Verbinden) und schaltet sich nach 5 Minuten ohne verbundenes Gerät selbst ab. SSID `WATCHOS-ULTRA-xxxx`, Passwort pro Gerät (10 Zeichen, aus der eFuse-MAC abgeleitet, im NVS gespeichert). Es wird nur auf der Uhr (Connect-Seite) angezeigt und steht nicht im Quelltext.
- **Portal-PIN**: Sechsstellige PIN, bei jedem AP-Start neu erzeugt und auf der Uhr angezeigt; alle `/api/*`-Aufrufe benötigen sie (`X-Pin`-Header oder `?pin=`). Ein gesetzter `Origin`-Header muss zur AP-Adresse passen, sonst 403.
- **BLE**: Characteristics verlangen verschlüsselte, authentifizierte Verbindungen; Kopplung per Passkey, der auf der Uhr angezeigt wird. BLE-Schreibzugriffe werden nicht im NimBLE-Host-Task, sondern in der Hintergrundschleife verarbeitet.
- **OTA**: Upload über `/api/ota/upload` (PIN) in die inaktive Partition mit SHA-256-Berechnung; der Neustart erfolgt erst nach Bestätigung auf der Uhr (Hash-Präfix und Größe werden angezeigt, 120 s Timeout = Ablehnung). Keine Signaturprüfung (kein Secure Boot).
- **Funk**: Kein automatisches Einschalten von LoRa/WLAN durch Seitenaufrufe; Positions-Beacons nur mit echtem oder kürzlich gespeichertem Fix.

---

## Bauen & Flashen

Voraussetzungen: PlatformIO Core (CI nutzt 6.2.0), Python 3.9+. Plattform (pioarduino 55.03.30 = Arduino 3.3.0 / ESP-IDF 5.5) und Bibliotheken sind in `platformio.ini` auf feste Versionen gepinnt.

```bash
git clone https://github.com/diggaknoxville/WatchOS-Ultra.git
cd WatchOS-Ultra
pio run                                  # kompilieren
pio run -t upload --upload-port COM5     # flashen (Port anpassen, z. B. /dev/ttyACM0)
pio device monitor -b 115200             # serieller Monitor
```

Oder mit dem Skript (bash, auch unter Git-Bash/WSL):

```bash
./build_firmware.sh build
./build_firmware.sh upload --port /dev/ttyACM0
./build_firmware.sh monitor --port /dev/ttyACM0
./build_firmware.sh sync-time            # Zeit & Zeitzone vom PC
./build_firmware.sh tour                 # Screenshots nach screenshots/
```

Partitionstabelle `app3M_fat9M_16MB.csv`: 3 MB App-Partition (OTA-fähig), 9 MB FAT (nicht gemountet), NVS. `maximum_size` im Board-JSON entspricht der App-Partition, PlatformIO warnt bei Überschreitung.

Die CI (`.github/workflows/build-firmware.yml`) baut bei jedem Push, lädt `firmware.bin/.elf/.map` als Artefakt hoch, gibt die Flash-/RAM-Belegung aus und erstellt bei Tags `v*` ein GitHub-Release mit der Binärdatei.

---

## Werkzeuge

Abhängigkeiten: `pip install -r tools/requirements.txt` (pyserial, Pillow, fonttools; optional tzlocal).

### `tools/watchctl.py`

Spricht mit der seriellen Debug-Konsole der Uhr. Port über `--port`, Umgebungsvariable `WATCHCTL_PORT` oder Auto-Erkennung (Espressif-VID 0x303A). Läuft unter Linux, macOS und Windows.

```bash
python tools/watchctl.py list-ports
python tools/watchctl.py --port COM5 time              # RTC + Zeitzone setzen, wartet auf TIME_SET
python tools/watchctl.py shot display.png --page system
python tools/watchctl.py tour screenshots/            # alle Zifferblätter und Seiten
python tools/watchctl.py goto position                 # Seitennamen aus include/ui/Navigator.h
python tools/watchctl.py face bauhaus
python tools/watchctl.py notify "Titel" "Text"
python tools/watchctl.py bench                         # [PERF]-Zeilen: Leerlauf + synthetische Swipes
python tools/watchctl.py dmabench                      # Dauer eines Vollbild-DMA-Pushs
python tools/watchctl.py gestures                      # Navigationstest mit synthetischen Gesten
```

Der Screenshot-Empfang prüft die `SNAP w h stride`-Kopfzeile, liest exakt `stride·h` Bytes und verifiziert die `END`-Marke; bei Störungen durch Hintergrund-Logs wird wiederholt.

### `tools/upload_map_sd.py`

Überträgt eine `.knav`-Kartendatei per USB-Seriell auf die microSD-Karte der Uhr (`/kronos/germany.knav`). Aufruf und Optionen: `python tools/upload_map_sd.py --help`.

### `tools/build_germany_map.cpp`

Host-Programm (C++17), das aus einem OSM-PBF-Extrakt die Kartendatei erzeugt (Zellen, POIs, Routing-Graph). Das Binärformat ist in `include/MapFormat.h` definiert und wird von Firmware und Builder gemeinsam verwendet. Aufruf, Parameter und Bounding-Box: siehe `--help` bzw. Kopfkommentar der Datei.

### `tools/fonts/`

`build_fonts.py` erzeugt die Subsets von Orbitron und Rajdhani (`src/fonts/`), `build_icons.py` den Material-Symbols-Subset und `include/ui/Icons.h`.

---

## Projektstruktur

```
WatchOS-Ultra/
├── boards/lilygo-t-watch-ultra.json   # Board-Definition (Variante lilygo_twatch_ultra, 3 MB App)
├── include/
│   ├── Config.h                       # Produktname, Version, Enums, SystemSettings
│   ├── MapFormat.h                    # Kartenformat (Firmware + Builder)
│   ├── Compositor.h / KronosDisplay.h # RLE-Compositor, LVGL-Display-/Touch-Treiber
│   ├── DisplayEngine.h                # Seitenaufbau, Overlays, Debug-Konsole
│   ├── NavEngine.h                    # Karte, Routing, Positionsquellen, GPX
│   ├── PowerManager.h / RtcManager.h  # PMU & Power-States, UTC-Uhr & Zeitzonen
│   ├── MotionHaptic.h / AudioEngine.h # Schritte, Haptik, I²S-Sounds
│   ├── GpsManager.h / RadioLoRa.h     # GNSS, SX1262 Meshtastic
│   ├── CompanionServer.h              # WLAN-AP, Portal, REST-API, OTA
│   ├── BleCompanion.h / AiLimitsStore.h
│   ├── SettingsStorage.h / NotificationStore.h
│   └── ui/                            # Navigator, UiKit, Pages, Icons
├── src/                               # Implementierungen, src/ui/pages/*, src/fonts/*
├── lib/
│   ├── LilyGoLib/                     # BSP, vendored Fork (siehe Fork notice in lib/LilyGoLib/README.MD)
│   ├── TinyGPSPlus/                   # von LilyGoLib/GPS.h benötigt
│   ├── NFC-RFAL-fork/, ST25R3916-fork/ # von der Board-Variante (USING_ST25R3916) eingebunden
├── tools/                             # watchctl.py, upload_map_sd.py, build_germany_map.cpp, fonts/
├── .github/workflows/build-firmware.yml
├── platformio.ini, build_firmware.sh, version.json
├── LICENSE, THIRD_PARTY_NOTICES.md
├── REVIEW.md                          # Code-Review, Grundlage der Überarbeitung 1.1.0
└── REVIEW-2.md                        # Review 2: Akku, Touch, App-Drawer, Start (1.2.0)
```

---

## Bekannte Einschränkungen

- Energie: CPU-Takt nach Zustand (240/160/80 MHz), Panel-Sleep bei dunklem Display, LoRa-RX im Duty-Cycle, sparsame BLE-Intervalle. Light Sleep greift nur, solange Bluetooth aus ist (Energiesparmodus in der Energie-App): die vorkompilierten Arduino-Bibliotheken haben kein BT-Modem-Sleep. Details in `REVIEW-2.md`.
- Der Routing-Graph ist rasterbasiert (Zellen 0,5–2,4 km), nicht topologisch; Routen sind Näherungen.
- Straßen- und Gebäudeebene der Karte nur im Raum Stuttgart; außerhalb Autobahn bis Kreisstraße, Bahn, Gewässer.
- Nur 24-h-Anzeige, keine 12-h-Uhr.
- Companion-App (BLE-Kalender, AI-Limits) ist nicht im Repo enthalten.
- Keine automatisierten Tests; Lade­strom wird nicht gemessen (AXP2101 ohne entsprechenden ADC), Distanz/Kalorien sind Schätzungen.
- Kein Secure Boot / keine OTA-Signaturprüfung; Schutz ist PIN + Bestätigung auf der Uhr.

---

## Lizenz

MIT, siehe [LICENSE](LICENSE). Lizenzen der eingebetteten Bibliotheken und Fonts: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Hardware: [LilyGO](https://github.com/Xinyuan-LilyGO).
