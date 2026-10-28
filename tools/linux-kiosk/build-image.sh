#!/usr/bin/env bash
# build-image.sh — build a Raspberry Pi 5 SD card image that boots into Soundshed Guitar.
#
# Raspberry Pi OS Lite (pi-gen stages 0-2, at a pinned tag) plus one stage of our own
# that runs install.sh in the image. Needs Linux (or WSL2) with Docker; pi-gen's
# build-docker.sh does the rest, cross-building arm64 through qemu-user.
#
# Usage: SOUNDSHED_ADMIN_PASS=... ./build-image.sh --dist <dir> [options]
#   --dist <dir>        An arm64 distribution folder from `build_linux.sh --arch arm64`
#                       (linux-dist-arm64), or the same layout unzipped from a CI artifact
#   --work-dir <dir>    Where pi-gen is checked out and built; must be on a Linux
#                       filesystem, not /mnt/c (default: ~/.cache/soundshed-pi-gen)
#   --pi-gen-ref <tag>  pi-gen tag to build from (default: PI_GEN_REF below)
#   --hostname <name>   Image hostname (default: soundshed)
#
# Environment:
#   SOUNDSHED_ADMIN_PASS    Required. Password for the admin user (SSH and tty1). It goes
#                           into the work dir's pi-gen config only, never this repo.
#   SOUNDSHED_ADMIN_USER    Admin user name (default: admin)
#   SOUNDSHED_ADMIN_PUBKEY  Optional authorized_keys contents; when set, SSH takes keys only
#   SOUNDSHED_WPA_COUNTRY   Optional Wi-Fi regulatory country (two letters, e.g. GB)
#
# The image lands in <work-dir>/pi-gen/deploy/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Bumping this is deliberate: list newer tags with
#   git ls-remote --tags https://github.com/RPi-Distro/pi-gen.git | grep arm64
PI_GEN_REF="2026-10-06-raspios-trixie-arm64"
PI_GEN_URL="https://github.com/RPi-Distro/pi-gen.git"

DIST_DIR=""
WORK_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/soundshed-pi-gen"
HOSTNAME_VALUE="soundshed"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dist)       DIST_DIR="$2"; shift ;;
        --work-dir)   WORK_DIR="$2"; shift ;;
        --pi-gen-ref) PI_GEN_REF="$2"; shift ;;
        --hostname)   HOSTNAME_VALUE="$2"; shift ;;
        --help)       sed -n '2,24p' "$0"; exit 0 ;;
        *)            echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Run this on Linux or in WSL2 (pi-gen needs Docker and a Linux filesystem)." >&2
    exit 1
fi
if [[ -z "${SOUNDSHED_ADMIN_PASS:-}" ]]; then
    echo "Set SOUNDSHED_ADMIN_PASS (pi-gen locks a first user with no password, and" >&2
    echo "without one the first-boot rename wizard would sit in front of the kiosk)." >&2
    exit 1
fi

app="${DIST_DIR}/opt/Soundshed/soundshed-guitar/soundshed-guitar"
if [[ -z "$DIST_DIR" || ! -x "$app" ]]; then
    echo "--dist must point at a Linux distribution folder; no executable at ${app}" >&2
    exit 1
fi
if command -v file >/dev/null 2>&1 && ! file -L "$app" | grep -q 'aarch64'; then
    echo "${app} is not an arm64 build: $(file -L "$app")" >&2
    exit 1
fi
DIST_DIR="$(cd "$DIST_DIR" && pwd)"

pigen="${WORK_DIR}/pi-gen"
if [[ -d "${pigen}/.git" ]]; then
    current="$(git -C "$pigen" describe --tags --exact-match 2>/dev/null || true)"
    if [[ "$current" != "$PI_GEN_REF" ]]; then
        echo "${pigen} is at '${current:-untagged}', not ${PI_GEN_REF}; remove it to rebuild." >&2
        exit 1
    fi
else
    mkdir -p "$WORK_DIR"
    git clone --depth 1 --branch "$PI_GEN_REF" "$PI_GEN_URL" "$pigen"
fi

echo "▶ Staging the Soundshed stage into pi-gen…"
stage="${pigen}/stage-soundshed"
rm -rf "$stage"
cp -R "${SCRIPT_DIR}/pi-gen/stage-soundshed" "$stage"
mkdir -p "${stage}/00-soundshed/files"
# The kiosk files without the image tooling itself.
mkdir -p "${stage}/00-soundshed/files/kiosk"
cp -R "${SCRIPT_DIR}/rootfs" "${SCRIPT_DIR}/install.sh" "${SCRIPT_DIR}/packages.txt" \
    "${stage}/00-soundshed/files/kiosk/"
cp -R "$DIST_DIR" "${stage}/00-soundshed/files/app"
grep -v '^\s*\(#\|$\)' "${SCRIPT_DIR}/packages.txt" > "${stage}/00-soundshed/00-packages-nr"
# pi-gen only runs scripts that are executable, and a Windows checkout loses the bit.
chmod +x "${stage}/prerun.sh" "${stage}/00-soundshed/01-run.sh"

# Lite (stage2) is the base; only our stage exports an image.
touch "${pigen}/stage2/SKIP_IMAGES"

# printf %q keeps a password with spaces or quotes intact in the sourced config.
{
    echo "IMG_NAME=soundshed-guitar"
    echo "TARGET_HOSTNAME=$(printf '%q' "$HOSTNAME_VALUE")"
    echo "FIRST_USER_NAME=$(printf '%q' "${SOUNDSHED_ADMIN_USER:-admin}")"
    echo "FIRST_USER_PASS=$(printf '%q' "$SOUNDSHED_ADMIN_PASS")"
    echo "DISABLE_FIRST_BOOT_USER_RENAME=1"
    echo "ENABLE_SSH=1"
    if [[ -n "${SOUNDSHED_ADMIN_PUBKEY:-}" ]]; then
        echo "PUBKEY_SSH_FIRST_USER=$(printf '%q' "$SOUNDSHED_ADMIN_PUBKEY")"
        echo "PUBKEY_ONLY_SSH=1"
    fi
    if [[ -n "${SOUNDSHED_WPA_COUNTRY:-}" ]]; then
        echo "WPA_COUNTRY=$(printf '%q' "$SOUNDSHED_WPA_COUNTRY")"
    fi
    echo "STAGE_LIST=\"stage0 stage1 stage2 stage-soundshed\""
    echo "DEPLOY_COMPRESSION=xz"
} > "${pigen}/config"
chmod 600 "${pigen}/config"

echo "▶ Building with pi-gen ${PI_GEN_REF} (this takes a while)…"
(cd "$pigen" && ./build-docker.sh)

echo "✓ Image:"
ls -1 "${pigen}/deploy/"*soundshed* 2>/dev/null || ls -1 "${pigen}/deploy/"
