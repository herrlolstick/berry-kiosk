# berry-kiosk

A photo frame and kiosk display for the Raspberry Pi, built to show nature
photography in a living room or a public space. It runs without a desktop,
straight on the screen (Qt Quick via eglfs/KMS), works on low-cost boards like
the Pi Zero 2 W and uses 4K wherever the screen and the Pi can handle it.

> **Status:** 1.0 – slideshow with transitions, widgets, media library, image
> pipeline, admin web API, the web UI incl. the layout editor, operations
> (schedule, screen off, public profile, watchdog, HDMI hotplug) and a Debian
> package for Raspberry Pi OS. Not yet tested on every Pi model – feedback welcome.

## Features

- Slideshow from a folder (sub folders are albums), sequential or shuffled, with transitions: fade, slide, zoom, Ken Burns or a random mix – falling back to fades on boards that cannot keep up
- **Grid layouts:** regions are placed on grid cells, e.g. one large picture and two small ones (collage), edited in the browser with a live preview on the kiosk
- **Widgets:** clock, image info (title, location, date, camera settings from EXIF/IPTC/XMP) and quotes
- **4K with automatic fallback to Full HD** (see below)
- **Media library** (SQLite) with prepared 1080p/2160p renditions and thumbnails, kept up to date in the background
- **Web UI** for phone and desktop: media library with search, filters and bulk actions, uploads (whole folders too), albums, editing titles and quotes, remote control of the display (next picture, pause, live screenshot)
- **Admin web API** behind it, with login and CSRF protection
- **Schedule:** a layout per time of day and weekday, screen off at night – with HDMI-CEC standby for TVs or DDC/CI for monitors
- **Public profile** for kiosks in public spaces: no input devices, short admin sessions, the web API can be switched off entirely
- **Self-healing:** systemd watchdog for display and admin service, the display restarts on HDMI hotplug
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
| Übersicht | Live screenshot of the kiosk, status, next picture / pause, screen on/off, library and storage, latest pictures |
| Mediathek | Grid with infinite scroll; filter by album, search, shown/hidden, sort; select pictures to show, hide, move or delete them; detail view with a large preview, metadata and editing of title, location, description, quote and "in der Diashow zeigen", arrow keys and swipe for the next picture |
| Hochladen | Drop zone for files and whole folders (sub folders become sub albums), queue with progress and retries; one file at a time, as the server holds an upload in memory |
| Alben | Album tree with covers and counts: create, rename/move, delete (to the trash) |
| Layouts | Grid editor: drag and resize regions on the grid, templates (full screen, with clock and image info, collage, portrait pair), properties per region (album, order, interval, fit, transition; widget style), undo/redo, keyboard (arrows move, Shift+arrows resize, Delete removes); "Auf Kiosk vorschauen" shows the unsaved layout on the kiosk for 60 s and fetches a screenshot; activate a layout; a "Zitate" tab for the global quotes |
| Zeitplan | Rules per weekday and time: which layout to show, when the screen is off |
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
- After 5 failed logins from one address (IPv6: one /64 network), the wait doubles
  with every further failure, from 30 seconds up to 15 minutes. More than 30 failed
  logins within 10 minutes from anywhere pause all logins for a minute. Changing the
  password counts the same way. Behind a reverse proxy all clients share one address.
- The web UI only answers to IP addresses, `localhost` and the device's own name
  (`<name>` and `<name>.local`); other host names are refused, which stops DNS
  rebinding attacks. Behind a reverse proxy with its own name, add that name to
  `admin.allowedHosts`.
- The locations of pictures, library, cache, state and trash (`media.path`,
  `library.*`) cannot be changed through the API, only in `config.json` on the
  device; the password always lives in `state/auth.json` next to `config.json`.
- Symbolic links inside the media folder are ignored, and nothing is ever written
  through one. `media.path` itself may point anywhere, e.g. to a NAS mount.
- Uploads are checked by content and size (at most 200 megapixels for JPEG,
  60 for PNG/WebP); every image decode is capped at 192 MB, so hostile files fail
  instead of exhausting the Pi's memory.
- It speaks plain HTTP and is meant for the home network. For anything else, put
  it behind a TLS reverse proxy (e.g. on a NAS or Umbrel) and set `admin.address`
  to `127.0.0.1`, or firewall the port.
- Request bodies are held in memory (up to `admin.maxUploadMB`), so do not expose
  the port to untrusted networks. The admin service is capped at 256 MB by systemd
  and is the first to be stopped when memory runs out; the display is a separate
  process and keeps running.
- Both services run as the unprivileged user `berry-kiosk` with systemd hardening.
  The display gets the GPU (`video`, `render`) but no input devices; for a touch
  screen or keyboard add them with `sudo systemctl edit berry-kiosk-display`:
  `[Service]` `SupplementaryGroups=video render input`.

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
| `POST` | `/display/screen` | `{ "on": true\|false }`: switch the screen by hand; holds until the schedule next changes |
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

**From a release** (fastest): download `berry-kiosk_<version>_arm64.deb` from the
[releases](https://github.com/herrlolstick/berry-kiosk/releases) and run

```bash
sudo apt install ./berry-kiosk_1.0.1_arm64.deb
sudo -u berry-kiosk berry-kiosk-admin --config /var/lib/berry-kiosk/config.json --set-password
```

**From source**: the install script builds the same package on the Pi and installs it.

```bash
git clone https://github.com/herrlolstick/berry-kiosk.git
cd berry-kiosk
./scripts/install-pi.sh
```

The package creates the system user `berry-kiosk`, the data directory
`/var/lib/berry-kiosk` (via systemd-tmpfiles) with the example configuration,
installs the udev rule and starts the services `berry-kiosk-display` and
`berry-kiosk-admin`. The web UI is then at `http://<pi>.local:8080/`. Pictures go
into `/var/lib/berry-kiosk/media/` – copied in, or uploaded via the web. Removing
the package never deletes pictures, library or settings.

```bash
journalctl -u berry-kiosk-display -u berry-kiosk-admin -f     # logs
sudo systemctl restart berry-kiosk-display
```

> Not yet tested on every Pi model – feedback welcome.

## Configuration

See [`config.example.json`](config.example.json). Top level:

| Field | Meaning |
|---|---|
| `profile` | `"home"` (default) or `"public"`, see [Running in public spaces](#running-in-public-spaces) |
| `locale` | Language for dates and numbers, e.g. `"de_DE"`, `"en_GB"` |
| `display.maxRender` | `"auto"` (4K when possible) or `"1080p"` |
| `display.rotation` | `0`, `90`, `180`, `270` – for screens mounted in portrait |
| `display.power` | How the schedule switches the screen off: `"none"` (black picture, default), `"cec"` or `"ddc"` – see [Schedule](#schedule) |
| `media.path` | Picture folder, relative to the configuration file or absolute |
| `library.renditions` | `"auto"` (the display's tier) or a list such as `["1080p", "2160p"]` |
| `library.jpegQuality` | JPEG quality of renditions, `50` … `100`, default `88` |
| `library.database`, `library.cache`, `library.state` | Paths, default `library.db`, `cache`, `state` – e.g. put the cache on a USB drive (editable only on the device) |
| `library.trash` | Where deleted pictures are moved, default `trash` |
| `admin.address` | `"0.0.0.0"` (every interface) or `"127.0.0.1"` (only the device, e.g. behind a reverse proxy) |
| `admin.port` | Port of the API and web UI, default `8080` |
| `admin.maxUploadMB` | Largest upload in MB, `1` … `128`, default `64` |
| `admin.allowedHosts` | Extra host names the web UI answers to, e.g. `["kiosk.example.org"]` behind a reverse proxy |
| `admin.enabled` | `false` switches the web API and UI off; the library is still kept up to date. Default `true` |
| `schedule` | Layouts and screen off by time, see [Schedule](#schedule) |
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

### Schedule

`schedule` is a list of rules; at any moment the last rule that matches wins.
Each rule has `days`, `from`, `to` and either `layout` (a layout `id`) or
`"screen": "off"`:

```json
"schedule": [
  { "days": "mo-fr", "from": "07:00", "to": "22:30", "layout": "tag" },
  { "days": "sa,so", "from": "09:00", "to": "23:00", "layout": "wochenende" },
  { "days": "daily", "from": "22:30", "to": "07:00", "screen": "off" }
]
```

- `days`: `"daily"`/`"täglich"`, German or English abbreviations or names with
  ranges and lists (`"mo-fr"`, `"sa,so"`, `"fri-mon"` wraps around the weekend),
  or an array of names or numbers 1 (Monday) … 7. Missing = every day.
- `from`/`to`: `HH:mm` (`7:00` works too); `from` is included, `to` is not. A `to`
  before `from` runs past midnight: the morning part belongs to the day after a
  listed day, so `"fr"` 22:00–02:00 covers Saturday 01:00. `from` = `to` means the
  whole day.
- No matching rule: the `activeLayout` shows and the screen is on. A preview from
  the layout editor still takes precedence over the schedule.
- Invalid rules are skipped with a message in the display's log.
- The schedule runs inside the display process, so it also works when the admin
  service is stopped. The screen switch in the web UI (`POST /api/display/screen`)
  holds until the schedule next changes its mind.

With the screen off the display unloads the layout (nothing renders) and, with
`display.power`, sends the screen to sleep:

| `display.power` | Off / on | Needs |
|---|---|---|
| `"none"` | black picture only | – |
| `"cec"` | `cec-ctl -d0 --playback --to 0 --standby` / `--image-view-on` (TVs) | `v4l-utils` |
| `"ddc"` | `ddcutil setvcp d6 04` / `ddcutil setvcp d6 01` (monitors with DDC/CI) | `ddcutil`, the service user in the `i2c` group |

These fixed commands are the only ones: there is deliberately no setting for a
free-form command, because `config.json` can be edited through the web API.

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

## Running in public spaces

- Set `"profile": "public"`: the display ignores keyboards, mice and touch screens
  (eglfs input is switched off) and has no shortcuts even in debug mode; admin
  sessions last 2 hours instead of 12; the admin service warns in its log when it
  listens on every interface.
- Restrict the admin service: `admin.address` `"127.0.0.1"` (only via SSH tunnel or
  a reverse proxy on the device) or `admin.enabled: false` (manage pictures by
  copying files, e.g. with rsync or Syncthing).
- Read-only root file system, so power cuts cannot corrupt the SD card:
  `sudo raspi-config nonint do_overlayfs 0` (and reboot). Everything written then
  vanishes on reboot, so put the data directory on a separate writable partition or
  a USB stick and point `media.path` and `library.database`/`library.cache`/
  `library.state`/`library.trash` there.
- Both services run under systemd with a watchdog: a display or admin service that
  stops responding is restarted automatically.
- The udev rule `packaging/udev/99-berry-kiosk-hotplug.rules` (installed by the
  install script) restarts the display when a screen is plugged in or swapped, so
  it picks the new screen's mode and render tier.

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
8. ✅ **Operations** – schedule, screen off (CEC/DDC), public profile, read-only root, watchdog, HDMI hotplug
9. ✅ **Distribution** – Debian package, release workflow for tags, security review
10. **Later** – a ready-made SD card image (pi-gen), more widgets (weather, QR code)

## License

[MIT](LICENSE). berry-kiosk uses Qt under the LGPLv3 (dynamically linked) and Exiv2 under the GPLv2+.
