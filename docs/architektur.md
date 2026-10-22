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
│     ├─ REST-API + WebSocket                      │ "Screenshot!"      │
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

Stand Meilenstein 2: nur `berry-kiosk-display` und der Teil von `libberrycore`, den
die Anzeige braucht. Die Anzeige liest Bilder und Metadaten (Exiv2, Sidecar-JSON)
noch direkt aus dem Medienordner. Widgets folgen einer Diashow-Region über deren
`shown`-Signal; `LayoutView` merkt sich das aktuelle Bild je Region.

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

## Geplante Verwaltungswebsite

Svelte-SPA mit gridstack.js, ausgeliefert vom Admin-Server (kein Node.js auf dem Pi):

- **Dashboard** – Live-Screenshot des Kiosks, Status, nächstes Bild/Pause
- **Raster-Editor** – Regionen per Drag & Drop, Eigenschaften pro Region,
  „Auf Kiosk vorschauen“ und „Aktivieren“
- **Animationen** – Übergänge global und pro Region
- **Mediathek** – Upload per Drag & Drop, Alben, Titel/Zitat pro Bild, EXIF-Anzeige
- **Zeitplan** – Layout je Uhrzeit, Bildschirm aus
- **Einstellungen** – Profil `home`/`public`, Passwort, Auflösung, Drehung
