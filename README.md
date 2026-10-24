# WatchOS Ultra

Eine eigenständige, hochmoderne Firmware und Betriebssystem für die **LilyGO T-Watch Ultra** (ESP32-S3). WatchOS Ultra nutzt die gesamte Hardware der Next-Gen Smartwatch voll aus – angetrieben von einem hochauflösenden **CO5300 QSPI AMOLED-Display (410×502)**, **LVGL 9.4**, FreeRTOS Dual-Core Architektur, **Semtech SX1262 LoRa-Funk**, **u-blox Satelliten-GNSS**, **BHI260AP Bewegungssensorik**, **DRV2605 Haptik**, I²S-Audio und einem integrierten **Web Companion Dashboard**.

Analog zum Schwesterprojekt [WatchOS (für die T-Watch 2020)](https://github.com/diggaknoxville/WatchOS) bietet WatchOS Ultra maximale Performance, ein futuristisches Interface und tiefe Hardware-Integration bei vollständiger Daten- und Zeitpersistenz.

---

## Inhalt

- [Hardware & Spezifikationen](#hardware--spezifikationen)
- [Systemarchitektur & Performance](#systemarchitektur--performance)
- [Funktionsübersicht](#funktionsübersicht)
  - [Zifferblätter & UI-Engine](#zifferblätter--ui-engine)
  - [Gesten- & Kachel-Navigation](#gesten--kachel-navigation)
  - [Sensorik & Fitness (BHI260AP)](#sensorik--fitness-bhi260ap)
  - [LoRa Mesh-Messenger (SX1262)](#lora-mesh-messenger-sx1262)
  - [Satellitennavigation & GPS-Zeit (u-blox)](#satellitennavigation--gps-zeit-u-blox)
  - [Power-Management & AOD (AXP2101)](#power-management--aod-axp2101)
  - [Audio-Engine & Haptik (DRV2605 + I²S)](#audio-engine--haptik-drv2605--i2s)
  - [Phone Sync & Web Companion Portal](#phone-sync--web-companion-portal)
- [Bauen & Flashen](#bauen--flashen)
  - [PlatformIO](#platformio)
  - [Build-Skript (`build_firmware.sh`)](#build-skript-build_firmwaresh)
  - [Host-Tool (`watchctl.py`)](#host-tool-watchctlpy)
- [Projektstruktur](#projektstruktur)
- [Pinbelegung & Hardware-Referenz](#pinbelegung--hardware-referenz)
- [Lizenz & Credits](#lizenz--credits)

---

## Hardware & Spezifikationen

Zielplattform ist die **LilyGO T-Watch Ultra** (inkl. SX1262 LoRa- und GNSS-Subsystem):

| Komponente | Baustein / Spezifikation | Anbindung / Bus |
| :--- | :--- | :--- |
| **SoC** | ESP32-S3 (Dual-Core Xtensa LX7 @ 240 MHz) | 16 MB Flash, 8 MB Octal PSRAM |
| **Display** | 1,75" CO5300 Ultra-HD AMOLED (410×502 px, 326 PPI) | High-Speed QSPI via `LilyGoLib` |
| **Touchscreen** | FocalTech FT3168 kapazitiver Multi-Touch | I²C `Wire` (Interrupt-gesteuert) |
| **Echtzeituhr (RTC)** | NXP PCF85063 (batteriegepuffert) | I²C `Wire` (GPIO 3 / GPIO 2) |
| **Power Management** | X-Powers AXP2101 PMU & Coulomb Counter | I²C `Wire` (Ladeüberwachung, DCDC/LDO) |
| **Sensor-Hub (IMU)** | Bosch BHI260AP (6-Achsen Gyro/Accel + DSP) | I²C `Wire` (Pedometer, Wrist-Wake) |
| **Haptischer Treiber** | TI DRV2605L LRA Linear Resonant Actuator | I²C `Wire` (ROM Haptic Effects) |
| **LoRa-Transceiver** | Semtech SX1262 (868 MHz / 915 MHz) | High-Speed SPI + IRQ |
| **Satelliten-GNSS** | u-blox GNSS-Modul (GPS/GLONASS/Galileo) | UART (Hardware Serial) |
| **Audio-DAC** | Maxim MAX98357A Class-D I²S Verstärker | I²S TX |
| **NFC / RFID** | STMicroelectronics ST25R3916 Transceiver | SPI |
| **I/O-Expander** | NXP XL9555 16-Bit GPIO Expander | I²C `Wire` |

---

## Systemarchitektur & Performance

WatchOS Ultra setzt auf eine strikte **FreeRTOS Dual-Core Trennung**, um eine flüssige 60-FPS-Benutzeroberfläche ohne Ruckler bei gleichzeitiger Verarbeitung von Hintergrunddiensten zu gewährleisten:

```mermaid
graph TD
    subgraph Core 1: UI Compositor & Display Task
        LVGL[LVGL 9.4 Graphics Engine]
        Compositor[AMOLED Framebuffer Compositor]
        Navigator[Tile Navigator & Touch InDev]
        Faces[Watchfaces & UI Pages]
    end

    subgraph Core 0: Background Services Task
        PMU[PowerManager & AXP2101 Telemetry]
        IMU[MotionHaptic BHI260AP Service]
        GPS[GpsManager u-blox Service]
        LoRa[RadioLoRa SX1262 Mesh Engine]
        RTC[RtcManager PCF85063 Master Clock]
        WebServer[Companion Web Server & REST API]
    end

    SharedNVS[(Non-Volatile Storage NVS)]
    Hardware[[LilyGoLib Hardware Layer]]

    Core 0 --> SharedNVS
    Core 1 --> SharedNVS
    Core 0 --> Hardware
    Core 1 --> Hardware
```

- **Core 1 (`UiCompositor`)**: Führt die LVGL 9.4 Render-Pipeline, Touch-Erkennung, Kachel-Transitionen und Animationen mit voller Priorität aus.
- **Core 0 (`BgServices`)**: Kümmert sich asynchron im Hintergrund um Sensorauslesung, Satelliten-Fixes, LoRa-Paketempfang, Batterietelemetrie und den WiFi-Webserver.
- **Speicherpersistenz**: Sämtliche Benutzereinstellungen (Zifferblatt, Helligkeit, Display-Timeouts, Alarme, Zeitzone, Schrittziele) werden direkt im ESP32 NVS gespeichert und überstehen Stromausfälle und Neustarts.

---

## Funktionsübersicht

### Zifferblätter & UI-Engine

Die UI basiert auf einem hochauflösenden Cyberpunk-Design mit sattem AMOLED-Schwarz (Zero-Power-Pixel):

1. **Modular Digital Hero Face:** Übergroße Digitaluhr, Wochentag/Datum, Live-Aktivitätsbalken (Schrittziel), Akku-Prozentanzeige mit Lade-Blitz, LoRa- und Satelliten-Statusbadges.
2. **Chronograph Face:** Analoges High-Precision-Zifferblatt mit LVGL 9 Scale-Widgets, dynamischem Stunden-, Minuten- und Sekundenzeiger sowie Hilfszifferblättern.
3. **Minimal Typo Face:** Typografisches Zifferblatt für ablenkungsfreie Ablesbarkeit und extremen Stromsparmodus.

### Gesten- & Kachel-Navigation

Intuitive Navigation über das gesamte AMOLED-Panel:
- **Horizontal Wischen:**
  - `Tile [0, 0]` : **Watchface** (Hauptansicht)
  - `Tile [1, 0]` : **AI Limits & Quota** (Anthropic, Antigravity IDE & Google Cloud Limits via BLE)
  - `Tile [2, 0]` : **Aktivität & Fitness** (Schritte, Distanz, Kalorien, Zielerreichung)
  - `Tile [3, 0]` : **Position & GNSS** (Breitengrad, Längengrad, Höhe, Satellitenanzahl, GPS-Fix)
  - `Tile [4, 0]` : **LoRa Mesh** (Paket-Counter, RSSI/SNR-Signalstärke, Ping-Sender)
  - `Tile [5, 0]` : **System & Hardware** (Akkuspannung, Ladestrom, RAM/PSRAM-Auslastung, Uptime)
  - `Tile [6, 0]` : **Connect & Phone Sync** (WiFi AP Status, IP-Adresse, Web-Dashboard QR)
- **Vertikal nach unten wischen:**
  - `Tile [0, 1]` : **Control Center** (Quick-Toggles für Helligkeitsslider, Always-On-Display, Wrist-Wake, Haptik, WiFi-Hotspot, Sounds)

### Sensorik & Fitness (BHI260AP)
- Hardware-beschleunigtes Pedometer über den integrierten DSP des BHI260AP.
- Automatische Handgelenkserkennung (**Wrist-Wake**): Display schaltet sich bei Anheben des Arms ein.
- 6-Achsen Lagedaten und Schrittverlauf.

### LoRa Mesh-Messenger (SX1262)
- Vollständige Integration des Semtech SX1262 Transceivers auf 868 MHz (bzw. 915 MHz).
- Interrupt-gesteuerter Paketempfang im Hintergrund.
- On-Watch Ping-Funktion und Telemetrie (Signalstärke in dBm, SNR).

### Satellitennavigation & GPS-Zeit (u-blox)
- U-blox Multi-GNSS Empfänger mit NMEA-Parser.
- **Automatische Zeitdisziplinierung:** Die interne RTC (PCF85063) wird bei vorhandenem Satelliten-Fix automatisch auf atomuhrgenaue UTC-Zeit synchronisiert.
- Anzeige von Echtzeit-Koordinaten und Höhenprofil.

### Power-Management & AOD (AXP2101)
- Mehrstufiges Energiekonzept:
  - **Aktivmodus (240 MHz):** Maximale Performance bei Benutzerinteraktion.
  - **Dim-Modus (80 MHz):** Automatische Dimmung vor dem Standby.
  - **Always-On-Display (AOD):** Stromsparendes Minimal-Zifferblatt auf AMOLED-Basis.
  - **Deep/Light Sleep:** Display aus, Sensoren auf Low-Power-Interrupt, Aufwecken durch Berührung oder Handgelenksbewegung.

### Audio-Engine & Haptik (DRV2605 + I²S)
- Präzise haptische Rückmeldung über den TI DRV2605 LRA-Treiber (Klick-, Erfolgs- und Warnimpulse).
- Kristallklare Audiosignale über den MAX98357A I²S-Verstärker (Boot-Sound, Benachrichtigungstöne, Timer-Alarm).

### Phone Sync & Web Companion Portal
- Die Uhr spannt auf Wunsch einen eigenen WLAN-Access-Point auf (`WATCHOS-ULTRA` / `VORTEX-WATCH`).
- Über jedes Smartphone oder PC im Browser (`http://192.168.4.1`) erreichbar.
- Responsives Dashboard mit Glassmorphism-Design zur Steuerung von:
  - Zifferblatt-Auswahl & Akzentfarben
  - Display-Helligkeit & Timeout
  - Zeitzonen-Konfiguration (inkl. Sommerzeitregeln)
  - Alarmen, Schrittzielen und Hardware-Telemetrie

---

## Bauen & Flashen

### PlatformIO

Das Projekt ist für **PlatformIO** mit dem modernen `platform-espressif32` (pioarduino Core 3.3+) optimiert:

```bash
# 1. Repository klonen
git clone https://github.com/diggaknoxville/WatchOS-Ultra.git
cd WatchOS-Ultra

# 2. Firmware kompilieren
pio run

# 3. Uhr per USB anschließen und flashen
pio run -t upload

# 4. Seriellen Monitor öffnen
pio device monitor -b 115200
```

### Build-Skript (`build_firmware.sh`)

Ein praktisches Bash-Skript bündelt alle häufigen Aufgaben:

```bash
chmod +x build_firmware.sh

./build_firmware.sh build      # Kompilieren
./build_firmware.sh upload     # Flashen auf die Watch
./build_firmware.sh monitor    # Serieller Monitor
./build_firmware.sh sync-time  # Zeit & Zeitzone vom PC synchronisieren
./build_firmware.sh tour       # Screenshots aller Seiten erstellen
```

### Host-Tool (`watchctl.py`)

Über das mitgelieferte Python-Tool `tools/watchctl.py` kann die Uhr direkt über den USB-CDC-Port administriert werden:

```bash
# PC-Systemzeit und Zeitzone sofort auf die RTC der Uhr übertragen:
python3 tools/watchctl.py time

# Screenshot des aktuellen Displays als PNG speichern:
python3 tools/watchctl.py shot display.png

# Screenshots sämtlicher UI-Kacheln automatisiert erstellen:
python3 tools/watchctl.py tour ./screenshots/
```

---

## Projektstruktur

```
WatchOS-Ultra/
├── boards/
│   └── lilygo-t-watch-ultra.json     # Board-Definition für PlatformIO
├── include/                          # Header & Schnittstellen
│   ├── Config.h                      # Globale Systemkonfiguration & Farbcodes
│   ├── DisplayEngine.h               # Display- & UI-Fassade
│   ├── KronosDisplay.h               # LVGL 9 Driver & Framebuffer
│   ├── PowerManager.h                # AXP2101 PMU & Helligkeitssteuerung
│   ├── RtcManager.h                  # PCF85063 RTC & Zeitzonen-Handling
│   ├── MotionHaptic.h                # BHI260AP IMU & DRV2605 Haptik
│   ├── GpsManager.h                  # u-blox GNSS-Dienst
│   ├── RadioLoRa.h                   # Semtech SX1262 LoRa-Treiber
│   ├── AudioEngine.h                 # MAX98357A I²S Soundausgabe
│   ├── CompanionServer.h             # Phone Web Companion Portal
│   ├── NotificationStore.h           # Persistenter Nachrichtenspeicher
│   └── ui/
│       ├── Navigator.h               # Kachel- & Gestennavigation
│       ├── UiKit.h                   # Typografie, Farben & UI-Komponenten
│       ├── Pages.h                   # Kachel-Deklarationen
│       └── Icons.h                   # Material Symbols Icon Font
├── src/                              # Implementierung
│   ├── main.cpp                      # FreeRTOS Dual-Core Initialisierung
│   ├── DisplayEngine.cpp
│   ├── KronosDisplay.cpp
│   ├── PowerManager.cpp
│   ├── RtcManager.cpp
│   ├── MotionHaptic.cpp
│   ├── GpsManager.cpp
│   ├── RadioLoRa.cpp
│   ├── AudioEngine.cpp
│   ├── CompanionServer.cpp
│   ├── SettingsStorage.cpp
│   ├── NotificationStore.cpp
│   └── ui/
│       ├── Navigator.cpp             # Touch-Gesten & Kachelübergänge
│       ├── UiKit.cpp                 # Widgets & Themes
│       └── pages/                    # UI-Kacheln (Watchface, Activity, GPS, LoRa, Control, ...)
├── lib/                              # Board Support Package & Bibliotheken
│   ├── LilyGoLib/                    # T-Watch Ultra BSP
│   ├── TinyGPSPlus/                  # NMEA GPS Parser
│   ├── Adafruit_BusIO/
│   ├── Adafruit_TCA8418/
│   ├── NFC-RFAL-fork/                # ST25R3916 NFC Treiber
│   └── ST25R3916-fork/
├── tools/                            # Entwickler- & Host-Werkzeuge
│   ├── watchctl.py                   # Time-Sync & Screenshot Tool
│   └── fonts/                        # Font- & Icon-Generator-Skripte
├── platformio.ini                    # PlatformIO Build-Konfiguration
├── build_firmware.sh                 # All-in-One Build-Skript
├── version.json                      # Release- & Versionsinformationen
└── .github/workflows/
    └── build-firmware.yml            # CI GitHub Action Workflow
```

---

## Pinbelegung & Hardware-Referenz

| Peripherie | Pin / Signal | Anmerkung |
| :--- | :--- | :--- |
| **I²C Hauptbus** | SDA: GPIO 3 / SCL: GPIO 2 | AXP2101, PCF85063, DRV2605, BHI260AP, XL9555 |
| **QSPI AMOLED** | SCK: 18, CS: 12, D0-D3: 13,11,17,10 | CO5300 QSPI Bus |
| **Touchscreen** | SDA: GPIO 3 / SCL: GPIO 2, INT: Expander | FT3168 I²C |
| **LoRa (SX1262)** | MOSI: 41, MISO: 40, SCLK: 39, CS: 42, IRQ: 46 | SPI Bus |
| **GNSS (GPS)** | RX: GPIO 44, TX: GPIO 43 | Hardware Serial 2 |
| **I²S Audio DAC** | BCLK: GPIO 15, LRCLK: GPIO 16, DOUT: GPIO 7 | MAX98357A |
| **NFC (ST25R3916)**| MOSI: 41, MISO: 40, SCLK: 39, CS: Expander | SPI Bus |

---

## Lizenz & Credits

Entwickelt für die Open-Source-Community und Maker. 

- **Hardware:** [LilyGO](https://github.com/Xinyuan-LilyGO) (T-Watch Ultra)
- **Grafikbibliothek:** [LVGL](https://lvgl.io/) (Light and Versatile Graphics Library)
- **Funk:** [RadioLib](https://github.com/jgromes/RadioLib) von Jan Gromes
- **Sensoren:** [SensorLib & XPowersLib](https://github.com/lewisxhe) von Lewis He
- **Schwesterprojekt:** [WatchOS (für T-Watch 2020)](https://github.com/diggaknoxville/WatchOS)

