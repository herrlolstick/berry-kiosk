# berry-kiosk

A photo frame and kiosk display for the Raspberry Pi, built to show nature
photography in a living room or a public space. It runs without a desktop,
straight on the screen (Qt Quick via eglfs/KMS), works on low-cost boards like
the Pi Zero 2 W and uses 4K wherever the screen and the Pi can handle it.

> **Status:** milestone 7 – slideshow with transitions, widgets, media library,
> image pipeline, admin web API and the web UI incl. the layout editor. Operations
> (schedule, screen off, public profile) are next, see the [roadmap](#roadmap).

## Features

- Slideshow from a folder (sub folders are albums), sequential or shuffled, with transitions: fade, slide, zoom, Ken Burns or a random mix – falling back to fades on boards that cannot keep up
- **Grid layouts:** regions are placed on grid cells, e.g. one large picture and two small ones (collage), edited in the browser with a live preview on the kiosk
- **Widgets:** clock, image info (title, location, date, camera settings from EXIF/IPTC/XMP) and quotes
- **4K with automatic fallback to Full HD** (see below)
- **Media library** (SQLite) with prepared 1080p/2160p renditions and thumbnails, kept up to date in the background
- **Web UI** for phone and desktop: media library with search, filters and bulk actions, uploads (whole folders too), albums, editing titles and quotes, remote control of the display (next picture, pause, live screenshot)
- **Admin web API** behind it, with login and CSRF protection
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

## Media library and renditions

The media folder stays the source of truth: copy pictures in (rsync, Samba, a USB
stick, later the web UI) and organise them in sub folders, which are albums.
The service `berry-kiosk-admin` keeps an index of them in `library.db` and
prepares renditions in the background:

| Rendition | Size | Used by |
|---|---|---|
| `1080p` | covers 1920 × 1920, at most 2048 px | displays rendering at 1080p |
| `2160p` | covers 3840 × 3840, at most 4096 px | displays rendering at 4K |
| `thumb` | fits into 512 × 512 | the admin UI |

- Only the tier the display actually uses is prepared (`library.renditions: "auto"`):
  the display reports its tier in `state/display.json`.
- The display always picks the best file it has: its own tier, else the other
  tier, else the original. Switching to a 4K screen therefore works right away
  and gets sharper as the 2160p renditions come in.
- Renditions are upright (EXIF orientation applied), newest pictures are done first,
  and broken files are skipped without being retried until they change.
- Moved files are recognised (same name, size and modification time in a new
  place): they keep their id and their renditions, so moving pictures between
  albums – in the web UI, the Finder or with rsync – does not re-render 4K files on
  the Pi. A renamed file counts as a new one.
- Without a library (admin service not running) the display reads the folder directly.
- The admin service runs with low CPU and I/O priority, so the slideshow never stutters.

Data layout, all relative to the configuration file by default:

```
config.json
media/          your pictures (sub folders = albums, sidecar .json files next to them)
library.db      index and metadata (SQLite, WAL)
cache/1080p/    renditions per tier, <id>.jpg
cache/2160p/
cache/thumb/
state/          display.json and library.json – how display and admin service talk
```

## Admin web API

`berry-kiosk-admin` also serves a REST API and the web UI (compiled into the
binary) on port 8080: open `http://<pi>.local:8080/`.

### Web UI

| View | What it does |
|---|---|
| Übersicht | Live screenshot of the kiosk, status, next picture / pause, library and storage, latest pictures |
| Mediathek | Grid with infinite scroll; filter by album, search, shown/hidden, sort; select pictures to show, hide, move or delete them; detail view with a large preview, metadata and editing of title, location, description, quote and "in der Diashow zeigen", arrow keys and swipe for the next picture |
| Hochladen | Drop zone for files and whole folders (sub folders become sub albums), queue with progress and retries; one file at a time, as the server holds an upload in memory |
| Alben | Album tree with covers and counts: create, rename/move, delete (to the trash) |
| Layouts | Grid editor: drag and resize regions on the grid, templates (full screen, with clock and image info, collage, portrait pair), properties per region (album, order, interval, fit, transition; widget style), undo/redo, keyboard (arrows move, Shift+arrows resize, Delete removes); "Auf Kiosk vorschauen" shows the unsaved layout on the kiosk for 60 s and fetches a screenshot; activate a layout; a "Zitate" tab for the global quotes |
| Einstellungen | Change the password; language, maximum resolution, rotation, renditions, JPEG quality, maximum upload size |

It is plain ES modules and CSS – no build step and no npm dependencies, so the Pi
builds it without Node.js. The files in `admin/www/` are compiled into
`berry-kiosk-admin` as Qt resources: `index.html`, `js/main.js` (shell and hash
router), `js/api.js` (API client), `js/ui.js` (helpers), one `js/view-*.js` per
view, `css/base.css` (design tokens) and one CSS file per view. A strict Content
Security Policy allows no inline scripts or styles. It works on phones and follows
the system's light or dark mode.

### Setup

There is one admin account. Set or change its password on the Pi:

```bash
sudo -u berry-kiosk berry-kiosk-admin --config /var/lib/berry-kiosk/config.json --set-password [--user admin]
```

It asks twice without echo, or reads one line from stdin (for scripts). The
password needs at least 8 characters and is stored as salted PBKDF2-SHA256
(200 000 iterations) in `state/auth.json`, readable by the service user only.
Changing it ends all sessions. The install script asks for it on first install.

### Security

- Logging in sets the cookie `berry_session` (random token, `HttpOnly`,
  `SameSite=Strict`, valid for 12 hours of inactivity). Sessions live in memory:
  after a restart of the service you log in again.
- Requests that change something need the header `X-Berry-Kiosk: 1`; together
  with `SameSite` this shuts out cross-site requests (CSRF).
- After 5 failed logins from one address, the wait doubles with every further
  failure, from 30 seconds up to 15 minutes.
- It speaks plain HTTP and is meant for the home network. For anything else, put
  it behind a TLS reverse proxy (e.g. on a NAS or Umbrel) and set `admin.address`
  to `127.0.0.1`, or firewall the port.
- Request bodies are held in memory (up to `admin.maxUploadMB`), so do not expose
  the port to untrusted networks. The display is a separate process and keeps
  running even if the admin service is overloaded.

### How changes are stored

The media folder stays the source of truth, so the library can always be rebuilt
from it:

- **Uploads** land in the album folder. Names are sanitised, an existing file
  gets a ` (2)` suffix instead of being overwritten, and only JPEG, PNG and WebP
  are accepted (checked by content).
- **Metadata edits** go to the picture's sidecar JSON (`title`, `description`,
  `location`, `quote`, `quoteAuthor`); `"enabled": false` hides a picture from the slideshow.
- **Deleting** moves the picture and its sidecar to `trash/<yyyyMMdd-HHmmss>/<path>`;
  nothing is deleted for good.
- The library worker picks all of this up by itself.

### Link to the display

The display listens on the local socket `state/display.sock` (only the same
system user can connect; a short name in the temp folder is used when the path
would be too long). The admin service uses it for next picture, pause/resume,
live screenshots and the status, and the display reports every picture it shows.

### Endpoints

All under `/api`, JSON in and out, errors as `{ "error": "…" }`.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/auth/session` | Logged in? Password set? |
| `POST` | `/auth/login` | `{ "username", "password" }`, sets the session cookie |
| `POST` | `/auth/logout` | Ends the session |
| `POST` | `/auth/password` | `{ "current", "new" }`, ends all sessions |
| `GET` | `/status` | Version, uptime, display, library, storage |
| `GET` | `/media?album=&q=&enabled=&sort=&order=&offset=&limit=` | Pictures; `q` searches title, location, description and path, `enabled=true\|false`, `sort=added\|taken\|name`, `order=asc\|desc` (default: newest first). Items carry `fileSize` and `version`, which changes with the file (for cache busting) |
| `GET` | `/media/{id}` | One picture with its metadata |
| `PATCH` | `/media/{id}` | Edit metadata or `enabled` (written to the sidecar) |
| `DELETE` | `/media/{id}` | Move to the trash |
| `GET` | `/media/{id}/thumb` | Thumbnail |
| `GET` | `/media/{id}/image?kind=1080p\|2160p\|original` | Rendition or original |
| `POST` | `/media?album=&name=` | Upload, the raw file as body |
| `POST` | `/media/batch` | `{ "ids", "action": "enable"\|"disable"\|"delete"\|"move", "album"? }` → `{ "done", "failed" }` |
| `GET` | `/albums` | Albums with picture counts and a `cover` `{ id, version }` |
| `POST` | `/albums` | `{ "name" }`, creates a folder |
| `PATCH` | `/albums` | `{ "from", "to" }`, renames or moves a folder (409 if `to` exists) |
| `DELETE` | `/albums?name=` | Moves the folder with its pictures to the trash |
| `GET` | `/config` | `config.json` |
| `PUT` | `/config` | Validated; the previous version is kept as `config.json.bak` |
| `POST` | `/display/next`, `/display/pause`, `/display/resume` | Remote control |
| `GET` | `/display/screenshot?maxWidth=` | Live screenshot (JPEG) |
| `POST` | `/display/preview` | `{ "layout", "seconds" }` (1–600, default 60): show an unsaved layout on the kiosk for a while, checked like a saved one |
| `DELETE` | `/display/preview` | End the preview now |
| `POST` | `/library/sync` | Sync the library now |

The web UI itself is served from `/`, `/js/`, `/css/` and `/assets/`.

## Developing on macOS

```bash
brew install qt exiv2 cmake ninja   # Qt includes the SQLite driver and Qt HttpServer
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

To build the library and the renditions, run the admin service next to it –
continuously, or once:

```bash
build/admin/berry-kiosk-admin --config config.json          # watches media/ and keeps going
build/admin/berry-kiosk-admin --config config.json --once   # sync, render, exit
```

Running continuously, it also serves the API and web UI on
http://localhost:8080. Set a password first:

```bash
build/admin/berry-kiosk-admin --config config.json --set-password
```

## Installing on a Raspberry Pi

Requires **Raspberry Pi OS Lite, 64-bit, based on Debian 13 "trixie"** (Qt ≥ 6.8 for Qt HttpServer).

```bash
git clone https://github.com/herrlolstick/berry-kiosk.git
cd berry-kiosk
./scripts/install-pi.sh
```

The script builds on the Pi, creates the system user `berry-kiosk`, copies the
example configuration to `/var/lib/berry-kiosk/config.json` and starts the
services `berry-kiosk-display` and `berry-kiosk-admin`. On first install it asks
for the admin password; the web UI is then at `http://<pi>.local:8080/`.
Pictures go into `/var/lib/berry-kiosk/media/` – copied in, or uploaded via the web.

```bash
journalctl -u berry-kiosk-display -u berry-kiosk-admin -f     # logs
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
| `library.renditions` | `"auto"` (the display's tier) or a list such as `["1080p", "2160p"]` |
| `library.jpegQuality` | JPEG quality of renditions, `50` … `100`, default `88` |
| `library.database`, `library.cache`, `library.state` | Paths, default `library.db`, `cache`, `state` – e.g. put the cache on a USB drive |
| `library.trash` | Where deleted pictures are moved, default `trash` |
| `admin.address` | `"0.0.0.0"` (every interface) or `"127.0.0.1"` (only the device, e.g. behind a reverse proxy) |
| `admin.port` | Port of the API and web UI, default `8080` |
| `admin.maxUploadMB` | Largest upload in MB, default `64` |
| `quotes` | Global quotes: `[{ "text": "…", "author": "…" }]` or plain strings |
| `activeLayout` | `id` of the layout to show |
| `layouts[].name` | Display name in the web UI (optional; `id` stays stable) |
| `layouts[].grid` | Grid size, e.g. `{ "columns": 12, "rows": 12 }` |
| `layouts[].regions[]` | Regions: `type`, position `x`/`y` and size `w`/`h` in grid cells, optional `id` and `z` |

### `slideshow`

| Field | Values | Default |
|---|---|---|
| `album` | Sub folder of `media.path`, empty = all pictures | `""` |
| `order` | `"sequential"`, `"shuffle"` | `"sequential"` |
| `interval` | Seconds per picture | `15` |
| `fit` | `"cover"` (fill), `"contain"` (show whole picture) | `"cover"` |
| `transition.type` | `"fade"`, `"slide-left"`, `"slide-right"`, `"slide-up"`, `"slide-down"`, `"zoom"`, `"kenburns"` (fade plus a slow zoom and pan while the picture shows), `"mix"` (a random fade, slide or zoom each time), `"none"` | `"fade"` |
| `transition.duration` | Milliseconds | `1200` |
| `backdrop` | With `fit: "contain"`: fill the bars with a blurred copy of the picture | `true` |

If two transitions in a row run below 20 frames per second, the region switches to
plain fades for the rest of its life – the show stays smooth on a Pi Zero. The
blurred backdrop is a tiny decode that the GPU scales up, so it costs next to nothing.

The display shows the preview layout from the editor first, then the layout the
schedule asks for (milestone 8), then `activeLayout`. Switching layouts fades
through black.

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

An empty string hides a value that is embedded in the picture. `"enabled": false`
hides the picture from the slideshow.

## Project layout

```
core/        libberrycore: configuration, render tier, metadata, media library, renditions
display/     berry-kiosk-display: the Qt Quick display (C++ and QML)
admin/       berry-kiosk-admin: library sync, renditions and web API
admin/www/   the web UI (ES modules and CSS, compiled in)
tests/       unit and API tests
packaging/   systemd units
scripts/     install script for the Pi
docs/        architecture and planning (German)
```

## Roadmap

1. ✅ **Foundation** – slideshow, grid layouts, 4K/1080p detection, systemd
2. ✅ **Widgets** – clock, image info (EXIF/IPTC/XMP), quotes (sidecar or global list)
3. ✅ **Media library** – SQLite index, image pipeline with 1080p/2160p renditions and thumbnails
4. ✅ **Admin server** – REST API, login, uploads, metadata editing, remote control of the display
5. ✅ **Web UI I** – media library, uploads, albums and settings; moved files keep their renditions
6. ✅ **Web UI II** – layout editor with preview on the kiosk, transition settings, quotes
7. ✅ **Transitions** – slide, zoom, Ken Burns, mix, blurred backdrop, automatic fallback to fades
8. **Operations** – schedule, screen off (DPMS/CEC), public profile, read-only root, watchdog, HDMI hotplug
9. **Distribution** – `.deb` packages and a ready-made SD card image

## License

[MIT](LICENSE). berry-kiosk uses Qt under the LGPLv3 (dynamically linked) and Exiv2 under the GPLv2+.
