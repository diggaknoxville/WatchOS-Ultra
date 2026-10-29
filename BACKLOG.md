# WatchOS Ultra: Backlog (BACKLOG.md)

Dieses Backlog enthält alle identifizierten Aufgaben zur Erreichung des Zielzustands aus `INTENT.md`.
Aufgaben sind nach Kritikalität und Nutzen priorisiert.

---

### [x] P0: Watchdog-Initialisierung
- **Status:** Erledigt
- **Beschreibung:** `antigravity_watchdog.py` erstellt, angepasst für Antigravity/VS-Code-Logs und als autonomer Hintergrundprozess gestartet.

---

### [x] P1: GPX-Parser & SD-Lock Entkopplung (Stabilität & Performance)
- **Status:** Erledigt
- **Beschreibung:** SD-Karten-I/O in `loadGpxRoute` und `listGpxFiles` entkoppelt von `NavLock`. Wegpunkte werden sauber unter `lockSd` eingelesen, `<bounds minlat...>` Tags herausgefiltert und `NavLock` erst bei Zuweisung an die aktive Route atomar gebunden. UI-Compositor bleibt während Datei-I/O flüssig.

---

### [ ] P2: Autonome AI-Limits-Synchronisation & Webportal-Erweiterung
- **Komponente:** `src/CompanionServer.cpp`, `src/ui/pages/AiLimitsPage.cpp`
- **Symptom:** Nutzer verlangen Synchronisation auch ohne Smartphone-Companion-App direkt über das integrierte Webportal. Veraltete AI-Limits müssen prominent gewarnt werden.
- **Ziel:**
  1. Webportal-UI (`INDEX_HTML`) um einen vollwertigen, interaktiven AI-Quota-Manager erweitern (Schieberegler, Direkt-Reset, automatische 5h/Wochen-Reset-Zeitstempel-Berechnung).
  2. Prominente, zentrierte Stale-Meldung auf der Uhr (`AiLimitsPage.cpp`), wenn keine Synchronisation stattfand, mit direktem Hinweis/Schaltfläche zum Aktivieren des Hotspots.

---

### [ ] P3: Navigation & Vektorkarte im Google-Maps-Stil (Cartography Overhaul)
- **Komponente:** `src/NavEngine.cpp`, `src/ui/pages/PositionPage.cpp`
- **Symptom:** Kartendarstellung wirkt noch schematisch; Straßenlinien heben sich nicht deutlich vom Hintergrund ab.
- **Ziel:** Straßen mit zweistufigem Casing (dunkle Außenkante + heller Kern für Autobahnen, Bundesstraßen und Hauptstraßen) zeichnen, Gewässer- und Parkflächen kontrastreich absetzen, Ortsbeschriftungen hierarchisch rendern.

---

### [ ] P4: Quick Menu / Control Center Direktabsprung in Einstellungen
- **Komponente:** `src/ui/pages/ControlPage.cpp`
- **Symptom:** Im Control Center (Quick Menu) fehlt ein direkter Absprung in die vollständige Einstellungen-App (`nav::openApp(nav::PG_APP_SETTINGS)`).
- **Ziel:** Im Control Center einen Button für „Einstellungen“ integrieren, der direkt die Einstellungsseite öffnet.

---

### [ ] P5: Webportal-Sicherheit & XSS-Härtung (REVIEW-2 Finding 8)
- **Komponente:** `src/CompanionServer.cpp`
- **Symptom:** `esc()` im Portal-JavaScript maskiert einfache Anführungszeichen `'` nicht, was bei Asset-Namen oder Meldungen zu Attribut-Injection führen kann.
- **Ziel:** `esc()` um `'` (`&#39;`) erweitern und Eingaben im Webserver strikt validieren.
