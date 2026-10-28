#!/usr/bin/env bash
# install.sh — turn a Debian-based system (Raspberry Pi OS Lite, Debian minimal) into a
# Soundshed Guitar kiosk that boots straight into the app.
#
# Run as root on the target, or inside an image chroot (the pi-gen stage does this).
#
# Usage: sudo ./install.sh --dist <dir> [--no-apt]
#   --dist <dir>   A staged Linux distribution folder from build_linux.sh
#                  (linux-dist-<arch>, holding opt/Soundshed/soundshed-guitar)
#   --no-apt       Skip installing packages.txt (the image build installs them itself)
#
# Re-running it updates the app and the kiosk files in place.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS_SRC="${SCRIPT_DIR}/rootfs"
APP_DIR_NAME="soundshed-guitar"
APP_DST="/opt/Soundshed/${APP_DIR_NAME}"
KIOSK_USER="soundshed"
# The kernel options this needs: interrupt handlers as threads (so tune-audio can raise
# the USB controller's), and no USB autosuspend (an interface that sleeps drops out).
CMDLINE_OPTS=(threadirqs usbcore.autosuspend=-1)

DIST_DIR=""
INSTALL_PACKAGES=true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dist)   DIST_DIR="$2"; shift ;;
        --no-apt) INSTALL_PACKAGES=false ;;
        --help)   sed -n '2,13p' "$0"; exit 0 ;;
        *)        echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

if [[ $EUID -ne 0 ]]; then
    echo "Run as root." >&2
    exit 1
fi

APP_SRC="${DIST_DIR}/opt/Soundshed/${APP_DIR_NAME}"
if [[ -z "$DIST_DIR" || ! -x "${APP_SRC}/${APP_DIR_NAME}" ]]; then
    echo "--dist must point at a build_linux.sh distribution folder; no ${APP_SRC}/${APP_DIR_NAME}" >&2
    exit 1
fi

if [[ "$INSTALL_PACKAGES" == true ]]; then
    echo "▶ Installing packages…"
    mapfile -t packages < <(grep -v '^\s*\(#\|$\)' "${SCRIPT_DIR}/packages.txt")
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
fi

echo "▶ Installing the app → ${APP_DST}"
rm -rf "$APP_DST"
mkdir -p "$(dirname "$APP_DST")"
cp -R "$APP_SRC" "$APP_DST"
chown -R root:root "$APP_DST"

# The app keeps settings, presets and the WebView's data under its home
# (~/.config/Soundshed Guitar, ~/.local/share, ~/.cache), so the home must be writable.
echo "▶ Kiosk user ${KIOSK_USER}"
if ! id "$KIOSK_USER" >/dev/null 2>&1; then
    useradd --create-home --shell /usr/sbin/nologin "$KIOSK_USER"
fi
for group in audio video render input; do
    getent group "$group" >/dev/null && usermod -aG "$group" "$KIOSK_USER"
done

echo "▶ Kiosk files"
install -D -m 0644 "${ROOTFS_SRC}/etc/systemd/system/soundshed-kiosk.service" /etc/systemd/system/soundshed-kiosk.service
install -D -m 0644 "${ROOTFS_SRC}/etc/systemd/system/soundshed-audio-tune.service" /etc/systemd/system/soundshed-audio-tune.service
install -D -m 0644 "${ROOTFS_SRC}/etc/security/limits.d/95-soundshed-audio.conf" /etc/security/limits.d/95-soundshed-audio.conf
install -D -m 0755 "${ROOTFS_SRC}/usr/local/lib/soundshed-kiosk/session" /usr/local/lib/soundshed-kiosk/session
install -D -m 0755 "${ROOTFS_SRC}/usr/local/lib/soundshed-kiosk/xsession" /usr/local/lib/soundshed-kiosk/xsession
install -D -m 0755 "${ROOTFS_SRC}/usr/local/lib/soundshed-kiosk/tune-audio" /usr/local/lib/soundshed-kiosk/tune-audio
# Keep local edits to the settings file on a re-run.
if [[ ! -e /etc/default/soundshed-kiosk ]]; then
    install -D -m 0644 "${ROOTFS_SRC}/etc/default/soundshed-kiosk" /etc/default/soundshed-kiosk
fi

# Raspberry Pi OS keeps the one-line kernel command line on the boot partition.
cmdline=""
for candidate in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
    [[ -f "$candidate" ]] && { cmdline="$candidate"; break; }
done
if [[ -n "$cmdline" ]]; then
    for opt in "${CMDLINE_OPTS[@]}"; do
        if ! grep -qw -- "$opt" "$cmdline"; then
            sed -i "1 s|\$| ${opt}|" "$cmdline"
            echo "  ✓ ${opt} → ${cmdline}"
        fi
    done
else
    echo "  ! No cmdline.txt; add these kernel options by hand: ${CMDLINE_OPTS[*]}"
fi

echo "▶ Enabling services"
# Only a booted system has a manager to reload; in a chroot, enable just writes links.
if [[ -d /run/systemd/system ]]; then
    systemctl daemon-reload
fi
systemctl enable soundshed-audio-tune.service soundshed-kiosk.service
systemctl set-default graphical.target

# A missing library would only show up as a restart loop in the journal.
missing="$(ldd "${APP_DST}/${APP_DIR_NAME}" 2>/dev/null | grep 'not found' || true)"
if [[ -n "$missing" ]]; then
    echo "  ! The app is missing libraries:" >&2
    echo "$missing" >&2
    exit 1
fi
# JUCE opens WebKitGTK with dlopen at run time, so ldd cannot see it.
if ! ldconfig -p | grep -qE 'libwebkit2gtk-4\.(1\.so\.0|0\.so\.37)'; then
    echo "  ! No WebKitGTK (libwebkit2gtk-4.1 or 4.0); the UI will not load." >&2
    exit 1
fi

echo "✓ Done. Reboot to start the kiosk (journalctl -u soundshed-kiosk to watch it)."
