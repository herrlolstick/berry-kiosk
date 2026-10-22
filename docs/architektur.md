# Architektur

## Überblick

```
┌──────────────────────────── Raspberry Pi ─────────────────────────────┐
│                                                                       │
│  berry-kiosk-display  (C++ / QML, eglfs, direkt auf dem Bildschirm)   │
│     ├─ Layout-Engine: Raster → Regionen → Diashow / Widgets           │
│     ├─ Übergänge & Animationen                                        │
│     └─ lauscht auf lokalem Socket ◄──────────────┐                    │
│                                                  │ "config geändert", │
│  berry-kiosk-admin  (C++ / Qt HttpServer)        │ "neue Bilder",     │
│     ├─ REST-API                                  │ "Screenshot!"      │
│     ├─ liefert die Verwaltungswebsite aus ───────┘                    │
│     ├─ Upload, EXIF/XMP, Skalierung, Thumbnails                       │
│     └─ Login / Sessions                                               │
│                                                                       │
│  libberrycore (gemeinsam): Config, Mediathek, Bild-Pipeline, IPC      │
│                                                                       │
│  /var/lib/berry-kiosk/  config.json · library.db · originals/         │
│                         cache/display/{1080,2160}/ · cache/thumbs/    │
└───────────────────────────────────────────────────────────────────────┘
```

Zwei getrennte Prozesse: Stürzt die Verwaltung ab oder verarbeitet sie gerade viele
Uploads, läuft die Diashow ungestört weiter. Beide teilen sich `libberrycore`.

Stand Meilenstein 7: In `berry-kiosk-admin` läuft die Web-API (`QHttpServer`,
`admin/apiserver.*`) im Hauptthread, Abgleich und Varianten (`core/library.*`,
`core/renditions.*`) in einem eigenen Thread, Bild für Bild, damit neue
Abgleich-Anfragen dazwischenkommen. Die API liest die Mediathek nur lesend; ihre
Änderungen landen im Bilderordner (Uploads, Sidecar-JSON, Verschieben, Papierkorb) und
in `config.json`, der Worker übernimmt sie beim nächsten Abgleich. Der Abgleich erkennt
verschobene Dateien (gleicher Name, gleiche Größe und Änderungszeit an neuem Ort): Sie
behalten ID und Varianten, Verschieben zwischen Alben rechnet also nichts neu.
Anmeldung: ein Konto mit PBKDF2-Hash in `state/auth.json`, Sitzungen im Speicher,
CSRF-Schutz über SameSite-Cookie und Pflicht-Header. Zur Anzeige führt ein lokaler
Socket (`state/display.sock`, nur gleicher Benutzer) mit CBOR-Nachrichten
(`core/displayprotocol.*`): nächstes Bild, Pause, Bildschirmfoto, Status und das
Ereignis „Bild gezeigt“. Daneben bleiben die JSON-Dateien in `state/`
(`display.json` mit der Render-Stufe, `library.json` mit einem Generationszähler).
Die Anzeige liest die Mediathek nur lesend (SQLite im WAL-Modus) und fällt auf den
Ordner zurück, solange es keine gibt. Widgets folgen einer Diashow-Region über deren
`shown`-Signal; `LayoutView` merkt sich das aktuelle Bild je Region.

Seit Meilenstein 6/7: Der Layout-Editor schickt ungespeicherte Layouts über
`POST /api/display/preview` und den Socket-Befehl `preview` an die Anzeige.
`KioskController::updateLayout()` wählt Vorschau vor Zeitplan vor `activeLayout`;
`Main.qml` blendet Layoutwechsel über Schwarz. `SlideshowRegion` steuert Übergänge
über einen Fortschrittswert (fade, slide-*, zoom, kenburns, mix), misst die Bildrate
und fällt nach zwei Übergängen unter 20 fps auf Überblenden zurück. `SlideSlot` hält
ein Bild samt Ken-Burns-Bewegung und, bei `contain`, einen unscharfen Hintergrund aus
einem winzigen Decode (`image://media/backdrop/…`), den die GPU hochskaliert.

Die Web-Oberfläche (`admin/www/`) ist reines JavaScript mit ES-Modulen und CSS, ohne
Build-Schritt und ohne npm-Abhängigkeiten, als Qt-Ressourcen in den Admin-Dienst
einkompiliert: `js/main.js` (Gerüst und Hash-Router), `js/api.js` (API-Client),
`js/ui.js` (Helfer), je Ansicht ein `js/view-*.js` und eine CSS-Datei neben
`css/base.css` mit den Design-Tokens. Strenge CSP ohne Inline-Skripte und -Styles.

## Anzeige

- `main.cpp` – liest vor dem Start von Qt die Konfiguration, schreibt bei Bedarf eine
  eglfs-KMS-Konfiguration (1080p-Modus) und fragt die maximale Texturgröße der GPU ab.
- `KioskController` – stellt QML das aktive Layout und die Render-Entscheidung bereit,
  beobachtet `config.json` und lädt sie bei Änderungen neu.
- `MediaSource` – Bilder eines Albums in Abspielreihenfolge, beobachtet den Ordner.
- `MediaImageProvider` – `image://media/…`: dekodiert Bilder direkt in Anzeigegröße
  (JPEG skaliert beim Lesen) und begrenzt sie auf das Texturlimit der GPU.
- QML: `Main` (Fenster, Drehung) → `LayoutView` (Raster) → `SlideshowRegion` (zwei
  Bild-Slots mit Überblendung; nur sichtbare und ladende Bilder belegen Speicher).

## Render-Stufen

`decideRenderTier()` in `core/rendertier.cpp`:

1. `maxRender: "1080p"` → 1080p
2. Bildschirm nicht größer als Full HD → 1080p
3. GPU-Texturlimit unter 3840 px → 1080p
4. sonst → 2160p

Die Bilder werden höchstens auf die Größe der Region dekodiert, nie größer als die
Stufe. Auf Pi Zero 2 W / Pi 3 (VideoCore IV) setzt die Anzeige zusätzlich den
KMS-Modus auf 1920×1080, damit ein 4K-Fernseher nicht mit einem Modus betrieben wird,
den die GPU nicht füllen kann. Ändert sich `maxRender` unter eglfs, beendet sich die
Anzeige und systemd startet sie im neuen Modus neu.

## Verwaltungswebsite

Gebaut (Meilenstein 5) als ES-Module ohne Build-Schritt statt der ursprünglich
geplanten Svelte-SPA: Der Pi baut sie ohne Node.js, und es gibt keine
npm-Abhängigkeiten. Ausgeliefert vom Admin-Server:

- **Übersicht** – Live-Screenshot des Kiosks, Status, nächstes Bild/Pause, Mediathek und Speicher
- **Mediathek** – Raster mit Filtern und Suche, Mehrfachauswahl (zeigen, ausblenden,
  verschieben, löschen), Detailansicht mit EXIF und Bearbeiten von Titel/Ort/Zitat
- **Hochladen** – Drag & Drop auch ganzer Ordner, Warteschlange mit Fortschritt
- **Alben** – Baum mit Covern, anlegen, umbenennen/verschieben, löschen (Papierkorb)
- **Einstellungen** – Passwort, Sprache, Auflösung, Drehung, Varianten, Upload-Größe
- **Layouts** – Raster-Editor (Ziehen, Größe ändern, Vorlagen, Rückgängig, Tastatur),
  Eigenschaften und Übergang pro Region, „Auf Kiosk vorschauen“ mit Bildschirmfoto,
  Aktivieren, Zitate bearbeiten (`js/view-layouts.js`, `js/layout-editor.js`,
  `js/layout-model.js`, `js/layout-quotes.js`)

Noch offen:

- **Zeitplan** – Layout je Uhrzeit, Bildschirm aus
- **Profil** `home`/`public`
