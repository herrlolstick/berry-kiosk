#!/usr/bin/env bash
# Builds berry-kiosk on the Raspberry Pi itself and installs it as a systemd service.
# Target: Raspberry Pi OS Lite, 64-bit, based on Debian 13 "trixie" (Qt 6.8).
#
#   git clone https://github.com/herrlolstick/berry-kiosk.git
#   cd berry-kiosk && ./scripts/install-pi.sh
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build-pi"
STATE_DIR=/var/lib/berry-kiosk
SERVICE_USER=berry-kiosk

echo "==> Pakete installieren"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build \
    qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    qt6-qpa-plugins qt6-image-formats-plugins libexiv2-dev libgl-dev libegl-dev \
    fonts-dejavu-core

echo "==> Bauen"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBERRY_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR"
sudo cmake --install "$BUILD_DIR"

echo "==> Benutzer und Daten"
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
    sudo useradd --system --home-dir "$STATE_DIR" --shell /usr/sbin/nologin \
        --groups video,render,input "$SERVICE_USER"
fi
sudo install -d -o "$SERVICE_USER" -g "$SERVICE_USER" "$STATE_DIR"
# Group-writable so the login user can copy pictures in (e.g. with rsync)
sudo install -d -o "$SERVICE_USER" -g "$SERVICE_USER" -m 2775 "$STATE_DIR/media"
sudo usermod -aG "$SERVICE_USER" "$USER"
if [ ! -f "$STATE_DIR/config.json" ]; then
    sudo install -o "$SERVICE_USER" -g "$SERVICE_USER" -m 0644 "$REPO_DIR/config.example.json" "$STATE_DIR/config.json"
fi

echo "==> Dienst einrichten"
sudo install -m 0644 "$REPO_DIR/packaging/systemd/berry-kiosk-display.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now berry-kiosk-display.service

echo
echo "Fertig. Nach dem nächsten Login darf $USER Bilder nach $STATE_DIR/media kopieren, z. B. vom Mac:"
echo "  rsync -av ~/Bilder/Natur/ $USER@$(hostname).local:$STATE_DIR/media/"
echo "Logs: journalctl -u berry-kiosk-display -f"
