#!/usr/bin/env bash
# Builds berry-kiosk on the Raspberry Pi as a Debian package and installs it.
# Target: Raspberry Pi OS Lite, 64-bit, based on Debian 13 "trixie" (Qt 6.8).
#
#   git clone https://github.com/herrlolstick/berry-kiosk.git
#   cd berry-kiosk && ./scripts/install-pi.sh
#
# Faster: download the ready-made package from the GitHub releases and run
#   sudo apt install ./berry-kiosk_<version>_arm64.deb
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build-pi"
STATE_DIR=/var/lib/berry-kiosk
SERVICE_USER=berry-kiosk

echo "==> Pakete installieren"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build dpkg-dev file \
    qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
    qt6-httpserver-dev qt6-websockets-dev \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    qt6-qpa-plugins qt6-image-formats-plugins libqt6sql6-sqlite libexiv2-dev libgl-dev libegl-dev \
    fonts-dejavu-core

echo "==> Bauen"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBERRY_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR"
(cd "$BUILD_DIR" && rm -f berry-kiosk_*.deb && cpack -G DEB)

echo "==> Installieren"
# The package creates the user, the data directory, the udev rule and enables the services
sudo apt-get install -y "$(ls "$BUILD_DIR"/berry-kiosk_*.deb)"
# The login user may copy pictures into the media folder (after the next login)
sudo usermod -aG "$SERVICE_USER" "$USER"

echo "==> Zugang zur Verwaltung"
ADMIN="berry-kiosk-admin --config $STATE_DIR/config.json --set-password"
if sudo test -f "$STATE_DIR/state/auth.json"; then
    echo "Passwort ist bereits gesetzt."
elif [ -t 0 ]; then
    # Abbruch oder Tippfehler sollen die fertige Installation nicht als gescheitert melden
    sudo -u "$SERVICE_USER" $ADMIN || echo "Passwort nicht gesetzt – später nachholen (siehe unten)."
else
    echo "Kein Terminal: Passwort später setzen (siehe unten)."
fi

echo
echo "Fertig. Nach dem nächsten Login darf $USER Bilder nach $STATE_DIR/media kopieren, z. B. vom Mac:"
echo "  rsync -av ~/Bilder/Natur/ $USER@$(hostname).local:$STATE_DIR/media/"
echo "Verwaltung: http://$(hostname).local:8080/"
echo "Passwort setzen/ändern: sudo -u $SERVICE_USER $ADMIN"
echo "Logs: journalctl -u berry-kiosk-display -u berry-kiosk-admin -f"
echo
echo "Optional, um den Bildschirm per Zeitplan wirklich abzuschalten (display.power):"
echo "  Fernseher (HDMI-CEC): sudo apt install v4l-utils"
echo "  Monitore (DDC/CI):    sudo apt install ddcutil && sudo usermod -aG i2c $SERVICE_USER"
