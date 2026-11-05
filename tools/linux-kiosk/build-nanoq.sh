#!/usr/bin/env bash
# build-nanoq.sh — build Soundshed Nano Q for the kiosk, in a Debian trixie container (the
# release Raspberry Pi OS is built on), so the binary links the libraries on the device.
# An arm64 build cross-compiles, so it needs no Pi and no qemu.
#
# Usage: ./build-nanoq.sh [options]
#   --arch <arch>   arm64 (default) or x64
#   --out <dir>     Where the distribution folder goes, for install.sh and build-image.sh
#                   --dist. Must be on a Linux filesystem, not /mnt/c, or the executable bit
#                   is lost (default: ~/nanoq-dist-<arch>)
#   --clean         Start from an empty build tree
#
# Needs Docker. The build tree stays in a Docker volume, soundshed-nanoq-<arch>, so a rebuild
# only compiles what changed. `docker volume rm` it to get the space back.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

ARCH="arm64"
OUT_DIR=""
CLEAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)  ARCH="$2"; shift ;;
        --out)   OUT_DIR="$2"; shift ;;
        --clean) CLEAN=true ;;
        --help)  sed -n '2,15p' "$0"; exit 0 ;;
        *)       echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

case "$ARCH" in
    arm64) debian_arch=arm64; gnu_triple=aarch64-linux-gnu ;;
    x64)   debian_arch=amd64; gnu_triple=x86_64-linux-gnu ;;
    *)     echo "--arch must be arm64 or x64" >&2; exit 1 ;;
esac
OUT_DIR="${OUT_DIR:-${HOME}/nanoq-dist-${ARCH}}"
image="soundshed-nanoq-build:${ARCH}"
volume="soundshed-nanoq-${ARCH}"

echo "▶ Build image ${image}…"
docker build --platform linux/amd64 -t "$image" - >/dev/null <<EOF
FROM debian:trixie
RUN dpkg --add-architecture ${debian_arch} \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git curl ca-certificates python3 file \
        crossbuild-essential-${debian_arch} \
        libcairo2-dev:${debian_arch} libfontconfig-dev:${debian_arch} libfreetype-dev:${debian_arch} \
        libharfbuzz-dev:${debian_arch} libwebp-dev:${debian_arch} libpng-dev:${debian_arch} \
        libx11-dev:${debian_arch} libxcursor-dev:${debian_arch} libxrandr-dev:${debian_arch} \
        libxext-dev:${debian_arch} libxi-dev:${debian_arch} libasound2-dev:${debian_arch} \
        libbrotli-dev:${debian_arch} libbz2-dev:${debian_arch} libcurl4-openssl-dev:${debian_arch}
EOF

if [[ "$CLEAN" == true ]]; then
    docker volume rm "$volume" >/dev/null 2>&1 || true
fi
docker volume create "$volume" >/dev/null

toolchain=""
if [[ "$ARCH" == "arm64" ]]; then
    toolchain="-DCMAKE_TOOLCHAIN_FILE=/w/src/cmake/toolchains/linux-arm64.cmake"
fi

echo "▶ Copying the sources into ${volume}…"
# Copied, not mounted: Docker Desktop's mounts of a Windows checkout are slow, and the patches
# in such a checkout have CRLF line endings that a fresh Linux dependency checkout doesn't.
tar -C "$REPO_DIR" -cf - \
    --exclude='core/build*' --exclude='nanoq/build' --exclude='core/ui/node_modules' \
    --exclude='core/ui/dist' --exclude='core/ui/tests' --exclude='Dependencies/eigen/.git' \
    nanoq core cmake Dependencies/eigen |
    docker run -i --rm --platform linux/amd64 -v "${volume}:/w" "$image" bash -c '
        rm -rf /w/src && mkdir -p /w/src && tar -xf - -C /w/src
        sed -i "s/\r$//" /w/src/nanoq/patches/*.patch /w/src/nanoq/ios/patches/*.patch'

echo "▶ Building Nano Q for ${ARCH} (the first build takes a while)…"
docker run --rm --platform linux/amd64 -v "${volume}:/w" \
    -e PKG_CONFIG_LIBDIR="/usr/lib/${gnu_triple}/pkgconfig:/usr/share/pkgconfig" \
    -e GUITARFX_LINUX_PKG_CONFIG_LIBDIR="/usr/lib/${gnu_triple}/pkgconfig:/usr/share/pkgconfig" \
    "$image" bash -euc "
        git config --global --add safe.directory '*'
        cmake -S /w/src/nanoq -B /w/build -G Ninja -DCMAKE_BUILD_TYPE=Release ${toolchain}
        cmake --build /w/build --target nanoq_standalone
        ${gnu_triple}-strip '/w/build/products/Soundshed Nano Q'
        rm -rf /w/dist
        mkdir -p /w/dist/opt/Soundshed/soundshed-nano-q
        cp -R '/w/build/products/Soundshed Nano Q' '/w/build/products/Soundshed Nano Q Resources' \
            /w/build/products/resources /w/dist/opt/Soundshed/soundshed-nano-q/"

echo "▶ Copying the distribution to ${OUT_DIR}…"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
docker run --rm --platform linux/amd64 -v "${volume}:/w" "$image" tar -C /w/dist -cf - . |
    tar -xf - -C "$OUT_DIR"

app="${OUT_DIR}/opt/Soundshed/soundshed-nano-q/Soundshed Nano Q"
if [[ ! -x "$app" ]]; then
    echo "No executable at ${app}: is ${OUT_DIR} on a Linux filesystem?" >&2
    exit 1
fi
echo "✓ ${OUT_DIR}"
