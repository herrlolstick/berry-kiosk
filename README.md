# berry-kiosk

A photo frame and kiosk display for the Raspberry Pi, built to show nature
photography in a living room or a public space. It runs without a desktop,
straight on the screen (Qt Quick via eglfs/KMS), works on low-cost boards like
the Pi Zero 2 W and uses 4K wherever the screen and the Pi can handle it.

> **Status:** milestone 2 – slideshow and widgets. The web admin interface and
> the image pipeline are next, see the [roadmap](#roadmap).

## Features

- Slideshow from a folder (sub folders are albums), sequential or shuffled, with cross-fades
- **Grid layouts:** regions are placed on grid cells, e.g. one large picture and two small ones (collage)
- **Widgets:** clock, image info (title, location, date, camera settings from EXIF/IPTC/XMP) and quotes
- **4K with automatic fallback to Full HD** (see below)
- Images are decoded straight to their display size – even 24 MP JPEGs run on boards with 512 MB RAM
- EXIF orientation, portrait screens (`rotation`), panoramas are capped at the GPU texture limit
- `config.json` is reloaded while running
- Debug overlay (`--debug`, F1) with device, render tier and frame rate

## 4K and Full HD

At startup the display picks a **render tier** from three values – the smallest wins:

| Source | From |
|---|---|
| Screen | KMS mode / screen size |
| GPU | `GL_MAX_TEXTURE_SIZE` (2048 on Pi Zero 2 W/3, 4096 on Pi 4/5) |
| Setting | `display.maxRender`: `"auto"` or `"1080p"` |

- **Pi 3 / Zero 2 W on a 4K TV:** the screen mode is set to 1080p and the TV upscales.
- **Pi 4:** 4K at 30 Hz (ideal for still images), **Pi 5:** 4K at 60 Hz.
- Layouts are described in grid cells, not pixels – the same configuration works on both tiers.

## Developing on macOS

```bash
brew install qt exiv2 cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix exiv2)"
cmake --build build
ctest --test-dir build
```

Put some pictures into `media/` next to the configuration, then:

```bash
cp config.example.json config.json
build/display/berry-kiosk-display --config config.json --windowed --debug
```

`Esc` quits (only in windowed or debug mode), `F1` toggles the overlay.
`--snapshot <file>` saves a screenshot after `--snapshot-delay` seconds and exits.

## Installing on a Raspberry Pi

Requires **Raspberry Pi OS Lite, 64-bit, based on Debian 13 "trixie"** (Qt ≥ 6.5).

```bash
git clone https://github.com/herrlolstick/berry-kiosk.git
cd berry-kiosk
./scripts/install-pi.sh
```

The script builds on the Pi, creates the system user `berry-kiosk`, copies the
example configuration to `/var/lib/berry-kiosk/config.json` and starts the
`berry-kiosk-display` service. Pictures go into `/var/lib/berry-kiosk/media/`.

```bash
journalctl -u berry-kiosk-display -f     # logs
sudo systemctl restart berry-kiosk-display
```

> Not yet tested on real Pi hardware – feedback welcome.

## Configuration

See [`config.example.json`](config.example.json). Top level:

| Field | Meaning |
|---|---|
| `locale` | Language for dates and numbers, e.g. `"de_DE"`, `"en_GB"` |
| `display.maxRender` | `"auto"` (4K when possible) or `"1080p"` |
| `display.rotation` | `0`, `90`, `180`, `270` – for screens mounted in portrait |
| `media.path` | Picture folder, relative to the configuration file or absolute |
| `quotes` | Global quotes: `[{ "text": "…", "author": "…" }]` or plain strings |
| `activeLayout` | `id` of the layout to show |
| `layouts[].grid` | Grid size, e.g. `{ "columns": 12, "rows": 12 }` |
| `layouts[].regions[]` | Regions: `type`, position `x`/`y` and size `w`/`h` in grid cells, optional `id` and `z` |

### `slideshow`

| Field | Values | Default |
|---|---|---|
| `album` | Sub folder of `media.path`, empty = all pictures | `""` |
| `order` | `"sequential"`, `"shuffle"` | `"sequential"` |
| `interval` | Seconds per picture | `15` |
| `fit` | `"cover"` (fill), `"contain"` (show whole picture) | `"cover"` |
| `transition.type` | `"fade"`, `"none"` | `"fade"` |
| `transition.duration` | Milliseconds | `1200` |

### `clock`

| Field | Values | Default |
|---|---|---|
| `format` | Qt time format, seconds with `"HH:mm:ss"` | `"HH:mm"` |
| `showDate` | `true`, `false` | `true` |
| `dateFormat` | Qt date format | `"dddd, d. MMMM"` |

### `imageInfo`

Shows details of the picture a slideshow currently shows and hides itself for
pictures without any of the chosen fields.

| Field | Values | Default |
|---|---|---|
| `follows` | `id` of a slideshow region | first slideshow |
| `fields` | `title`, `location`, `date`, `description`, `camera`, `lens`, `exposure` | `["title", "location", "date"]` |
| `dateFormat` | Qt date format | `"d. MMMM yyyy"` |

Title, description and location come from XMP (`dc:title`, `dc:description`,
`photoshop:City`, …) or IPTC – what Lightroom, darktable or digiKam write.
GPS coordinates are never read, so the location of a nest or a den cannot end up
on a public screen.

### `quote`

| Field | Values | Default |
|---|---|---|
| `follows` | `id` of a slideshow region | first slideshow |
| `source` | `"image-or-global"`, `"image"`, `"global"` | `"image-or-global"` |
| `interval` | Seconds between global quotes when no slideshow is followed | `60` |

### Widget style

Every widget accepts a `style` object:

| Field | Values | Default |
|---|---|---|
| `background` | `"dark"`, `"light"`, `"none"` (text with a soft shadow) | `"dark"` |
| `backgroundColor`, `color` | Colours such as `"#80000000"`, `"white"` | depends on `background` |
| `align` | `"left"`, `"center"`, `"right"` | per widget |
| `font` | Font family | system font |
| `italic` | Quotes only | `true` |
| `opacity` | `0` … `1` | `1` |
| `padding`, `margin` | Share of the region's shorter side | `0.12`, `0.08` |

Text always scales to the size of its region, so widgets look the same at 1080p and 4K.

### Per-picture details (sidecar files)

A JSON file with the picture's name sits next to it (`elk.jpg` → `elk.json`) and
overrides the embedded metadata:

```json
{
  "title": "Elk in the morning mist",
  "location": "Kiruna, Sweden",
  "quote": "In every walk with nature one receives far more than he seeks.",
  "quoteAuthor": "John Muir"
}
```

An empty string hides a value that is embedded in the picture.

## Project layout

```
core/        libberrycore: configuration, render tier, device detection, image scanning, metadata
display/     berry-kiosk-display: the Qt Quick display (C++ and QML)
tests/       unit tests for core
packaging/   systemd units
scripts/     install script for the Pi
docs/        architecture and planning (German)
```

## Roadmap

1. ✅ **Foundation** – slideshow, grid layouts, 4K/1080p detection, systemd
2. ✅ **Widgets** – clock, image info (EXIF/IPTC/XMP), quotes (sidecar or global list)
3. **Core** – media library (SQLite), image pipeline with 1080p/2160p renditions
4. **Admin server** – REST API, login, uploads, link to the display
5. **Web UI I** – media library with uploads
6. **Web UI II** – grid editor with live preview on the kiosk, animation settings
7. **Transitions** – slide, Ken Burns, shader effects with fallbacks for weaker GPUs
8. **Operations** – schedule, screen off (DPMS/CEC), public profile, read-only root, watchdog, HDMI hotplug
9. **Distribution** – `.deb` packages and a ready-made SD card image

## License

[MIT](LICENSE). berry-kiosk uses Qt under the LGPLv3 (dynamically linked) and Exiv2 under the GPLv2+.
