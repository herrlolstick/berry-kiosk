# berry-kiosk

Bilderrahmen und Kiosk-Anzeige für den Raspberry Pi – gebaut, um Naturfotografie
im Wohnzimmer oder in öffentlichen Räumen zu zeigen. Läuft ohne Desktop direkt
auf dem Bildschirm (Qt Quick über eglfs/KMS), auch auf günstigen Boards wie dem
Pi Zero 2 W, und nutzt 4K, wo Bildschirm und Pi es können.

> **Status:** Meilenstein 1 – Diashow-Grundgerüst. Widgets, Verwaltungswebsite
> und Bild-Pipeline folgen, siehe [Fahrplan](#fahrplan).

## Was schon geht

- Diashow aus einem Ordner (Unterordner = Alben), nacheinander oder zufällig, mit Überblendung
- **Raster-Layouts:** Bereiche werden in Rasterzellen platziert, z. B. ein großes Bild und zwei kleine (Collage)
- **4K mit automatischem Rückfall auf Full HD** (siehe unten)
- Bilder werden direkt in Anzeigegröße dekodiert – auch 24-MP-JPEGs laufen auf Boards mit 512 MB RAM
- EXIF-Drehung, Hochformat-Bildschirme (`rotation`), Panoramen werden auf das GPU-Limit begrenzt
- `config.json` wird im laufenden Betrieb neu geladen
- Debug-Overlay (`--debug`, F1) mit Gerät, Render-Stufe und FPS

## 4K und Full HD

Beim Start wählt die Anzeige die **Render-Stufe** aus drei Werten – der kleinste gewinnt:

| Quelle | Woher |
|---|---|
| Bildschirm | KMS-Modus bzw. Bildschirmgröße |
| GPU | `GL_MAX_TEXTURE_SIZE` (2048 bei Pi Zero 2 W/3, 4096 bei Pi 4/5) |
| Einstellung | `display.maxRender`: `"auto"` oder `"1080p"` |

- **Pi 3 / Zero 2 W am 4K-Fernseher:** der Bildschirmmodus wird auf 1080p gesetzt, der Fernseher skaliert hoch.
- **Pi 4:** 4K mit 30 Hz (für Standbilder ideal), **Pi 5:** 4K mit 60 Hz.
- Layouts sind in Rasterzellen beschrieben, nicht in Pixeln – dieselbe Konfiguration läuft auf beiden Stufen.

## Entwickeln auf dem Mac

```bash
brew install qt cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
ctest --test-dir build
```

Bilder in den Ordner `media/` neben der Konfiguration legen, dann:

```bash
cp config.example.json config.json
build/display/berry-kiosk-display --config config.json --windowed --debug
```

Mit `Esc` beenden (nur im Fenster- oder Debug-Modus), `F1` schaltet das Overlay.

## Installation auf dem Raspberry Pi

Voraussetzung: **Raspberry Pi OS Lite, 64 Bit, auf Basis von Debian 13 „trixie“** (Qt ≥ 6.5).

```bash
git clone https://github.com/herrlolstick/berry-kiosk.git
cd berry-kiosk
./scripts/install-pi.sh
```

Das Skript baut auf dem Pi, legt den Systembenutzer `berry-kiosk` an, kopiert die
Beispielkonfiguration nach `/var/lib/berry-kiosk/config.json` und startet den Dienst
`berry-kiosk-display`. Bilder kommen nach `/var/lib/berry-kiosk/media/`.

```bash
journalctl -u berry-kiosk-display -f     # Logs
sudo systemctl restart berry-kiosk-display
```

> Auf echter Pi-Hardware ist Meilenstein 1 noch nicht getestet – Rückmeldungen willkommen.

## Konfiguration

Siehe [`config.example.json`](config.example.json). Die wichtigsten Felder:

| Feld | Bedeutung |
|---|---|
| `display.maxRender` | `"auto"` (4K, wenn möglich) oder `"1080p"` |
| `display.rotation` | `0`, `90`, `180`, `270` – für hochkant montierte Bildschirme |
| `media.path` | Bilderordner, relativ zur Konfigurationsdatei oder absolut |
| `activeLayout` | `id` des angezeigten Layouts |
| `layouts[].grid` | Rastergröße, z. B. `{ "columns": 12, "rows": 12 }` |
| `layouts[].regions[]` | Bereiche: `type`, Position `x`/`y`, Größe `w`/`h` in Rasterzellen |

Bereich vom Typ `slideshow`:

| Feld | Werte | Standard |
|---|---|---|
| `album` | Unterordner von `media.path`, leer = alle Bilder | `""` |
| `order` | `"sequential"`, `"shuffle"` | `"sequential"` |
| `interval` | Anzeigedauer in Sekunden | `15` |
| `fit` | `"cover"` (füllen), `"contain"` (ganz zeigen) | `"cover"` |
| `transition.type` | `"fade"`, `"none"` | `"fade"` |
| `transition.duration` | Millisekunden | `1200` |

## Aufbau

```
core/        libberrycore: Konfiguration, Render-Stufe, Geräteerkennung, Bildsuche (nur QtCore)
display/     berry-kiosk-display: Qt-Quick-Anzeige (C++ + QML)
tests/       Unit-Tests für core
packaging/   systemd-Units
scripts/     Installationsskript für den Pi
docs/        Architektur und Planung
```

Mehr dazu in [docs/architektur.md](docs/architektur.md).

## Fahrplan

1. ✅ **Grundgerüst** – Diashow, Raster-Layouts, 4K/1080p-Erkennung, systemd
2. **Widgets** – Uhr, Bildinfo (EXIF), Zitat (Sidecar/XMP)
3. **Kern** – Mediathek (SQLite), Bild-Pipeline mit 1080p-/2160p-Varianten, Exiv2
4. **Verwaltungsserver** – REST-API, Login, Upload, Verbindung zur Anzeige
5. **Website I** – Mediathek mit Upload
6. **Website II** – Raster-Editor mit Vorschau auf dem Kiosk, Animationen
7. **Übergänge** – Schieben, Ken Burns, Shader-Effekte mit Rückfall auf schwächerer GPU
8. **Betrieb** – Zeitplan, Bildschirm aus (DPMS/CEC), Profil „öffentlich“, Overlay-FS, Watchdog, HDMI-Hotplug
9. **Verteilung** – `.deb`-Pakete und fertiges SD-Karten-Image

## Lizenz

[MIT](LICENSE). berry-kiosk nutzt Qt unter der LGPLv3 (dynamisch gelinkt).
