# WatchOS Ultra: Backlog (BACKLOG.md)

Dieses Backlog enthält die Aufgaben zur kontinuierlichen Evolution des Systems (`INTENT.md`).
Aufgaben werden sequenziell, autonom und vollständig mit Firmware-Flash und Hardware-Verifikation umgesetzt.

---

### [X] P1: Zifferblatt-Overhaul: Wayfinder Live-Kompasslünette & Casio Vintage 7-Segment Retro-Look & Wortuhr Matrix
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshots `shot_verified_wayfinder_v2.png`, `shot_verified_casio_v2.png`, `shot_verified_wordclock_v2.png`, `shot_verified_lifetime_v2.png`).
- **Umsetzung:**
  1. **Wayfinder Kompass-Drehung & Polung:** `MotionHaptic::getEuler` Vorzeichen korrigiert (`sRawYaw - sYawZero`). Drehung nach links zeigt 315° NW, Drehung nach rechts 045° NO. Lünettenmarkierungen (N, O, S, W) drehen sich physisch mit Blickwinkel mit.
  2. **Casio Vintage Authentizität:** 7-Segment Shadow Ghosting (`88:88:88` und `88.88.`), 10-Segment Sekunden-Laufband, vollständige Sensorzeile (STP, DIR, BAT), Distanz & Kalorien, klassische Gehäuse-Bedruckung.
  3. **Wortuhr Komplett-Redesign:** Echtes 11×10 deutsches Qlocktwo-Buchstabenraster mit 110 Einzelzeichen, dezentem Dunkelschiefer-Raster und brillanten weißen Aktivwörtern, 4-Eck-Minutenpunkten (+1..+4 Min) und fehlerfreier Grammatik ("ZWANZIG NACH", "FÜNF VOR HALB", "EIN UHR").
  4. **Lifetime Watchface:** 4-Balken-System (LEBEN, JAHR, MONAT, TAG) mit interaktivem Umschalter `[VERBRAUCHT]` / `[VERBLEIBEND]` und exakter Resttage-Anzeige.
  5. **Spiele-Neigungssteuerung:** `TILT_SIGN_X` und `TILT_SIGN_Y` in `GameKit.h` auf physikalische Hangabtriebskraft invertiert (Kugel rollt in Absenkungsrichtung).

---

### [X] P2: Heading-Up Vektorkarten-Navigation (Rotierende Karte nach Kompass/Kurs)
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshot `shot_verified_position_v2.png`).
- **Umsetzung:**
  1. `NavEngine::renderMap` um Heading-Rotationsmatrix um den Bildschirmmittelpunkt erweitert.
  2. Diagonale Bounding-Box-Erweiterung für 360°-Kartendrehung ohne Kantenabschnitte.
  3. Interaktiver Umschalter `N-UP` / `HD-UP` in der Kartenansicht (`PositionPage`).
  4. Rotierende Nordrose und richtungsweisender Positionskegel.

---

### [X] P3: Detaillierte Aktivitäts- & Leistungsanalytik (Activity Deep Insights)
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshot `shot_verified_activity_v2.png`).
- **Umsetzung:**
  1. `MotionHaptic`: 24-Stunden-Histogramm-Speicher `sHourlySteps[24]` mit NVS-Persistenz und Rollover.
  2. `FacePage` Schritte-Detailmodal: 24h-Stunden-Histogramm-Karte mit Erkennung der aktivsten Stunde und aktuellem Stundenmarker.
  3. Historische 7-Tage-Durchschnitte, Kadenz-Trends und Zielerreichungsquoten.

---

## Iteration 3: Spiele-Grafik, Wissenschaftlicher Rechner & Zero-Conf Verbinden

### [X] P1: Spiele-Grafik- & Arcade-Overhaul (Visuelles Redesign der Games)
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshots `shot_verified_games_v3.png`).
- **Umsetzung:**
  1. `G_Labyrinth.cpp`: Metallische 3D-Kugel mit Glanzlicht (`sGlint`), Neon-Cyan-Wände mit dynamischem Drop-Shadow (`0x0080FF`), pulsierendes Zielportal und taktiles Haptik-Feedback bei Wandstößen.
  2. `G_Breakout.cpp`: Neon-Paddel mit Leuchteffekt und abgerundeten Ecken, Ziegelsteine mit Kanten-Highlight und Neon-Glow, metallischer Ball mit Spekular-Reflexion und Wandabprall-Haptik.
  3. `G_Snake.cpp`: Schlange mit Farbverlauf von Kopf (Neon-Türkis `0x00FFC2` mit Glow) bis Schwanz (Smaragdgrün), leuchtendes Rubin-Futter mit weißem Kern und abgerundete Matrix.
  4. `G_Balance.cpp`: 3D-Kugel mit Farbverlauf, Glanzpunkt und Haptik.
  5. `GamesPage.cpp`: Lebendiges Neon-Ordner-Grid mit individuellen Leuchtschatten (`0x00D0FF` etc.) pro Spielkachel.

---

### [X] P2: Wissenschaftlicher Taschenrechner mit Memory-Funktion & Prozentberechnung
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshot `shot_verified_calc_v3.png`).
- **Umsetzung:**
  1. Vollständige Formelhierarchie mit rekursivem Abstieg: `+`, `-`, `*`, `/`, `^`, `%`, `!`, Klammern `(`, `)`.
  2. Mathematische Funktionen: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `ln`, `log`, `abs`, `pi`, `e`, `Ans`.
  3. Vollwertige Speicherfunktionen (Memory): `MC` (Clear), `MR` (Recall), `M+` (Add), `M-` (Subtract) mit Live-Anzeige des Speicherwerts und `[M]` Badge auf dem Display.
  4. Postfix-Prozentberechnung (`200 * 15% = 30`).

---

### [X] P3: Wi-Fi Hotspot & BLE Pairing-Assistent mit Live-QR-Code & Zero-Conf
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshots `shot_verified_connect_v3.png`, `shot_verified_control_v3.png`).
- **Umsetzung:**
  1. BLE-Sicherheit auf nahtloses "Just Works" Bonding ohne PIN-Zwang und ohne Companion-App umgestellt (`mitm = false`, `BLE_HS_IO_NO_INPUT_OUTPUT`). Smartphones (iOS & Android) koppeln sich direkt über die Systemeinstellungen für Mediensteuerung und Benachrichtigungen.
  2. Direkter Schnellzugriff: Das Aktivieren des Hotspots im Kontrollzentrum oder in den Einstellungen navigiert sofort auf die Verbindungs-Seite mit dem großen, scanbaren QR-Code (`WIFI:T:WPA;S:...;P:...;;`).
  3. Kontrollzentrum bietet direkte Tasten für Einstellungen ("Optionen") und Power-Management.

---

## Iteration 4: Live-Funkstatus, AI-Sync-Prominenz & Navigation

### [X] P1: Startscreen-Kopfleiste & Funkstandard-Indikatoren (Live Subsystem Telemetrie)
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshot `shot_verified_face_v4.png`).
- **Umsetzung:**
  1. Alle Funk- & Systemindikatoren (ECO, DND, WLAN, BLE, GPS, LoRa) bleiben permanent im Header sichtbar.
  2. Aktive / verbundene Zustände leuchten in ihren Neon-Akzentfarben (z. B. WLAN `0xFF9F0A`, BLE `0x0A84FF`, GPS Fix `0x30D158`).
  3. Inaktive / ausgeschaltete Zustände verbleiben als dezentes Dunkelschiefer-Icon (`0x253044`).
  4. Status-Pill und Symbolleiste sind interaktiv: Ein Fingertipp öffnet direkt das Kontrollzentrum.

---

### [X] P3: Standalone AI-Sync & Stale-Warning Banner
- **Status:** ERLEDIGT & HARDWARE-VERIFIZIERT (Screenshot `shot_verified_ailimits_v4.png`, `shot_verified_face_v4.png`).
- **Umsetzung:**
  1. Prominenter, 44 px hoher Warnbanner in Leucht-Bernstein mit Drop-Shadow direkt unter dem Header in der AI-Limits-Ansicht bei Werten älter als 12 Stunden.
  2. Ein Fingertipp auf den Banner aktiviert sofort den WLAN-Hotspot und navigiert automatisch zur QR-Code-Verbindungsseite.
  3. Auf dem Startbildschirm (Modular Face) erscheint bei veralteten Daten ohne anstehende Termine ein Hinweis mit Direktlink zur Synchronisation.

---

### [ ] P2: Google Maps Cartography Overhaul (Detailgrad & Straßenhierarchie)
- **Komponente:** `src/NavEngine.cpp`, `src/ui/pages/PositionPage.cpp`
- **Ziel:** Weitere Optimierung der Vektorkarte nach Google-Maps-Standard (Wasserlinienkontrast, präzise Parkflächen, Straßenbeschriftung).


