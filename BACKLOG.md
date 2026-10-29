# WatchOS Ultra: Backlog (BACKLOG.md)

Dieses Backlog enthält alle identifizierten Aufgaben zur Erreichung des Zielzustands aus `INTENT.md`.
Aufgaben sind nach Kritikalität und Nutzen priorisiert.

---

### [x] P0: Watchdog-Initialisierung
- **Status:** Erledigt
- **Beschreibung:** `antigravity_watchdog.py` erstellt, angepasst für Antigravity/VS-Code-Logs mit Schutz gegen Selbstbezug/RPC-Echoes und als autonomer Hintergrundprozess gestartet.

---

### [x] P1: GPX-Parser & SD-Lock Entkopplung (Stabilität & Performance)
- **Status:** Erledigt
- **Beschreibung:** SD-Karten-I/O in `loadGpxRoute` und `listGpxFiles` entkoppelt von `NavLock`. Wegpunkte werden sauber unter `lockSd` eingelesen, `<bounds minlat...>` Tags herausgefiltert und `NavLock` erst bei Zuweisung an die aktive Route atomar gebunden. UI-Compositor bleibt während Datei-I/O flüssig.

---

### [x] P2: Autonome AI-Limits-Synchronisation & Webportal-Erweiterung
- **Status:** Erledigt
- **Beschreibung:**
  1. Webportal-UI (`INDEX_HTML`) um vollwertigen AI-Quota-Manager mit Schiebereglern, Presets (100%, 70%, 20%, 0%), Token-Verifizierung und automatischer Berechnung synchroner 5h-, Tages- und Wochen-Reset-Timestamps erweitert.
  2. Prominente, zentrierte Stale-Meldung auf der Uhr (`AiLimitsPage.cpp`) mit direkter Touch-Aktivierung des Hotspots bei veralteten Quotas oder leerem Zustand.
  3. `AiLimitsStore::updateFromJson` leitet bei fehlenden oder abgelaufenen Reset-Timestamps realistische Restabklingzeiten proportional zum Verbrauch ab.

---

### [x] P3: Navigation & Vektorkarte im Google-Maps-Stil (Cartography Overhaul)
- **Status:** Erledigt
- **Beschreibung:**
  1. Kartenthema vollständig an Google Maps Dark Mode angepasst: Satter Schieferhintergrund (`#1A1F2A`), dezentes Gitter, kontrastreiche Gewässer (`rgb565(20, 70, 125)` mit Uferlinie) und Park-/Waldflächen.
  2. Zweistufiges Straßen-Rendering (Casing + Core-Füllung) für Autobahnen (Orange mit Rostcasing), Bundesstraßen (Sonnengelb mit Goldcasing), Stadt- und Wohnstraßen.
  3. Vollständiger 4-Pixel-Halo um Textbeschriftungen für gestochen scharfe Lesbarkeit.
  4. Geschwindigkeitsadaptiver Zoom auf 6 Stufen verfeinert (Zoom 16 für stehende/langsame Fußgängernavigation mit Gebäuden und Details).

---

### [x] P4: Quick Menu / Control Center Direktabsprung in Einstellungen
- **Status:** Erledigt
- **Beschreibung:** Im Control Center (`ControlPage.cpp`) unter der Medien-/Statuskarte direkte Buttons für „Optionen“ (`nav::PG_APP_SETTINGS`) und „Energie“ (`nav::PG_APP_POWER`) ergonomisch platziert.

---

### [x] P5: Webportal-Sicherheit & XSS-Härtung (REVIEW-2 Finding 8)
- **Status:** Erledigt
- **Beschreibung:** `esc()` im Portal-JavaScript um Maskierung einfacher Anführungszeichen `'` (`&#39;`) erweitert, um Attribut-Ausbrüche bei Asset-Namen oder Statusnachrichten zu unterbinden.
