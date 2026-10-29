# WatchOS Ultra: Intent & Architecture Specification (INTENT.md)

Dieses Dokument definiert die Kernziele, die Architektur, die funktionalen und visuellen Qualitätsmaßstäbe sowie den Soll-Zustand der Firmware für die **LilyGO T-Watch Ultra**.

---

## 1. Vision & Produkt-Charakter

WatchOS Ultra ist als hochperformantes, zuverlässiges und autonomes Expeditions- und Alltags-Instrument für das Handgelenk konzipiert.
Es vereint modernes Smartwatch-Interaktionsdesign (Apple Human Interface Guidelines, vsync-synchronisierte 60-fps-Gestensteuerung) mit autarker Outdoor-Tauglichkeit (Offline-Vektorkartografie, GNSS-Navigation, Meshtastic-kompatibler LoRa-Mesh-Funk, ehrliche Sensortelemetrie ohne Cloud-Zwang).

---

## 2. Hardware- & Systemarchitektur

### 2.1 Hardware-Plattform
- **SoC:** ESP32-S3 (2× Xtensa LX7 bis 240 MHz, 16 MB Flash, 8 MB Octal-PSRAM).
- **Display:** 1,75" CO5300 AMOLED, 410 × 502 px, QSPI-DMA mit Hardware Tearing-Effect (TE) Pin 6 Synchronisation.
- **Touch & Tasten:** CST9217 kapazitiver Multi-Touch, XL9555 Expander, Hardware-Seitentaste (Home/Power) und Action-Taste (GPIO 0).
- **Energie & Batterie:** X-Powers AXP2101 PMU, programmierbare Lade- und Spannungslimits, VBUS-Erkennung, LiPo-Spannungsüberwachung.
- **Master-Uhr:** NXP PCF85063 RTC, batteriegepuffert, stetig synchronisiert in UTC, zeitzonenbewusst über POSIX-Regeln.
- **Sensoren:** Bosch BHI260AP Sensor-Hub (Schrittzähler mit Kadenzfilterung, Aktivitätserkennung, Wrist-Tilt, 6-Achsen-Rotationsvektor).
- **Haptik & Audio:** TI DRV2605 LRA-Treiber mit Effekt-Wellenformen; MAX98357A I2S Class-D Audio-Verstärker.
- **Konnektivität & Speicher:** Semtech SX1262 LoRa (EU868 Meshtastic), u-blox GNSS, Wi-Fi 802.11 b/g/n, BLE 5.0 (NimBLE), MicroSD-Kartenleser.

### 2.2 Dual-Core-Task-Topologie
```
┌────────────────────────────────────────────────────────┐
│ Core 1: UiCompositor (28 KB Stack, Priorität 2)        │
│ - LVGL 9.4 Engine                                      │
│ - RLE Surface Compositor (60 fps, TE-gelockt)          │
│ - Navigator: Gestenklassifikation, physikalischer Schwung│
│ - DisplayEngine: Seitensteuerung, Overlays, Konsole     │
└────────────────────────────────────────────────────────┘
                           ▲ I2C-Mutex / Ringbuffer
┌────────────────────────────────────────────────────────┐
│ Core 0: BgServices (16 KB Stack, Priorität 1)          │
│ - CompanionServer: Webportal, Captive AP, OTA, Web-Sync│
│ - BleCompanion: NimBLE Stack, Media- & Kalender-Queue  │
│ - PowerManager: Dynamische CPU-Taktskalierung, Schlaf  │
│ - MotionHaptic: BHI260AP-Polling & DRV2605-Warteschlange│
│ - NavEngine: Asynchrones Kachel- & Vektor-Routing       │
│ - RadioLoRa: Meshtastic SX1262 Paketabwicklung         │
│ - Storage: Verzögerte, differenzielle NVS-Persistenz   │
└────────────────────────────────────────────────────────┘
```

---

## 3. Funktionale Kernanforderungen

1. **Zifferblätter & Personalisierung:**
   - 14 vollständig ausgearbeitete Zifferblätter (Modular, Chronometer, Minimal, Wayfinder Ultra, Chrono Master GT, Casio Vintage, Bauhaus, Cyberpunk HUD, Wortuhr, Solar Orbit, Pilot Flieger, Diver Pro, Radar Tactical, Lifetime 52).
   - Sofortige Zifferblatt-Umschaltung ohne Render-Artefakte oder Text-Überlappungen.
   - Wayfinder mit kontinuierlich mitdrehendem Kompassring und Kursmarkierung.

2. **Drei-Ringe-Detailansichten (Schritte, Akku, Tag):**
   - Eigene, tiefe Detailansichten erreichbar durch Antippen der jeweiligen Komplikation.
   - Schritte: Echter 7-Tage-Verlauf mit Kadenzauswertung, Bewegungsart und gefilterten Fehlschritten.
   - Akku: Echte PMU-Spannung, Ladestatus, Temperatur, USB-Versorgung und Energiesparmodus.
   - Tag: NOAA-basierte Sonnenzeiten für die GPS-Position, Mondphase, Jahreszeit und 12h/24h-Formatumschaltung.
   - Saubere Scrollbarkeit ohne Fehltriggerung von Control Center oder Benachrichtigungen.

3. **Autonome Konnektivität & AI-Limits:**
   - Standalone-Webportal über Hotspot (mit QR-Code-Direktverbindung) zur Verwaltung und Anzeige aller Einstellungen.
   - Direkte Synchronisation und Konfiguration von AI-Limits (Claude/Anthropic, Gemini/Google, OpenAI, DeepSeek, Grok) direkt im Webportal ohne zwingende Companion-App.
   - Prominente, zentrale Warnanzeige auf der Uhr bei veralteten Daten.

4. **Karten & Outdoor-Navigation:**
   - Vektordarstellung mit Antialiasing, klaren Straßenhierarchien, Gewässern, topografischer Klarheit und rotierender Kartenausrichtung.
   - Echte GNSS-Routenführung mit Turn-by-Turn-Hinweisen oder ehrlicher Luftlinie ohne Fix.
   - GPX-Trackaufzeichnung und Backtrack-Funktion auf MicroSD.

5. **Spiele & Interaktion:**
   - 9 Spiele (Labyrinth, Breakout, Würfel-Duell, Balance, Snake, 2048, Reaktion, Memory, Flappy) mit physikalisch korrekter Neigungssteuerung (Kugel rollt hangabwärts) und ansprechendem Arcade-Design.

6. **Dienstprogramme & System:**
   - Vollwertiger Taschenrechner mit Klammerlogik, Prozentrechnung und wissenschaftlichen Funktionen.
   - Control Center mit Statusanzeigen für alle Funkstandards, Schnellzugriff auf Einstellungen und Mediensteuerung.

---

## 4. Visuelle & Qualitative Maßstäbe (HIG-Konformität)

- **Barrierefreiheit & Kontrast:** Minimaler Textkontrast von 4.5:1 (Tertiärtext mindestens 5.6:1 auf Schwarz).
- **Trefferflächen:** Bedienelemente mindestens 44 × 44 pt (88 × 88 px) oder unsichtbar erweitert auf 56 px via `pressable()`.
- **Ehrliche Telemetrie:** Keine erfundenen Pseudo-Messwerte. Wenn Sensordaten (GNSS, BLE, Stromsensor) fehlen, wird dies transparent deklariert.
- **Flüssigkeit & Boot-Erlebnis:** 60 fps Animationen; nach dem Boot-Splash direkte Überblendung in das fertige Zifferblatt ohne Flackern oder segmentweisen Neuaufbau.

