# Code-Review: WatchOS-Ultra

Repository: https://github.com/diggaknoxville/WatchOS-Ultra
Stand: Commit `6fd153e` (2026-10-25), 10 Commits, erstellt innerhalb von zwei Tagen.
Zielplattform: LilyGO T-Watch Ultra (ESP32-S3), PlatformIO, Arduino-Core 3.x, LVGL 9.4, FreeRTOS.

Umfang: ca. 15.300 Zeilen eigener Code in `src/`, `include/` und `tools/`. Der Rest des Repos (ca. 70.000 Zeilen) sind vendored Bibliotheken in `lib/`, die nur auf Veränderungen und Nutzung geprüft wurden. Das Review ist statisch; ein Build konnte in dieser Umgebung nicht ausgeführt werden (kein PlatformIO, keine Hardware).

---

## Gesamturteil

Das Projekt ist ein technisch ambitionierter Prototyp mit einigen wirklich guten Teilen (RLE-Compositor, saubere LVGL-Single-Task-Disziplin, korrekte Zeitzonen-/DST-Logik, echter Meshtastic-Empfang) und einer langen Liste von Problemen, die es **nicht releasefähig** machen:

1. **Sicherheit:** Hart codiertes, für alle Geräte identisches WLAN-Passwort, unauthentifizierte REST-API inklusive OTA-Firmware-Upload ohne Signatur, BLE-Characteristics ohne Pairing-Pflicht. Jeder in Funkreichweite kann beliebige Firmware flashen.
2. **Stabilität:** Keine Synchronisation zwischen UI- und Hintergrund-Task in der Navigations-Engine (Use-after-free bei Karten-Pan während GPX-Aufzeichnung), unaligned 32-Bit-Zugriffe auf BLE-Puffer, Stack-Pointer-Bug im Bauhaus-Zifferblatt, A* schreibt bei beschädigter Kartendatei außerhalb der Puffer.
3. **Hardware-Risiko:** Die Firmware initialisiert bei fehlgeschlagenem SD-Schreibtest die Karte roh neu und beschreibt bei gesetzten Write-Protect-Bits das CSD-Register. Audio-Task schreibt PMU-Register ohne I2C-Mutex.
4. **Ehrlichkeit der Features:** Viele beworbene Funktionen sind weniger, als Commit-Messages, README und UI behaupten: "Hybrid Positioning" übernimmt die Position des lautesten LoRa-Nachbarn, "Adaptive Activity Learning" lernt nichts, das benutzt wird, "YouTube App" ist eine BLE-Fernbedienung, Auto-/Fußgänger-Routing liefert identische Routen, mehrere Zifferblätter und die AI-Limits-Seite zeigen erfundene Werte.
5. **Prozess:** Keine Tests, kein LICENSE, README veraltet (3 statt 14 Zifferblätter, falsche Pins, falscher Touch-Controller), Host-Tooling zur Hälfte nicht lauffähig, CI ohne Artefakt, vendored LilyGoLib still gepatcht.

Empfohlene Reihenfolge: Sicherheit (Abschnitt 1), dann Abstürze/Hardware (Abschnitt 2), dann Protokoll- und Logikfehler (Abschnitt 3). Alles darunter ist Qualitätsarbeit.

---

## 1. Kritisch: Sicherheit

| # | Befund | Fundstelle |
|---|---|---|
| S1 | **OTA-Upload ohne jede Authentifizierung und ohne Signatur-/Hash-Prüfung.** `/api/ota/upload` nimmt jede Multipart-Datei, schreibt sie per `Update.write()` und startet neu. `Update.end(true)` prüft nur das Image-Magic. | `src/CompanionServer.cpp:264-289` |
| S2 | **AP-Passwort hart codiert, für alle Geräte identisch, im Repo veröffentlicht** und auf der Uhr als Klartext und QR-Code angezeigt. SSID-Präfix `VORTEX-WATCH-` macht Geräte erkennbar. Mit S1 zusammen: Vollübernahme aus WLAN-Reichweite per `curl -F file=@evil.bin`. | `include/Config.h:16`, `src/ui/pages/AppsSmall.cpp:182-189` |
| S3 | **Gesamte REST-API ohne Auth und CSRF-fähig.** Settings, Zeit, Notify, LoRa-Senden (inkl. SOS), Kalender, Media, Karten-Upload auf SD. Alle POSTs sind "simple requests" ohne CORS-Preflight, also von jeder Webseite im Browser des verbundenen Telefons auslösbar. | `src/CompanionServer.cpp:252-303` |
| S4 | **BLE-Characteristics ohne `WRITE_ENC`/`WRITE_AUTHEN`.** `setSecurityAuth(true,true,true)` ist wirkungslos, weil kein Attribut Verschlüsselung verlangt. MITM=true mit `NO_INPUT_OUTPUT` ist widersprüchlich. Jeder Central kann Kalender, AI-Limits und Media-State schreiben und den letzten Kalender-Payload lesen. | `src/BleCompanion.cpp:192-213` |
| S5 | **Fake-Position wird als echtes Positions-Beacon ins Mesh gesendet.** Ohne GPS-Fix sendet `sendPositionBeacon()` die Stuttgarter Schlossplatz-Koordinaten als POSITION_APP-Paket. | `src/RadioLoRa.cpp:872-873` |
| S6 | **Mesh-Peers sind spoofbar und überschreiben die eigene Position.** `getMeshFallbackPosition()` liefert die Position des Peers mit höchstem RSSI ohne Alterscheck; in `updateCurrentPosition()` hat sie Vorrang vor dem eigenen gespeicherten GPS-Fix. Peers werden in NVS persistiert und überleben Reboots. Namens-Match in `updatePeer` erlaubt "Verschieben" bekannter Peers. | `src/RadioLoRa.cpp:975-996`, `src/NavEngine.cpp:426-451` |
| S7 | JSON-Ausgaben ohne Escaping für `title`, `loc`, `artist` (JSON-Injection). `jsonEscape()` existiert, wird aber nur in `status`/`inbox` benutzt. | `src/CompanionServer.cpp:504, 550-551` |
| S8 | HTTP-Body und SD-Upload ohne Größenlimit (Heap-DoS, SD voll). | `src/CompanionServer.cpp:461, 490` |

**Fix-Richtung:** Pro-Gerät-Passwort aus eFuse-MAC oder Zufall beim ersten Boot, nur auf der Uhr anzeigen. Session-Token für die API, `Origin`-Prüfung, `application/json` erzwingen. OTA nur nach Bestätigung auf der Uhr und mit Signaturprüfung (ESP-IDF Secure Boot v2 oder mindestens SHA256-Vergleich). BLE auf `WRITE_ENC | WRITE_AUTHEN` mit Passkey-Display. Kein Position-TX ohne `hasLastFix()`. Peer-Positionen nie als Navigationsursprung, nur als "Nähe von X" mit Alter.

---

## 2. Kritisch/Hoch: Abstürze, Datenverlust, Hardware

| # | Befund | Fundstelle |
|---|---|---|
| A1 | **NavEngine: geteilter Zustand ohne jede Synchronisation.** `sGpxTrail.push_back` (Bg-Task) vs. Iteration in `renderMap` (UI-Task); `sRoute.clear()` (UI) vs. `updateTurnByTurn` (Bg); `clearCellCache()` bei WiFi-Upload (Bg) vs. Cache-Zeiger in `renderMap` (UI). Use-after-free bei Karten-Pan während GPX-Aufzeichnung. | `src/NavEngine.cpp:694, 744, 831, 617-660, 1636ff, 461, 1722` |
| A2 | **SD-Karte: rohe Neuinitialisierung und CSD-Programmierung.** Bei jedem fehlgeschlagenen Schreibtest (auch bei voller FAT oder SPI-Kollision) wird SD deinitialisiert, per CMD0/CMD8/ACMD41 roh neu gestartet, und falls die CSD Write-Protect-Bits meldet, per CMD27 beschrieben, inklusive Versuch das OTP-Bit `PERM_WRITE_PROTECT` zu löschen. Danach Remount mit 16 MHz statt der 4 MHz, die LilyGoLib für den geteilten Bus vorsieht. Läuft beim Boot im UI-Task. | `src/NavEngine.cpp:221-305, 471-476` |
| A3 | **A* ohne Bounds-Checks auf SD-Daten.** `readAt`-Rückgabewerte ignoriert, `firstEdge + k` und `e.toNode` nicht gegen `numRouteEdges`/`numRouteNodes` geprüft, `numRouteNodes * 16` kann überlaufen. Abgebrochener Upload mit intaktem Header führt zu wilden PSRAM-Schreibzugriffen. | `src/NavEngine.cpp:750-812` |
| A4 | **Unaligned 32-Bit-Zugriffe auf angreifer-kontrollierte Puffer.** `*(const uint32_t*)(bytes + off)` mit `off=3,59,115…`. Auf Xtensa je nach Speicherbereich `LoadStoreAlignment`-Exception, in jedem Fall Strict-Aliasing-UB. | `src/BleCompanion.cpp:128-129`, `src/RadioLoRa.cpp:382-384` |
| A5 | **Bauhaus-Zifferblatt: `lv_line_set_points` mit Stack-Array.** `lv_point_precise_t pts[2]` ist schleifenlokal, LVGL speichert nur den Zeiger. Alle 12 Stundenbatons zeigen nach dem Builder auf toten Stack. | `src/ui/pages/FacePage.cpp:569-576` |
| A6 | **Audio-Task schreibt AXP2101-Register ohne I2C-Mutex.** `powerControl(POWER_SPEAK)` ist ein Read-Modify-Write auf Register 0x90, in dem auch Display-, Radio-, GPS- und Sensor-Rails liegen. Parallel läuft Telemetrie/GPS-Rail-Steuerung unter Lock. Lost Update möglich. | `src/AudioEngine.cpp:53, 64` |
| A7 | **RTC-Zeitrückfall bei transientem I2C-Lock-Timeout.** Schlägt `lockI2C(30)` fehl, bleibt `dt` default-konstruiert (Jahr 0), die Validitätsprüfung scheitert, und der "RTC lost power"-Zweig schreibt die bis zu 10 Minuten alte `last_epoch` per `settimeofday` **und** zurück in die RTC. | `src/RtcManager.cpp:158-183` |
| A8 | **Blockierende Flash-Writes und String-Parsing in Spinlock-Sections im NimBLE-Host-Task.** `onWrite` parst 512 Byte JSON innerhalb `portENTER_CRITICAL`, ruft dann NVS-Writes und `showNotification` → weitere NVS-Writes. Stack ca. 4 KB. | `src/BleCompanion.cpp:110-184` |
| A9 | **SPI-Bus-Race:** UI-Task zieht `LORA_CS` HIGH ohne Lock, während RadioLib auf Core 0 eine Transaktion fährt. Kann SX1262-Kommando abbrechen. Der Hack ist überflüssig, RadioLib lässt CS ohnehin HIGH. | `src/NavEngine.cpp:204, 222, 359, 1723` |
| A10 | **Karten-Rendering inklusive SD-Lesen bei jedem `LV_EVENT_PRESSING` (~70 Hz) im UI-Task**, parallel zu GPX-Schreibzugriffen auf Core 0, ohne Mutex. UI friert pro Pan-Event für ein volles Software-Rendering ein. | `src/ui/pages/PositionPage.cpp:428-441` |
| A11 | **NotificationStore schreibt bei jeder Notification einen 3,3-KB-Blob** in eine 20-KB-NVS-Partition, im Kontext des Aufrufers (oft UI-Task). Praktisch jede Notification löst Page-GC aus. `ESP_ERR_NVS_NOT_ENOUGH_SPACE` wird still verschluckt. 14 UI-Aufrufer, darunter jeder Control-Center-Toggle. | `src/NotificationStore.cpp:13-31, 59` |
| A12 | **NVS-Flash-Write bei jedem empfangenen LoRa-Paket** (`persistPeers` + `persistInbox`, zwei Blobs). In einem aktiven Mesh hunderte Writes pro Stunde. | `src/RadioLoRa.cpp:145, 440-806` |
| A13 | **Kompletter Routing-Graph wird bei jeder Zielwahl und jedem Moduswechsel von SD geladen** (3–5 MB bei 4 MHz SPI), im UI-Task. 10–15 s Einfrieren. `std::priority_queue` ohne Stale-Skip kann auf >5 MB wachsen. | `src/NavEngine.cpp:722-757, 783` |
| A14 | `radio.transmit()` blockiert den Bg-Task bis ca. 2 s (SF11/250 kHz), Spectrum-Sweep 24 Frequenzwechsel, BLE-Scan 1,5 s. Währenddessen GPS-UART-Overflow, HTTP, PMU-Loop stehen. Kein EU868-Duty-Cycle-Tracking. | `src/RadioLoRa.cpp:590, 645, 663, 765-789` |

---

## 3. Hoch: Protokoll- und Logikfehler

| # | Befund | Fundstelle |
|---|---|---|
| P1 | **Meshtastic-TX protokollinkompatibel.** `want_response` wird als Feld 6 kodiert; in `mesh.proto` ist `Data.want_response = 3`, Feld 6 ist `Data.id` (fixed32). Nanopb bricht bei Wire-Type-Mismatch ab. Alle drei TX-Pfade setzen `wantResponse=true`. **Folge: Kein von der Uhr gesendetes Paket kommt bei echten Meshtastic-Knoten an.** Die "Hub Discovery" kann so nie funktionieren. RX ist korrekt. | `src/RadioLoRa.cpp:560-564, 627, 638, 642` |
| P2 | MapReport (Portnum 73) mit falschen Feldnummern (6/7 statt 9/10). `hw_model = 43` ist vermutlich nicht T-Watch. | `src/RadioLoRa.cpp:508-511, 625` |
| P3 | **POI-Kategorien zwischen Builder und Firmware um 1 verschoben.** Builder: `POI_TRANSIT=1 … POI_STREET=8`. Firmware: `0=Transit … 7=StreetName`. Alle POI-Icons falsch, Straßennamen als Badge, Sehenswürdigkeiten als Straßennamen. | `tools/build_germany_map.cpp:58-68`, `src/NavEngine.cpp:1579-1612` |
| P4 | **A*-Modusgewichtung ist toter Code.** Builder schreibt `roadRank ∈ 0..4` in Bits 2+; Firmware vergleicht mit `FEAT_MOTORWAY(14)`, `FEAT_PRIMARY(13)`, 7, 8. Nie wahr. Car/Ped-Bits werden nirgends geprüft. **Auto- und Fußgänger-Modus liefern identische Routen**, Fußgänger über Autobahn, Autos über Fußwege. | `src/NavEngine.cpp:802-808`, `tools/build_germany_map.cpp:830-834` |
| P5 | **Zoom 13 im Stuttgart-Raum rendert keine Kartendaten.** Ultra-Tier nur bei ≤6 Zellen, Viewport braucht ~10; Fine/Coarse wird in der Metro übersprungen. Zoom 13 ist die Auto-Zoomstufe für 22–55 km/h. | `src/NavEngine.cpp:1481, 1507`, `src/ui/pages/PositionPage.cpp:77` |
| P6 | Routing-Graph ist rasterbasiert (Zellen 0,5–2,4 km), nicht topologisch: Straßen, die dieselbe Zelle berühren, gelten als verbunden. `motorway_link` nicht routable. | `tools/build_germany_map.cpp:318, 785-795` |
| P7 | Builder mit hart codierten PBF-Blockindizes (`firstWay = 55210; firstRel = 64132`) für einen bestimmten Snapshot, feste Pfade im Home-Verzeichnis eines Entwicklers, nicht-deterministische Ausgabe. | `tools/build_germany_map.cpp:207-208, 243-244` |
| P8 | **Fake-Route als Fallback inklusive erfundener Abbiegehinweise.** Ohne Karte oder im MESH-Modus wird eine Sinus-Kurve generiert, aus der "HALBRECHTS HALTEN", Distanz und ETA abgeleitet werden. | `src/NavEngine.cpp:843-852` |
| P9 | GPX-Dateien sind ungültiges XML (nie geschlossen, keine `<time>`), und loggen Default-/Peer-Positionen als eigenen Track. | `src/NavEngine.cpp:686-716` |
| P10 | **Navigator-Zustandsautomat:** App aus einem Sheet öffnen (Control → Media/Power) setzt `sBase` auf das Sheet, Rückweg bleibt in `M_APP`. Sheet-Schließgeste tot, Back animiert auf sich selbst. | `src/ui/Navigator.cpp:385-392, 460-472, 484-495` |
| P11 | **"Display off" im Power-Menü wirkungslos:** `POWER_SLEEP` wird im nächsten UI-Loop durch `lv_display_get_inactive_time() < 150` sofort wieder aufgehoben. | `src/ui/pages/AppsSmall.cpp:131-135`, `src/main.cpp:57-59` |
| P12 | **Kadenz-Gate misst Batch-Intervall statt Schrittkadenz:** Nach jeder Pause wird der erste Batch eines Gehabschnitts als "unplausibel" verworfen. Systematische Unterzählung bei Stop-and-go. | `src/MotionHaptic.cpp:266-283` |
| P13 | Zeit ist per Default "gültig": `last_epoch`-Default 1792753000 (2026-10-23) macht `isTimeValid()` nach RTC-Batterieverlust immer true; Fake-Datum wird in die RTC gebrannt. Drei inkonsistente Mindest-Epochen. | `src/SettingsStorage.cpp:123`, `src/RtcManager.cpp:12, 232` |
| P14 | Kalenderwoche falsch berechnet (keine ISO-8601; 30.12.2024 → 54). Das Modular-Face nutzt korrekt `strftime("%V")`. | `src/ui/pages/AppsSmall.cpp:249` |
| P15 | `face`-Parameter negativ → `toInt() % FACE_COUNT` = −1 → `(uint8_t)255`. Gleicher Fehler im Serial-Kommando `F`. | `src/CompanionServer.cpp:381`, `src/DisplayEngine.cpp:222` |

---

## 4. Hoch: Seiteneffekte, die Nutzereinstellungen überschreiben

Ein wiederkehrendes Muster: Seitenbuilder und `refresh()`-Funktionen schalten Funk ein.

| # | Befund | Fundstelle |
|---|---|---|
| E1 | **Connect-Seite erzwingt WiFi-AP bei jedem Boot.** `buildConnect` läuft in `DisplayEngine::init` für alle Seiten; `isApActive()` ist dann immer false → `requestAp(true)` + `wifiApEnabled = true` + `save()`. Nutzer-Setting wird überschrieben, AP mit bekanntem Passwort läuft dauerhaft. | `src/ui/pages/AppsSmall.cpp:165-169` |
| E2 | **LoRa-Seite schaltet Funk bei jedem Anzeigen ein.** `onShow` → `sendNodeInfoDiscovery` → `setEnabled(true)` wenn deaktiviert. Durchwischen des Carousels reicht. | `src/ui/pages/LoraPage.cpp:394-396`, `src/RadioLoRa.cpp:858-862` |
| E3 | **Jedes `send*` aktiviert LoRa**, auch `/api/lora` aus dem unauthentifizierten Portal. Einmalige Migration `v2_en` setzt `loraEnabled=true`. `injectDemoPeers()` heißt "no-op", sendet aber ein NodeInfo-Paket. | `src/RadioLoRa.cpp:253-258, 847-871, 998-1001` |
| E4 | **Spektrum-Scan wird durch Hintergrund-Prerender reaktiviert.** `refresh()` setzt `setSpectrumScanActive(sLoraTab == 1)`, läuft aber auch für unsichtbare Seiten. Nach jedem Sweep wird PG_LORA dirty → Endlosschleife, Mesh-RX blockiert. | `src/ui/pages/LoraPage.cpp:74`, `src/ui/Navigator.cpp:102, 696`, `src/RadioLoRa.cpp:788` |
| E5 | LoRa-TX bei jedem Seitenaufruf (LoraPage onShow, PositionPage Zielsuche). Kalender-Sync-Notify bei jedem onShow/Tab/Button. Kein Rate-Limit. | `src/ui/pages/LoraPage.cpp:394`, `PositionPage.cpp:203`, `AppsSmall.cpp:332, 409, 423-427` |
| E6 | **AOD/SLEEP rendern mit voller Framerate weiter.** UI-Task läuft mit 1–5 ms Delay unabhängig vom Power-State; das Face hat `refreshMs = 0`; LVGL zeichnet unter dem deckenden AOD-Overlay. `POWER_SLEEP` setzt nur Helligkeit 0. `Config.h` verspricht "CPU light sleep", README verspricht "Dim-Modus 80 MHz" und "Deep/Light Sleep"; `PowerManager.cpp:128-130` sagt explizit, dass `setCpuFrequencyMhz` nie aufgerufen wird. | `src/main.cpp:55-63`, `src/ui/pages/FacePage.cpp:1366`, `src/PowerManager.cpp:146-149` |

---

## 5. Erfundene oder vorgetäuschte Daten

Diese Punkte sind keine Bugs im engeren Sinn, aber sie untergraben das Vertrauen in alles, was die Uhr anzeigt.

| Was | Realität | Fundstelle |
|---|---|---|
| "Hybrid Positioning" (Commit-Message) | Position des Peers mit höchstem RSSI wird unverändert übernommen. Keine Trilateration, kein Distanzmodell, kein Alterscheck. | `src/RadioLoRa.cpp:975-996` |
| "Adaptive Activity Learning" | Einziges "gelerntes" Datum ist `learnedCadenceSpm` (EMA), das angezeigt, aber in keiner Entscheidung verwendet wird. Alle Gates sind Konstanten. | `src/MotionHaptic.cpp:19, 293, 396` |
| "Standalone YouTube App" | BLE-Fernbedienung: sendet `CMD:MEDIA:*`-Strings, zeigt vom Telefon gepushte Titel. Kein YouTube-API, kein Audio. Lautstärke ±5 wird lokal simuliert, auch ohne Verbindung. Default-Titel "YouTube Music". | `src/ui/pages/AppsSmall.cpp:632-796` |
| "BLE Google Calendar Sync" | Zeigt bis zu 6 Termine, die eine (nicht im Repo enthaltene) Companion-App per BLE-JSON liefert. "Sync" ist ein Notify-String. Monatsraster selbst ist korrekt. | `src/ui/pages/AppsSmall.cpp:202-429` |
| "Offline HD vector maps for Stuttgart & DE" | Straßen-/Gebäudeebene nur innerhalb 48.42–49.18 N / 8.75–9.75 E; außerhalb nur Autobahn bis Kreisstraße, Bahn, Flüsse. Ortsnamen auf der Karte stammen aus einer eingebauten Liste von Großstädten, nicht aus der Kartendatei. Deutschlandgrenze ist ein handgezeichnetes 42-Punkt-Polygon. | `src/NavEngine.cpp:121-176`, `tools/build_germany_map.cpp:883` |
| AI-Limits-Seite | Startet mit "sample data" (78 %, 42 %, 85 %), `has_data` wird in der UI nie geprüft. Nutzer sieht erfundene Quoten als real. | `src/AiLimitsStore.cpp:6-35`, `src/ui/pages/AiLimitsPage.cpp:51` |
| BLE "Threat Radar" | Apple Type 0x10 sendet jedes iPhone → "AIRTAG/FINDMY". `0xFEED` ist bei Tile eine Service-UUID, keine Company-ID → Zweig trifft nie. Samsung mit `mfg.size() > 8` = "SMARTTAG". Ergebnis: Liste aller BLE-Geräte mit zufälligen Tracker-Labels. | `src/RadioLoRa.cpp:678-703` |
| Wayfinder-Face | Ohne Fix: Münchner Koordinaten, "ELEV 520M • 1018 hPa" (es gibt keinen Barometer), "BEARING 034° NE" nie aktualisiert, obwohl `getHeadingDeg()` existiert. | `src/ui/pages/FacePage.cpp:322-344, 1142` |
| Lifetime-Face | `lifePct = 43` hart codiert (kein Geburtsdatum), Monat immer `/31`, Jahr `/365`. | `src/ui/pages/FacePage.cpp:1303-1315` |
| Cyberpunk/Radar/Neon-Faces | "MESH: OK" unabhängig vom Status, "CORE: 240MHz • THREADS: 2" statisch, Radar-Blips an festen Pixeln, Label "UTC" zeigt Lokalzeit, "SYNTHWAVE 1984" statt Datum. | `src/ui/pages/FacePage.cpp:650, 778, 986-988, 1210, 1292` |
| Batterietelemetrie | `cachedPercent = 85` initial, SoC per linearer Spannungsinterpolation (Kommentar: "High accuracy curve") statt AXP2101-Fuel-Gauge, `getChargeCurrentMa()` liefert konstant 300. | `src/PowerManager.cpp:73, 178-184, 193` |
| GPS-Satellitenliste | `used = snr >= 18` ist erfunden (echte Nutzung steht in `$xxGSA`); Liste altert nie aus. | `src/GpsManager.cpp:62-65, 89` |
| Kalorien/Schrittlänge | `steps*0.042`, 0,75 m, ohne Nutzerprofil. `getAccel()` ist aus Euler-Winkeln synthetisiert. | `src/MotionHaptic.cpp:364-370, 413-414` |

---

## 6. Mittel: Weitere Korrektheits- und Stabilitätsprobleme

- **`MediaState` über drei Tasks ohne Sperre** (UI liest, NimBLE und WebServer schreiben); `setMediaState(!ms.isPlaying, ms.title, …)` ist `strncpy` mit src == dst (UB). `src/BleCompanion.cpp:314-330`, `ControlPage.cpp:295`
- **Control-Center-Mediakarte verschwindet nach "Pause"**, Wiedergabe von dort nicht mehr startbar. `src/ui/pages/ControlPage.cpp:174, 292-298`
- **`PageDef::ttlMs` ist toter Code**: `registerPage` überschreibt ihn unbedingt mit 0; jede Seite setzt trotzdem Werte. Gecachte Nachbar-Surfaces (System-Uptime, AI-Countdowns) sind in Transitions beliebig alt. `src/ui/Navigator.cpp:705-712`
- **4–8 KB Stack-Arrays in LVGL-Event-Callbacks** (`NavPlace pool[64]`, `DiskPlace batch[64]`, `LoraPeerNode peers[16]`) auf dem 24-KB-UI-Stack. `src/ui/pages/PositionPage.cpp:180-181`
- **Serial-Upload-Kommando `U` blockiert den UI-Task** für die gesamte Dauer des Kartentransfers mit Busy-Wait (`esp_rom_delay_us`); Boot-Spiegelung Flash→SD kopiert die ganze 10-MB-Partition im UI-Task während des Splash. `src/DisplayEngine.cpp:287-324`, `src/NavEngine.cpp:493-521`
- **Coarse-Tier ohne Zellenlimit**: bei Zoom 6–8 bis 32×17 Zellen, 2 Durchgänge, 32 Cache-Slots → jede Zelle pro Frame zweimal von SD. `src/NavEngine.cpp:389, 1531-1541`
- **Zellen > 64 KB werden still verworfen**, Builder deckelt nicht. Löcher in der Karte. `src/NavEngine.cpp:1243`, `tools/build_germany_map.cpp:717, 1065`
- **Upload überschreibt die gerade geladene Kartendatei**, `mapLoaded` bleibt true, Cache füllt sich mit Teildaten. `src/CompanionServer.cpp:291-299`, `src/NavEngine.cpp:1720-1722`
- **`searchPlaces` mit Query = linearer SD-Scan über 25.000 × 48 B im UI-Task**; Buchstaben-Filter sehen nur die Top-500. `src/NavEngine.cpp:931-955`, `PositionPage.cpp:181`
- **`CMD:CAL_SYNC` in `onConnect` geht ins Leere** (vor CCCD-Subscribe); Status-Characteristic überlagert Binär- und ASCII-Protokoll. `src/BleCompanion.cpp:93, 288-312`
- **JSON-Mini-Parser matcht Keys ohne Objektgrenze**: fehlende Keys übernehmen Werte des Folgetermins. `src/BleCompanion.cpp:147-158, 357-373`
- **Haptik-Postfach mit einem Slot**: zwei `triggerHaptic()` innerhalb 10 ms verlieren das erste. `src/MotionHaptic.cpp:34, 221-225`
- **`drvReady`/`rtcReady` ohne `HW_*_ONLINE`-Prüfung**. `src/MotionHaptic.cpp:157`, `src/RtcManager.cpp:152`
- **`days * 86400L`** ist 32-Bit → 2038-Überlauf, obwohl `time_t` 64 Bit ist. `src/RtcManager.cpp:100`
- **`instance.pmu.shutdown()` ohne `lockI2C`**. `src/ui/pages/AppsSmall.cpp:150`
- **`sKeyEvent` read-then-clear** nicht atomar zwischen Tasks (Event kann verloren gehen). `src/PowerManager.cpp:59-63`
- **`isLegacyFakePeerName`** blockiert echte Knoten mit Namen "MESH-REL1"/"ULTRA-02"/"BASE-HQ". Relikt früherer Demo-Peers. `src/RadioLoRa.cpp:80-85`
- **Compositor-Speicherbudget ohne Fallback**: Mirror + Scratch + RowEnc ≈ 1,65 MB PSRAM fix, dazu 19 Page-Surfaces (RLE, realistisch 50–300 KB je). Kein Fehlerpfad, wenn `heap_caps_malloc` scheitert außer `valid = false`. `src/Compositor.cpp:37-53`
- **AOD auf AMOLED ohne Pixel-Shift** (Burn-in-Risiko). `src/DisplayEngine.cpp:106-130`

---

## 7. Build, CI, Tooling, Repo-Hygiene

| # | Befund | Fundstelle |
|---|---|---|
| B1 | **Kein LICENSE.** README hat einen Abschnitt "Lizenz & Credits" ohne Lizenz. Damit ist das Repo "all rights reserved". Fonts (Orbitron, Rajdhani: OFL; Material Symbols: Apache 2.0) sind eingebettet, ihre Lizenzen nicht mitgeliefert. | Repo-Root |
| B2 | **Keine Tests.** Kein `test/`-Verzeichnis, keine Unity-Tests, nichts Host-seitiges für Protobuf-Walker, A*, Zeitzonen oder RLE-Encoder. | Repo-Root |
| B3 | **`lib/LilyGoLib` ist ein stiller Fork.** Drei KRONOS-Patches (`pushColorsAsync`/`waitPushComplete`, `LV_DEF_REFR_PERIOD 10`, Flush-Instrumentierung), `library.json` meldet weiter `0.1.0`. Ein Upstream-Update überschreibt die Patches. `library.json` pinnt RadioLib 7.1.2 / lvgl 9.2.2 / SensorLib 0.3.1, `platformio.ini` zieht 7.4.0 / 9.4.0 / 0.3.3. `lv_conf.h` ist eine 9.2.2-Konfiguration unter 9.4.0. | `lib/LilyGoLib/library.json`, `src/LilyGoDispInterface.h:138`, `src/lv_conf.h:75`, `src/LV_Helper_v9.cpp:36` |
| B4 | **`tools/watchctl.py`: `tour`, `bench`, `rendercost` sind nicht lauffähig.** `goto(s, p)` wird mit drei Argumenten aufgerufen (TypeError). `TILES` sind (col,row), Firmware erwartet Page-IDs. `bench` sendet `W`, `rendercost` sendet `R`, beide Kommandos existieren in `debugConsole()` nicht. `build_firmware.sh tour` und README bewerben damit einen toten Pfad. | `tools/watchctl.py:12, 84, 108-143` |
| B5 | **Host-Tooling Linux-only, Abhängigkeiten nicht deklariert.** `/dev/ttyACM0` hart codiert, `local_zone()` liest `/etc/localtime`. Keine `requirements.txt` (pyserial, Pillow, fonttools). `upload_map_sd.py` akzeptiert jede Logzeile, die auf "K" endet, als ACK. | `tools/watchctl.py:11, 29-34`, `tools/upload_map_sd.py:27-28, 85` |
| B6 | **Screenshot-Stream wird von Hintergrund-Logs korrumpiert**: Bg-Task schreibt weiter `[GPS]`/`[FIT]`/`[PERF]` auf Serial, während der UI-Task Rohbytes sendet; Python prüft `END` nicht. | `src/DisplayEngine.cpp:207-215`, `tools/watchctl.py:59-81` |
| B7 | **CI baut, liefert aber kein Artefakt**; `version.json` verweist auf Tag `latest`, den nichts erzeugt. Cache-Key hasht nur `platformio.ini`, Platform-URL ist der bewegliche `stable`-Zip → nicht reproduzierbar. `.pio/build` mit im Cache. `submodules: recursive` ohne `.gitmodules`. | `.github/workflows/build-firmware.yml` |
| B8 | **`maximum_size: 16777216`** im Board-JSON, App-Partition ist 3 MB. PlatformIO warnt nie bei Überlauf. `-DARDUINO_USB_CDC_ON_BOOT=1` doppelt definiert. | `boards/lilygo-t-watch-ultra.json:42` |
| B9 | **TFT_eSPI ist tote Abhängigkeit** mit ST7789/240×240-Flags für ein 410×502-AMOLED; Kommentar in `platformio.ini:6-8` beschreibt eine nicht existierende Sprite-Pipeline. TinyGPSPlus doppelt (vendored + lib_deps). `Adafruit_TCA8418`, `Adafruit_BusIO`, `NFC-RFAL-fork`, `ST25R3916-fork` werden in `src/` nicht benutzt. | `platformio.ini:26-53`, `lib/` |
| B10 | **README veraltet und teils falsch**: 3 statt 14 Zifferblätter; Tile-Layout entspricht nicht `Navigator.h`; SSID "WATCHOS-ULTRA" existiert nicht; Touch-Controller FT3168 statt CST9217; LoRa-Pins 41/40/39 vs. Code-Kommentar 33/34/35; Projektstruktur ohne Compositor, NavEngine, BleCompanion, AiLimitsStore, Map-Builder. | `README.md` |
| B11 | **Drei Produktnamen** parallel: "WatchOS Ultra" (README), "KRONOS OS" (`Config.h`), "VortexOS" (platformio.ini, AP-SSID, NVS-Namespace `vortex_sys`). Versionsstrings widersprechen sich: `version.json` "1.0.0", `OS_VERSION` "KRONOS 4.0", SystemPage "v4.1". | diverse |
| B12 | `syncFromNtp()` hat keinen Aufrufer und würde 6 s blockieren. `AppId`/`DisplayEngine::setApp/nextApp/prevApp` ohne Aufrufer. Montserrat-Fonts kompiliert (~50 KB Flash), nie verwendet. `wText[26]`, `sRoot`, `nhSun`, `nhDate`, `cdown` ungenutzt. "KNAVDE01"-Legacy-Pfad ohne V1-Writer. | `src/RtcManager.cpp:200-214`, `include/Config.h:51-61`, `src/ui/UiKit.cpp:35-37`, `src/NavEngine.cpp:60, 498` |
| B13 | **Git-Historie**: 10 Commits, 86.000 Zeilen in zwei Tagen, keine PRs, keine Issues, mehrere Features pro Commit. Für Review und Bisect ungeeignet. | `git log` |

---

## 8. Was gut ist

Diese Punkte wurden explizit geprüft und sind in Ordnung. Sie verdienen Erwähnung, weil der Rest des Berichts sonst ein schiefes Bild zeichnet.

- **Compositor** (`src/Compositor.cpp`): Die RLE-Surface-Idee ist clever und für ein AMOLED-UI mit viel Schwarz passend. Encoder/Decoder, Strip-Rendering über private LVGL-APIs, TE-synchronisierter DMA-Push und Dim-Funktion sind sauber. Alle Puffer sind bounds-geprüft.
- **LVGL-Disziplin**: Ein einziger Task erzeugt, ändert und löscht Objekte. Keine Timer-Leaks, keine Styles auf dem Stack (außer A5), Seiten werden einmal gebaut, kein Speicherwachstum pro Swipe. Programmatische Navigation ist korrekt über Pending-Actions entkoppelt.
- **Zeit**: Zeitzonen-Tabelle mit korrekten POSIX-Regeln inklusive DST, Halbstundenzonen, `tm_year`/`tm_mon`-Offsets überall konsistent, days-from-civil korrekt, Tageswechsel mit Lückenbehandlung korrekt.
- **Meshtastic-RX**: 16-Byte-Header, Flag-Bits, AES-128-CTR mit korrektem Nonce, Default-PSK-Expansion, EU868-LongFast-Modemparameter, Protobuf-Walker mit konsequenten Bounds-Checks. Das ist echtes Meshtastic, kein Eigenformat.
- **NVS-Hygiene**: Alle Keys ≤ 15 Zeichen, `begin`/`end`-Paare, Blob-Größenprüfung beim Laden, Settings-Flush per Dirty-Flag und Diff.
- **Kartenformat**: Binärlayout zwischen Builder und Reader stimmt feldweise überein (bis auf die POI-Kategorien), Projektion konsistent, Pixel-Primitive bounds-geprüft.
- **ISR-Handling**: RadioLib-Flag `volatile` + `IRAM_ATTR`, nur Flag-Set. TE-ISR korrekt mit `FromISR`-API. BHI260-Callbacks laufen deferred auf dem Bg-Task unter I2C-Lock.
- **Audio**: Eigener Task mit Queue, Amp-Rail nur bei Wiedergabe, kein Blocking des UI-Tasks, neuer I2S-Treiber korrekt genutzt.
- **HTML-Portal**: Nutzerstrings gehen über `textContent`/`esc()`, kein XSS gefunden.
- **GPS-Fix-Filter** (≥4 Sats, HDOP < 5, Alter < 2 s, Jahr ≥ 2025) ist vernünftig.

---

## 9. Empfohlene Reihenfolge

1. **Sofort** (vor jedem weiteren Flashen auf fremde Geräte): S1–S4 (Auth, Passwort, BLE-Encryption), A2 (CMD27 entfernen), E1/E2/E3 (Funk nicht mehr ungefragt einschalten).
2. **Vor dem nächsten Feature**: A1 (NavEngine-Mutex oder Single-Task), A3/A4 (Bounds und Alignment), A5 (Bauhaus), A6/A7 (I2C/RTC), P1 (Meshtastic-TX, ein Byte), P3/P4 (POI-Enum, Routing-Flags).
3. **Ehrlichkeit herstellen**: Abschnitt 5 komplett. Entweder Features so umbenennen, dass sie das beschreiben, was sie tun, oder "--"/"NO DATA" anzeigen. Commit-Messages und README an die Realität angleichen.
4. **Flash-Verschleiß und Blockierungen**: A8, A11, A12, A13, A14 (alles auf Dirty-Flag + verzögertes Flush bzw. eigenen Task).
5. **Repo-Hygiene**: LICENSE, `requirements.txt`, LilyGoLib-Fork kennzeichnen, tote Dependencies raus, CI-Artefakt, watchctl reparieren, README neu schreiben, einen Produktnamen wählen.
6. **Tests**: Host-seitige Unit-Tests für Protobuf-Walker, A*, RLE-Encoder, Zeitzonen und Kalenderwoche wären mit wenig Aufwand möglich und hätten P1, P3, P4, P14 gefunden.
