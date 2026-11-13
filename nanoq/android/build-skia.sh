#!/usr/bin/env bash
# Builds the Skia that Nano Q's Android app draws with, on the GPU through OpenGL ES. Artist
# publishes prebuilt Skia for macOS, Windows and Linux only, so Android builds its own, as iOS
# does (nanoq/ios/build-skia.sh): Artist's own vcpkg port (Skia m148) and the vcpkg Artist pins,
# for the triplet nanoq/android/cmake/vcpkg-triplets/arm64-android-nanoq.cmake.
#
# Runs on Linux. From Windows, nanoq/android/build-skia.ps1 runs it in a Debian container.
#
# Usage: nanoq/android/build-skia.sh [work-dir]
#
#   work-dir   where vcpkg, Artist's ports, the NDK and vcpkg's caches go (default
#              nanoq/android/skia/work). A container keeps them in a volume.
#
# The installed tree ends up in nanoq/android/skia/arm64-android-nanoq (ignored by git), which
# the Gradle build hands to CMake as NANOQ_ANDROID_SKIA. The first run takes a while (vcpkg
# builds gn, Skia and its libraries); a later one reuses vcpkg's binary cache.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${1:-${SCRIPT_DIR}/skia/work}"
OUT="${SCRIPT_DIR}/skia/arm64-android-nanoq"
TRIPLET=arm64-android-nanoq

# Artist v1.0.2 (Elements v1.0.2's submodule) and the vcpkg it pins (its lib/external/vcpkg), as
# nanoq/ios/build-skia.sh.
ARTIST_COMMIT=d64fe8479ba2804cf20f578184bf5bd7550b0f8b
VCPKG_COMMIT=00d899c410b31467733472fc3a83a25729046b13

# The NDK the app builds with (android/app/build.gradle.kts, ndkVersion 27.3.13750724), so Skia
# and the app share one clang and one libc++.
NDK_RELEASE=r27d
NDK_REVISION=27.3.13750724

mkdir -p "${WORK}" "${OUT}"

fetch_at() {
    local url="$1" dir="$2" commit="$3"
    if [[ ! -d "${dir}/.git" ]]; then
        git init -q "${dir}"
        git -C "${dir}" remote add origin "${url}"
    fi
    if [[ "$(git -C "${dir}" rev-parse -q --verify HEAD 2>/dev/null)" != "${commit}" ]]; then
        git -C "${dir}" fetch -q --depth 1 origin "${commit}"
        git -C "${dir}" checkout -q -f --detach FETCH_HEAD
    fi
}

# ── The NDK ──────────────────────────────────────────────────────────────────────
NDK="${WORK}/android-ndk-${NDK_RELEASE}"
if [[ ! -f "${NDK}/source.properties" ]]; then
    echo "Fetching NDK ${NDK_RELEASE} ..."
    curl -fsSL -o "${WORK}/ndk.zip" "https://dl.google.com/android/repository/android-ndk-${NDK_RELEASE}-linux.zip"
    rm -rf "${NDK}"
    unzip -q "${WORK}/ndk.zip" -d "${WORK}"
    rm -f "${WORK}/ndk.zip"
fi
if ! grep -q "Pkg.Revision = ${NDK_REVISION}" "${NDK}/source.properties"; then
    echo "${NDK} is not NDK ${NDK_REVISION}:" >&2
    cat "${NDK}/source.properties" >&2
    exit 1
fi
export ANDROID_NDK_HOME="${NDK}"

# ── vcpkg and Artist's ports ─────────────────────────────────────────────────────
fetch_at https://github.com/cycfi/artist.git "${WORK}/artist" "${ARTIST_COMMIT}"
fetch_at https://github.com/microsoft/vcpkg.git "${WORK}/vcpkg" "${VCPKG_COMMIT}"

if [[ ! -x "${WORK}/vcpkg/vcpkg" ]]; then
    "${WORK}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# Ganesh on OpenGL ES, FreeType for the fonts the app carries (Artist loads them by file), harfbuzz
# for Artist's text, and the image codecs. No ICU (Artist does not use Skia's shaper), no
# fontconfig (Android has none: the app registers its own fonts).
"${WORK}/vcpkg/vcpkg" install \
    "skia[core,gl,freetype,harfbuzz,png,jpeg,webp]:${TRIPLET}" \
    --classic \
    --overlay-ports="${WORK}/artist/vcpkg/ports" \
    --overlay-triplets="${SCRIPT_DIR}/cmake/vcpkg-triplets" \
    --x-install-root="${WORK}/vcpkg/installed" \
    --disable-metrics

INSTALLED="${WORK}/vcpkg/installed/${TRIPLET}"

# Copied out of the work folder (a container volume, as a rule) to where the Gradle build finds it.
rm -rf "${OUT}"
mkdir -p "${OUT}"
# -L: links become files (a Windows bind mount takes no symlinks); no -a, whose owners it refuses.
cp -RL "${INSTALLED}/." "${OUT}/"

echo "Skia for Android: ${OUT}"
