#!/usr/bin/env bash
# Builds the Skia that Nano Q's iOS app draws with, on the GPU through Metal. Artist publishes
# prebuilt Skia for macOS, Windows and Linux only, so iOS builds its own, with Artist's own
# vcpkg port (Skia m148) and vcpkg at the commit Artist pins. See docs/plans/nano-q-ios.md.
#
# Usage: nanoq/ios/build-skia.sh
#
# Everything goes under nanoq/ios/skia (ignored by git): vcpkg, Artist's ports, and the
# installed tree, nanoq/ios/skia/vcpkg/installed/arm64-ios-nanoq, which nanoq/ios/build.sh
# hands to the configure. The first run takes a while (vcpkg builds gn, Skia and its
# libraries); a later one reuses vcpkg's binary cache.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${SCRIPT_DIR}/skia"
TRIPLET=arm64-ios-nanoq

# Artist v1.0.2 (Elements v1.0.2's submodule) and the vcpkg it pins (its lib/external/vcpkg).
ARTIST_COMMIT=d64fe8479ba2804cf20f578184bf5bd7550b0f8b
VCPKG_COMMIT=00d899c410b31467733472fc3a83a25729046b13

mkdir -p "${WORK}"

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

fetch_at https://github.com/cycfi/artist.git "${WORK}/artist" "${ARTIST_COMMIT}"
fetch_at https://github.com/microsoft/vcpkg.git "${WORK}/vcpkg" "${VCPKG_COMMIT}"

# The port hands vcpkg's flags to Skia's C and C++ only, so its Objective-C++ (all of Ganesh's
# Metal code) was built for the SDK's iOS, not 12.0; the compiler then takes its @available
# checks as passed, and iOS 12 meets iOS 13 calls. gn's ios_min_target sets every source's.
PORTFILE="${WORK}/artist/vcpkg/ports/skia/portfile.cmake"
if ! grep -q "ios_min_target" "${PORTFILE}"; then
    perl -0pi -e 's/(string\(APPEND OPTIONS " target_os=\\"ios\\""\))/$1\n    string(APPEND OPTIONS " ios_min_target=\\"\${VCPKG_OSX_DEPLOYMENT_TARGET}\\"")/' "${PORTFILE}"
    if ! grep -q "ios_min_target" "${PORTFILE}"; then
        echo "Could not add ios_min_target to ${PORTFILE}: has the port changed?" >&2
        exit 1
    fi
fi

if [[ ! -x "${WORK}/vcpkg/vcpkg" ]]; then
    "${WORK}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# Ganesh on Metal, harfbuzz for Artist's text, and the image codecs. No OpenGL (deprecated on
# iOS, and Metal is what the host draws with) and no ICU (Artist does not use Skia's shaper).
"${WORK}/vcpkg/vcpkg" install \
    "skia[core,metal,harfbuzz,png,jpeg,webp]:${TRIPLET}" \
    --classic \
    --overlay-ports="${WORK}/artist/vcpkg/ports" \
    --overlay-triplets="${SCRIPT_DIR}/cmake/vcpkg-triplets" \
    --x-install-root="${WORK}/vcpkg/installed" \
    --disable-metrics

INSTALLED="${WORK}/vcpkg/installed/${TRIPLET}"

# Every object in Skia's arm64 slice is to be for iOS 12.0. vcpkg in classic mode keeps a package
# it has installed even when its port changes, so an install from before the ios_min_target fix
# stays: `vcpkg remove skia:${TRIPLET}` (with the same overlays) and run this again.
THIN="$(mktemp -d)"
trap 'rm -rf "${THIN}"' EXIT
lipo "${INSTALLED}/lib/libskia.a" -thin arm64 -output "${THIN}/libskia.a" 2>/dev/null \
    || cp "${INSTALLED}/lib/libskia.a" "${THIN}/libskia.a"
NEWER="$(otool -l "${THIN}/libskia.a" | awk '/minos/ && $2 != "12.0"' | wc -l | tr -d ' ')"
if [[ "${NEWER}" != 0 ]]; then
    echo "${NEWER} objects in libskia.a are not built for iOS 12.0: remove skia:${TRIPLET} and rebuild" >&2
    exit 1
fi

echo "Skia for iOS: ${INSTALLED}"
