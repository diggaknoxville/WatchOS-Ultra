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

## Iteration 3: Geplante nächste Evolutionsschritte

### [ ] P1: Spiele-Grafik- & Arcade-Overhaul (Visuelles Redesign aller 9 Games)
- **Komponente:** `src/ui/games/GamesPage.cpp`, `G_Labyrinth.cpp`, `G_Breakout.cpp`, `G_Snake.cpp`, etc.
- **Ziel:** Hochwertiger visueller Neon-Arcade-Look mit geschmeidigen Schatten, kontrastreichen Spielfeldern, Glow-Effekten, Partikeln bei Treffern und Highscore-Overlays.

---

### [ ] P2: Wissenschaftlicher Taschenrechner mit Formelhierarchie
- **Komponente:** `src/ui/pages/CalcPage.cpp`
- **Ziel:** Erweiterung um wissenschaftliche Funktionen (`sin`, `cos`, `tan`, `sqrt`, `^`, `1/x`, `%`), Klammerebenen und Verlaufsband.

---

### [ ] P3: Wi-Fi Hotspot & BLE Pairing-Assistent mit Live-QR-Code
- **Komponente:** `src/CompanionServer.cpp`, `src/ui/pages/SettingsPage.cpp`
- **Ziel:** Generierung und Anzeige eines scanbaren QR-Codes (Wi-Fi WPA2 String `WIFI:S:WatchOS-Ultra-AP;T:WPA;P:...;;`) direkt auf dem AMOLED-Display für sofortige Smartphone-Verbindung ohne manuelle Passworteingabe.
