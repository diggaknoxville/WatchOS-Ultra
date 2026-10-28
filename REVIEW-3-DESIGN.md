# Design-Review: WatchOS Ultra (Stand f129530)

Review nach Apples Human Interface Guidelines mit dem `apple-design`-Skill. Plattform: Smartwatch mit 1,75"-AMOLED, 410 × 502 px bei etwa 2× (1 pt ≈ 2 px, die Auflösung entspricht der Apple Watch Ultra). Framework: LVGL 9 in C++. Die watchOS-spezifischen Abschnitte fehlen in den Referenzen; es gelten die Grundlagen (Barrierefreiheit, Farbe, Typografie, Layout, Schreiben) und die iOS-Konventionen sinngemäß. Kontrastwerte sind aus den Hex-Werten in `UiKit.h` berechnet, Größen aus dem Code. Nicht prüfbar ohne Gerät: Lesbarkeit bei Sonnenlicht, Gestenverhalten, Animationsqualität.

Thesis des Designs: ein Expeditions-Instrument am Handgelenk (Karte, GNSS, LoRa-Mesh, SOS) mit einem Zifferblatt-Wechsler als Bühne. Das Einprägsame sind die Orbitron-Ziffern und die fingerverfolgten, komponierten Übergänge.

## Zusammenfassung

Gesamturteil: **Critical issues** vor diesem Commit, nach den Fixes unten **Good**. Die Grundstruktur ist stark: klare Seitenanatomie, konsistente Bausteine, echte Leerzustände, Rückmeldung per Haptik. Zwei Probleme ziehen sich durch alles: Text ist zu klein und zu kontrastarm (Captions 8,5 pt in #5A6270 mit 3,4:1), und viele Bedienelemente liegen unter der Mindestgröße (Tableisten 13 pt hoch, Mini-Knöpfe 16 pt). Dazu kommt ein Vertrauensproblem: mehrere Detailansichten zeigen erfundene Werte (Stundenverlauf, Entladerate, Sonnenzeiten, Trend).

## Kritisch (behoben)

- **Tertiärtext unter 4,5:1.** `cText3` #5A6270 auf Schwarz ergibt 3,4:1, auf Karten (#0F1218) 3,05:1. Diese Farbe trägt Captions, Zeilenschlüssel, Hinweise, Bestwerte.
  - **Warum:** `accessibility.md › Vision`: „Up to 17 pts, all weights: 4.5:1“.
  - **Fix:** `cText3` auf #7C8594 (5,6:1 auf Schwarz, 4,6:1 auf #181C25). Hierarchie zu `cText2` #9BA4B4 (8,4:1) bleibt sichtbar.
- **Captions bei 8,5 pt.** `F.cap` ist 17 px (8,5 pt), `F.small` 21 px (10,5 pt). Beides unter dem Minimum von 11 pt, und Captions tragen Information, nicht nur Dekor.
  - **Warum:** `typography.md › Ensuring legibility`: Minimum 11 pt, bei dünnen Schnitten größer.
  - **Fix:** `F.cap` 17 → 20 px, `F.small` 21 → 22 px, Buchstabenabstand der Captions 2 → 1 px, damit die Versalien bei der neuen Größe noch in ihre Zeilen passen. Das bleibt knapp unter 11 pt; der nächste Schritt wäre 22 px und braucht ein Layout-Durchgang je Seite.
- **Danger-Button: Weiß auf #FF453A = 3,4:1.** Betrifft „Ausschalten“, „SOS-Notruf senden“, Werkseinstellungen-Dialog.
  - **Warum:** `accessibility.md › Vision`, Tabelle.
  - **Fix:** Schwarzer Text auf Rot (6,2:1), analog zum Primär-Button.
- **Bedienelemente unter 28 pt.** Tableisten 32 px (16 pt), Medienknöpfe im Control Center 32 px, „Gelesen“/„Leeren“ 32/30 px, Karten-Zoom 36 px, Kalibrier-Tasten 34 px, Einstellungszeilen 52 px (26 pt), Akzentfarb-Punkte 40 px.
  - **Warum:** `accessibility.md › Mobility`: Minimum 28 × 28 pt, Standard 44 × 44 pt; „spacing between controls as important as size“.
  - **Fix:** Tableisten 40 px, Zeilen 56 px, Medien- und Zoomknöpfe 44 px, Kopfzeilen-Buttons 44 px, Punkte 44 px. Dazu erweitert `pressable()` die Trefffläche jedes festen, kleineren Elements unsichtbar auf 56 × 56 px (`lv_obj_set_ext_click_area`). Pills wie „GPS AUS“ oder „SAT“ bleiben optisch klein, lassen sich aber treffen.

## Hoch (behoben)

- **Erfundene Daten in den Detailansichten.** Schritte: 24-h-Verlauf aus einer Sinuskurve, „Spitzenstunde 14:00“, „Stehziel 9/12“, „Grundumsatz 1.480 kcal“. Energie: „Verbleibend ca. 1d 16h“ aus Prozent × 0,42, Entladekurve synthetisch, „Verbraucher-Audit“ mit festen Prozenten. Tag: Sonnenauf- und -untergang fest 07:12/18:04, „Goldene Stunde“, „Jahreszeit Tag 36 von 90“, Mondphase fest. Fitness-Historie: „Trend: +14 % vs. Vorwoche“ ohne Vorwochendaten, Kadenz-Maximum als Schätzung +24.
  - **Warum:** `design-principles.md › Responsibility`: „Be fully transparent about what your product does“. Eine Uhr, die Messwerte vortäuscht, verliert das Vertrauen in alle anderen Zahlen.
  - **Fix:** Schritte zeigen den echten 7-Tage-Verlauf, die gelernte Kadenz und die Schätzgrundlage („0,75 m je Schritt“). Energie zeigt Spannung, Temperatur und Ladezustand; ohne Verbrauchsmessung steht „keine Restlaufzeit-Messung“. Tag berechnet Auf- und Untergang aus der letzten bekannten Position (NOAA-Näherung, auf 2 Minuten genau), ohne Position steht das dort. Mondphase und meteorologische Jahreszeit werden gerechnet. Der Trend entfällt, bis eine Vorwoche vorliegt.
- **Control Center läuft unten aus dem Glas.** Auf dem Schalter-Tab lagen „Apps“ und „Optionen“ bei y = 438–484 px, im Bereich der Home-Geste und der Eckrundung R120.
  - **Warum:** `layout.md › Guides and safe areas`; `designing-for-ios.md › Best practices`: wenige Bedienelemente, Sekundäres erreichbar halten.
  - **Fix:** Beide Buttons entfernt. Apps liegen einen Wisch links vom Zifferblatt, „Optionen“ bleibt auf dem System-Tab. Die Medienkarte wird 100 px hoch mit 44-px-Knöpfen und endet bei 442 px.

## Verbesserungen (behoben)

- **High – Toasts für Zustandswechsel.** „Bluetooth aktiviert“, „Hotspot wird gestartet“ nach einem Knopf, dessen Beschriftung den Zustand bereits zeigt. `feedback.md › Best practices`: Bestätigung nur für bedeutende Aktionen. Fix: Toasts auf der Verbinden-Seite entfernt; der Hotspot-Toast im Control Center bleibt, weil er auf die Zugangsdaten verweist.
- **High – Falsche Kopplungs-PIN.** Die Verbinden-Seite zeigt „PIN: 123456“, die Kopplung läuft aber ohne PIN (Just Works). Fix: ehrliche Zeile „Kopplung ohne PIN“.
- **Medium – Beschriftungen.** Stoppuhr in Versalien („START“, „RESET“) neben Satzschreibung überall sonst; „Ausschalten (halten)“ und „Kopplungen löschen (Reset)“ tragen die Anleitung im Button; Detailansichten mit „ZUR FITNESS-APP >“ und „STARTSEITE ZURÜCK“. `writing.md › Best practices`: Verben, konsistente Schreibweise, Anleitung nicht im Label. Fix: „Start/Stopp/Runde/Löschen“, „Ausschalten“ mit Hinweiszeile, „Zur Fitness-App“, „Zurück zum Zifferblatt“, Zurück-Knopf mit Chevron-Symbol.
- **Medium – Info-Dialog mit zwei Knöpfen.** „Über“ bot „OK“ und „Schliessen“. `alerts.md › Buttons`: informativ = ein Knopf. Fix: `confirmDialog` versteht jetzt einen leeren Abbrechen-Text und zentriert den einzelnen Knopf.
- **Medium – „fuer“ ohne Umlaut** im Veraltet-Banner der Limits-Seite. Fix: „für“.
- **Low – Dialog-Knöpfe 52 px.** Jetzt 56 px.

## Craft-Notizen

- **Standpunkt.** Das Projekt hat einen: Instrumentenuhr für draußen, mit Mesh und Karte als Alleinstellung. Die Orbitron-Ziffern, der RLE-Compositor mit vsync-gelockten Übergängen und die 14 Zifferblätter sind das Signatur-Element. Das trägt.
- **Vorlagen-Risiko.** Schwarz mit einem sauren Akzent („Cyber Cyan“, „Toxic Matrix Green“, „Hot Magenta“) ist eine der drei Looks, die generierte Oberflächen gerade dominieren. Hier ist Schwarz funktional (AMOLED, Akku) und der Akzent wählbar, der Vorwurf greift also nur halb. Was den Look eigen machen würde: ein Standardakzent aus der Welt des Produkts, etwa das Bernstein der Instrumentenbeleuchtung statt Cyan, und die Farbnamen aus dem Cyberpunk-Vokabular raus aus dem Code. Nicht geändert, weil es eine Produktentscheidung ist.
- **Typografie.** Zwei Textfamilien plus Symbolschrift, klare Rollen (Orbitron = Zahlen, Rajdhani = Text). Rajdhani ist schmal und eher leicht; genau deshalb schlugen die kleinen Größen so hart durch. Mit 20/22 px und weniger Spationierung bleibt der Charakter.
- **Kühnheit an einer Stelle.** Die Zifferblätter dürfen laut sein, der Rest ist ruhig. Das stimmt. Die Limits-Seite mit sieben Markenfarben nebeneinander ist die Ausnahme; dort würde eine Farbe je Anbieter reichen.
- **Ein Accessoire weniger.** Das Control Center hat seine zwei Navigationsknöpfe verloren und ist dadurch lesbarer. Der nächste Kandidat ist der Kalibrier-Bildschirm mit vier Kandidatenradien in vier Farben; als Entwicklerwerkzeug in Ordnung, im App-Drawer eher nicht.

## Was funktioniert

- Seitenanatomie mit `title()`, `CONTENT_Y`, `M_HEAD`/`M_BOT` und den Eckradien als Safe Area. Das sollte so bleiben und jede neue Seite nutzen.
- Leerzustände mit Symbol, Satz und nächstem Schritt (`emptyState`), zum Beispiel „Keine Position“ mit „GPS einschalten“.
- Zustand nie nur über Farbe: Schalter wandern, Kacheln wechseln Füllung und Textfarbe, Buttons wechseln den Text.
- Haptik konsequent an Ereignisse gebunden, in den Spielen nach Bedeutung unterschieden.
- Destruktive Aktionen (Neustart, Werkseinstellungen, SOS) mit Dialog, Abbrechen und rotem Primärknopf nur dort, wo die Aktion nicht die ursprüngliche Absicht war.
- Übergänge folgen dem Finger und lassen sich abbrechen (`motion.md › Providing feedback`).

## Plattform-Notizen

- Zurück in Apps gibt es nur per Kantenwisch oder Seitentaste (= Home). `accessibility.md › Mobility` verlangt eine Alternative zu Gesten. Die Seitentaste erfüllt das für Home; ein echtes „Zurück“ in der Kopfzeile scheitert am R120-Eck oben links. Offen, als Idee: Tippen auf den Seitentitel als Zurück.
- Zeitgesteuerte Elemente: Banner verschwinden nach 4,5 s, bleiben aber in den Mitteilungen erhalten. Das genügt `accessibility.md › Cognitive`.
- Dynamic Type gibt es nicht; eine Einstellung „Größere Schrift“ wäre der nächste Schritt für die Zugänglichkeit, kostet aber einen Layout-Durchgang je Seite.
- Dunkle Darstellung dauerhaft: auf einem AMOLED am Handgelenk legitim (`dark-mode.md › Best practices`, „in rare cases“).
