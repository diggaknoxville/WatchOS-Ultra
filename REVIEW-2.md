# Review 2: Akku, Bedienung, App-Drawer, Start (Version 1.2.0)

Dieses Review baut auf `REVIEW.md` auf. Es deckt den Stand `f2c7b10` ab, also auch die zehn Upstream-Commits seit der Überarbeitung 1.1.0. Teil A beschreibt die vier gemeldeten Probleme, ihre Ursachen und die Umsetzung. Teil B listet die Befunde aus den neuen Commits mit Status. Teil C nennt die nächsten sinnvollen Schritte.

Alle Änderungen sind gebaut, aber nicht auf der Uhr getestet. Stromwerte in diesem Dokument sind Erfahrungswerte für ESP32-S3, SX1262 und AMOLED, keine Messungen an diesem Gerät.

## Teil A: Die vier gemeldeten Punkte

### A1. Akkulaufzeit

**Ursachen**, nach geschätztem Gewicht:

| Ursache | Wirkung | Stelle |
|---|---|---|
| WLAN-Hotspot lief ab dem Start dauerhaft mit 19,5 dBm | größter Einzelverbraucher, zweistellige mA rund um die Uhr | `CompanionServer::init`, `SettingsStorage` (Default `true`) |
| CPU immer auf 240 MHz, auch bei dunklem Display | rund 20 bis 25 mA mehr als 80 MHz im Leerlauf | `PowerManager::setPowerState` (Kommentar schloss Taktabsenkung aus) |
| AMOLED im Schlafzustand nur auf Helligkeit 0, Panel-Treiber blieb an | einige mA dauerhaft | `PowerManager`, `DisplayEngine` |
| Zifferblatt zeichnete mit voller Bildrate (Refresh 0 ms), auch gedimmt | Kern 1 dauerhaft ausgelastet, QSPI-DMA ununterbrochen | `FacePage` (`refreshMs = 0`), `Navigator::update` |
| Display blieb nach dem Dimmen noch einmal die volle Timeout-Zeit an | 25 s zusätzlich gedimmtes Display pro Aufwecken | `PowerManager::loop` (2 × Schwelle) |
| BLE-Werbung im Standardintervall, Verbindung mit 30 bis 50 ms ohne Latenz, +9 dBm | ständige Funkereignisse | `BleCompanion` |
| LoRa-Empfänger dauerhaft im kontinuierlichen RX | rund 5 mA rund um die Uhr | `RadioLoRa` |
| Hintergrund-Task im 10-ms-Takt, auch bei dunklem Display | ständiges Aufwachen beider Kerne | `main.cpp` |
| `channels.txt` wurde ohne Fund im 10-ms-Takt auf der SD gesucht | SD- und SPI-Last, verhinderte jeden Leerlauf | `RadioLoRa::loop` (Upstream) |

Das Vergleichsprojekt *WatchOS* (T-Watch S3) erreicht seine Laufzeit mit genau diesen Hebeln: WLAN und Bluetooth standardmäßig aus, 80 MHz im Leerlauf, Display-Sleep-Befehl und Light-Sleep mit Touch-Wecken.

**Umsetzung**

- **Hotspot als Sitzung.** Er startet nie mehr beim Booten, schaltet sich nach 5 Minuten ohne verbundenes Gerät selbst ab und sendet mit 11 dBm. Der gespeicherte Schalter wird beim Start zurückgesetzt.
- **CPU-Takt nach Zustand.** Aktiv 240 MHz, gedimmt 160 MHz, AOD und Schlaf 80 MHz. Bei laufendem Hotspot mindestens 160 MHz, bei USB-Massenspeicher oder Routenberechnung 240 MHz. Alle drei Stufen lassen den APB-Takt bei 80 MHz, SPI, I2C, I2S und Funk laufen unverändert weiter. Beim Aufwecken hebt der UI-Task den Takt sofort an.
- **Panel-Schlaf.** Im Schlafzustand gehen `SLPIN` und `DISP_EN` aus, sobald die Helligkeit 0 erreicht ist. Beim Aufwecken folgen `SLPOUT`, ein vollständiges Bild und erst dann die Helligkeit.
- **Bildraten-Begrenzung.** Live-Seiten aktualisieren aktiv höchstens alle 33 ms (30 fps) und gedimmt einmal pro Sekunde.
- **Kürzere Dunkelphase.** Nach dem Dimmen folgen AOD oder Display aus jetzt nach 5 s statt nach weiteren 25 s.
- **Bluetooth.** 30 s schnelle Werbung nach Start oder Trennung, danach 417,5 bis 546,25 ms. Die Verbindung wird nach 15 s auf 120 bis 180 ms mit Latenz 4 entspannt (unter 0,9 s Reaktionszeit). Sendeleistung +3 dBm.
- **LoRa-Duty-Cycle.** Bei dunklem Display empfängt der SX1262 im RX-Duty-Cycle. Bei LongFast lauscht er etwa 41 ms und schläft 66 ms, ein 16-Symbol-Preamble wird trotzdem erkannt. Das ist etwa ein Drittel des Stroms. Presets mit 8-Symbol-Preamble bleiben kontinuierlich.
- **Hintergrund-Task** im Schlaf mit 25 Hz statt 100 Hz, Akkuwerte nur alle 10 s.
- **Light-Sleep.** Er greift automatisch im AOD und bei dunklem Display, sobald kein Funk die CPU braucht: Bluetooth-Stack nicht geladen, kein Hotspot, kein USB, GPS aus, kein Ton, keine Routenberechnung. Der UI-Task wird dafür geparkt.
  - Weckquellen sind Touch, Seitentaste, Aktionstaste, Sensor-Hub (Handgelenk), RTC, LoRa-Paket und ein 1-s-Timer.
  - Kanten, die während des Schlafs verloren gehen, werden nachgespielt, damit kein Ereignis hängen bleibt.
- **Energiesparmodus** in der Energie-App. Er schaltet Bluetooth (mit vollständigem Freigeben des Stacks), LoRa, AOD und Hotspot ab, damit der Light-Sleep greift. Beim Ausschalten stellt er den vorherigen Zustand wieder her. Die App zeigt Tiefschlafminuten und aktuellen CPU-Takt.

**Einschränkung.** Mit eingeschaltetem Bluetooth gibt es keinen Light-Sleep. Die vorkompilierten Arduino-Bibliotheken haben weder `CONFIG_PM_ENABLE` noch BT-Modem-Sleep. Ein manueller Light-Sleep würde Verbindungen abbrechen oder den Controller stören. Siehe C1.

### A2. Fehlbedienungen: zu empfindlich und zu unempfindlich zugleich

**Ursachen**

- **Weck-Touch löste Aktionen aus.** Bei dunklem Display oder AOD ging der weckende Touch an die Seite darunter. Ein Tipp aufs schwarze Display konnte Schalter umlegen.
- **Kein Entprellen.** Ein einzelner Scan-Frame wurde zum Tipp, etwa durch Tropfen, Ärmel oder Störimpulse.
- **Ausreißer beim Loslassen.** Ein kurzer Ausfall des Controllers mitten im Wischen beendete die Berührung. Aus einem Wisch wurden zwei Tipps.
- **Zu kleine Schwellen.** 8 px bis zum LVGL-Scroll und 10 px bis zur Gestenerkennung. Ein leicht wackelnder Finger scrollte oder wischte statt zu tippen. Das wirkte wie „Tipp kommt nicht an“.
- **Zu großzügige Richtungserkennung.** Bis 55° galt eine Bewegung als waagrecht. Schräges Scrollen in Listen löste Seitenwechsel aus, auch mitten im Scrollen.

**Umsetzung** in `KronosDisplay::pollTouch` und `Navigator`:

- Eine Berührung zählt erst nach 2 Scan-Frames oder 24 ms.
- Ein Loslassen zählt erst nach 35 ms ohne neuen Kontakt.
- Bei dunklem Display oder AOD weckt die Berührung nur und wird bis zum Loslassen verschluckt.
- **Handfläche.** Zwei oder mehr Kontaktpunkte werden komplett ignoriert. Bleibt die Hand 0,7 s liegen, geht das Display aus.
- Totzone von 3 px gegen Zittern eines ruhenden Fingers.
- Schwellen: LVGL-Scroll ab 14 px, Gestenerkennung ab 16 px.
- Auf Seiten mit senkrechtem Inhalt gilt eine Bewegung nur bis etwa 37° als waagrecht. Scrollt LVGL bereits, gibt es keinen Seitenwechsel mehr.

### A3. App-Drawer ruckelt

**Ursache.** Der Drawer war ein LVGL-Scroll-Container. Jeder Scroll-Frame ließ LVGL das komplette Raster mit 15 runden, kantengeglätteten Icons, Glyphen und Texten neu rendern, in 24-Zeilen-Streifen. Auf diesem Chip dauert das 40 bis 120 ms pro Bild. Dazu kam der Konflikt mit der Seiten-Geste: Leicht schräges Scrollen kippte in einen Seitenwechsel.

**Umsetzung.** Neuer Compositor-Scroller im Navigator, den jede Seite über `PageDef::scrollView`/`scrollContent` nutzen kann:

- Der Inhalt wird einmal im Hintergrund als RLE-Flächen gerendert und gecacht. Jeder Scroll-Frame ist danach nur noch eine Komposition aus dem Cache, wie bei den Seitenübergängen mit 60 fps und vsync.
- Der Finger wird 1:1 verfolgt. Danach folgt Schwung mit iOS-ähnlichem Abklingen und ein Gummiband an den Enden.
- Ein Tipp in die Bewegung stoppt sie, ohne ein Icon auszulösen.
- Beim Scrollen erscheint eine schmale Positionsleiste.
- LVGL bekommt nur die Endposition. Tippen und Treffererkennung bleiben damit unverändert.

### A4. Startbildschirm überlappt mit Teilen des Zifferblatts

**Ursachen**

1. Nach einem Neustart hält das AMOLED noch das letzte Bild. `setup()` schaltete die Helligkeit sofort auf 180 und initialisierte danach sekundenlang alle Subsysteme. Das alte Zifferblatt war zu sehen, bis der Splash kam.
2. Der Splash erschien erst nach der kompletten Initialisierung.
3. Der Übergang schob das Zifferblatt über den Splash. Komplikationen wie Kalender, Schritte und Batterie füllten sich erst danach nach und nach, der Kalender etwa erst bei Sekunde 0 oder 30.
4. Beim Verlassen des AOD zeichnete LVGL den Bildschirm Streifen für Streifen neu. Das war als Wischen von oben nach unten sichtbar.

**Umsetzung**

- Das Panel wird nach `instance.begin()` dunkel geschaltet und schwarz gefüllt.
- Der UI-Task startet sofort und zeigt den Splash auf `lv_layer_top`, deckend über allen Seiten. Danach blendet er die Helligkeit ein. Der Fortschrittsbalken zeigt erst die Initialisierung der Subsysteme, dann das Vorrendern der Seiten.
- Der Übergang ist eine Überblendung über Schwarz. Der Splash blendet aus, danach blendet das vollständig gerenderte Zifferblatt ein.
- Der erste Refresh füllt alle Komplikationen sofort, und der Kalender-Cache wird vor dem Aufbau der Oberfläche geladen.
- Beim Aufwachen aus AOD oder Schlaf zeigt der Navigator die aktuelle Seite sofort komplett (`nav::repaint`) statt eines Streifen-Neuaufbaus.

## Teil B: Befunde in den Upstream-Commits `343ffef..f2c7b10`

| # | Schwere | Befund | Status |
|---|---|---|---|
| 1 | Kritisch | Telemetrie-Parser (`RadioLoRa`, Portnum 67) prüft eine Unterfeld-Länge nicht. Ein einziges unverschlüsseltes Funkpaket lässt den Lesezeiger zurückspringen. Die Endlosschleife löst einen Watchdog-Neustart aus, beliebig wiederholbar. | **Behoben** (Längenprüfung) |
| 2 | Kritisch | BLE auf Just Works ohne MITM. Daten-, Status- und ANS-Characteristics ohne Verschlüsselungspflicht, Trennung bei unverschlüsseltem Link entfernt, Passkey-Bestätigung automatisch. Jedes Gerät in Reichweite kann Kalender, AI-Limits und Benachrichtigungen fälschen. Bei nur 3 Bond-Plätzen kann es das Telefon verdrängen. | Offen, Produktentscheidung. Empfehlung: `WRITE_ENC` auf a101/ANS, Kopplung nur in einem auf der Uhr gestarteten Kopplungsmodus und Bestätigung jeder neuen Kopplung. |
| 3 | Hoch | USB-Massenspeicher und FatFS schreiben gleichzeitig auf dieselbe Karte. Die Folge ist FAT-Korruption, etwa durch GPX-Log, LoRa-Log und Kartentest während des Host-Mounts. | Offen. Empfehlung: exklusiver Besitz, `SD.end()` vor Freigabe. |
| 4 | Hoch | Die SD-Karte hängt ohne Freigabe lesend und schreibend an jedem USB-Host, inklusive Kanal-Schlüsseln im Klartext und Bewegungsprofil. | Offen. Empfehlung: Freigabe nur per Bestätigung auf der Uhr. |
| 5 | Hoch | NodeInfo-Antworten per gefälschter Absender-ID alle 10 s auslösbar. Das leert das Duty-Cycle-Budget, SOS-Baken werden zurückgestellt. | **Teilweise behoben** (60 s auch für Unicast). Offen: Priorität für SOS, Auto-Announce nur per Einstellung. |
| 6 | Hoch (Akku) | `channels.txt` wurde ohne Fund alle 10 ms gesucht. | **Behoben** (einmal pro Minute) |
| 7 | Mittel | SD-Schreibzugriff bei jedem empfangenen Paket, ohne Größengrenze, mit CSV-Injection über Knotennamen. | Offen |
| 8 | Mittel | GitHub-OTA im Portal installiert die erste `.bin` eines fest codierten Repositorys ohne Signatur oder Downgrade-Schutz. Jeder Portal-Aufruf fragt GitHub ab. `esc()` maskiert `'` nicht. | Offen. Empfehlung: entfernen oder signierte Images. |
| 9 | Mittel | ANCS funktioniert nicht: In der Data-Source-UUID fehlt `BE44-`, und Control-Point-Writes gehen ohne Response raus. Nach einer Korrektur drohen Benachrichtigungsflut und Use-after-free. | Offen, absichtlich nicht nur die UUID korrigiert (siehe Gefahr). |
| 10 | Mittel | Medienbefehle gehen per HID und per Companion-App raus. Play/Pause hebt sich auf. | Offen |
| 11 | Mittel | ANS/ANCS-Parsing auf dem NimBLE-Host-Task | Offen |
| 12 | Mittel | GPX-Parser nimmt `<bounds minlat>` als ersten Punkt und hält den Navigations-Lock während des ganzen Lesens. | Offen |
| 13 | Mittel | GPX- und Backtrack-Route werden im Mesh-Modus bzw. beim Moduswechsel verworfen. | Offen |
| 14 | Niedrig | Long-Press auf „GPX“ löst zusätzlich CLICKED aus. | **Behoben** (`SHORT_CLICKED`) |
| 15 | Niedrig | Rund 27 Schlüsselversuche pro Paket erhöhen die Rate falsch entschlüsselter Nachrichten. | Offen |
| 16 | Niedrig | MSC-Callbacks warten unbegrenzt auf den Navigations-Mutex. | Offen |
| 17 | Niedrig | Task-Stacks ohne Messung verkleinert (UI 28 auf 20 KB, Hintergrund 16 auf 14 KB) | Offen. Empfehlung: High-Water-Mark loggen. |

Gehalten haben: keine neuen Portal-Endpunkte ohne PIN oder Origin-Prüfung, OTA-Upload weiter mit PIN, SHA-256 und Bestätigung auf der Uhr, keine neuen I2C-Zugriffe ohne Lock.

## Teil C: Nächste Schritte

1. **Light-Sleep mit verbundenem Telefon.** pioarduino kann die Arduino-Bibliotheken mit eigener `sdkconfig` neu bauen (`custom_sdkconfig`). Nötig sind `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE` und `CONFIG_BT_CTRL_MODEM_SLEEP` mit Low-Power-Takt. Dann schläft die CPU automatisch zwischen BLE-Verbindungsereignissen, und die Verbindung bleibt bestehen. Das ist der größte verbleibende Hebel. Er kostet deutlich längere Builds und braucht PM-Locks während des Display-DMA.
2. **Strommessung.** Je Zustand (aktiv, gedimmt, AOD, Schlaf mit und ohne Bluetooth) mit einem USB-Messgerät oder dem AXP2101-Batteriestrom messen und im README dokumentieren.
3. **Sicherheitsbefunde 2 bis 4 und 8** vor einem Release beheben.
4. **Weitere Listen auf den Compositor-Scroller umstellen**, zum Beispiel Mitteilungen und Einstellungen.
