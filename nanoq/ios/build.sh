#!/usr/bin/env bash
# nanoq/ios/build.sh — build Soundshed Nano Q for iOS and iPadOS, and optionally install and
# launch it in the Simulator. See docs/plans/nano-q-ios.md; signing, devices and the iOS 12
# legacy build work as for the JUCE apps (docs/ios-build.md).
#
# Usage: nanoq/ios/build.sh [sim|device] [debug|release] [run] [reconfigure] [legacy] [skia|quartz]
#
#   sim | device     The Simulator (default) or an iPhone/iPad. A device build needs a
#                    development team: SSG_IOS_DEVELOPMENT_TEAM=<team id>.
#   debug | release  Build configuration (default debug). Judge nothing about audio from a
#                    debug build: the DSP is unoptimised.
#   run              After building for the Simulator, boot one if none is running
#                    (SSG_IOS_SIMULATOR is its UDID; an iPad by default), install the app and
#                    launch it. SOUNDSHED_NANOQ_PROFILE, if set, is passed to the app as its
#                    data root.
#   reconfigure      Re-run CMake's configure step before building.
#   legacy           Build for iOS 12 (the iPad mini 2, iPad Air 1, iPhone 5s/6) in a tree of
#                    its own, nanoq/ios/build-legacy, with std::filesystem built into the app.
#   skia | quartz    Draw on the GPU with Skia on Metal, or on the CPU with Quartz. Taken at
#                    configure (pass reconfigure to switch a tree). Skia needs
#                    nanoq/ios/build-skia.sh run first, and is arm64 only, so a Simulator build
#                    on an Intel Mac draws with Quartz. Default: Skia for a device when that
#                    Skia is built, Quartz otherwise.
#
# Arguments may appear in any order. The build tree is nanoq/ios/build (Xcode generator); open
# nanoq/ios/build/SoundshedNanoQ.xcodeproj to work in Xcode (scheme nanoq_ios).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NANOQ_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

PLATFORM=sim
CONFIG=Debug
RUN=false
RECONFIGURE=false
LEGACY=false
RENDERER=
SKIA_DIR="${SCRIPT_DIR}/skia/vcpkg/installed/arm64-ios-nanoq"

TARGET=nanoq_ios
APP_NAME="Soundshed Nano Q"
BUNDLE_ID=com.soundshed.guitar.nanoq

for arg in "$@"; do
    case "$(printf '%s' "$arg" | tr '[:upper:]' '[:lower:]')" in
        sim|simulator) PLATFORM=sim ;;
        device|ios) PLATFORM=device ;;
        debug) CONFIG=Debug ;;
        release) CONFIG=Release ;;
        run) RUN=true ;;
        reconfigure) RECONFIGURE=true ;;
        legacy) LEGACY=true ;;
        skia) RENDERER=skia ;;
        quartz) RENDERER=quartz ;;
        -h|--help)
            sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg (try --help)" >&2
            exit 2
            ;;
    esac
done

if [[ "$LEGACY" == true ]]; then
    BUILD_DIR="${SCRIPT_DIR}/build-legacy"
fi

if [[ "$RUN" == true && "$PLATFORM" != sim ]]; then
    echo "'run' installs into the Simulator only. For a device, install from Xcode, with" >&2
    echo "  xcrun devicectl device install app --device <id> <path to .app>" >&2
    echo "or, for an iOS 12 device, ios-deploy (docs/ios-build.md)." >&2
    exit 2
fi

# ── Configure ────────────────────────────────────────────────────────────────
if [[ "$RECONFIGURE" == true || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    configure_args=(
        -S "${NANOQ_DIR}" -B "${BUILD_DIR}" -G Xcode
        -DCMAKE_SYSTEM_NAME=iOS
    )
    if [[ "$LEGACY" == true ]]; then
        configure_args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
    fi
    if [[ -n "${SSG_IOS_DEVELOPMENT_TEAM:-}" ]]; then
        configure_args+=(-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${SSG_IOS_DEVELOPMENT_TEAM}")
    fi
    if [[ -z "$RENDERER" ]]; then
        if [[ "$PLATFORM" == device && -f "${SKIA_DIR}/share/unofficial-skia/unofficial-skia-config.cmake" ]]; then
            RENDERER=skia
        else
            RENDERER=quartz
        fi
    fi
    if [[ "$RENDERER" == skia ]]; then
        if [[ ! -f "${SKIA_DIR}/share/unofficial-skia/unofficial-skia-config.cmake" ]]; then
            echo "No Skia for iOS at ${SKIA_DIR}: run nanoq/ios/build-skia.sh first" >&2
            exit 2
        fi
        configure_args+=(-DNANOQ_IOS_SKIA="${SKIA_DIR}")
    else
        configure_args+=(-DNANOQ_IOS_SKIA=)
    fi
    echo "Drawing with: ${RENDERER}"
    cmake "${configure_args[@]}"
fi

# ── Build ────────────────────────────────────────────────────────────────────
if [[ "$PLATFORM" == sim ]]; then
    SDK=iphonesimulator
    # The Simulator runs the host's architecture: x86_64 on an Intel Mac, arm64 otherwise.
    ARCH="$(uname -m)"
    xcode_args=(-sdk "$SDK" ARCHS="$ARCH" ONLY_ACTIVE_ARCH=NO CODE_SIGN_IDENTITY=-)
else
    SDK=iphoneos
    if [[ -z "${SSG_IOS_DEVELOPMENT_TEAM:-}" ]] && ! grep -q "CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM" "${BUILD_DIR}/CMakeCache.txt"; then
        echo "A device build needs a development team: SSG_IOS_DEVELOPMENT_TEAM=<team id> nanoq/ios/build.sh device reconfigure" >&2
        exit 2
    fi
    xcode_args=(-sdk "$SDK" ARCHS=arm64 -allowProvisioningUpdates -allowProvisioningDeviceRegistration)
    if [[ -n "${SSG_IOS_DEVELOPMENT_TEAM:-}" ]]; then
        xcode_args+=(DEVELOPMENT_TEAM="${SSG_IOS_DEVELOPMENT_TEAM}")
    fi
fi

cmake --build "${BUILD_DIR}" --config "$CONFIG" --target "$TARGET" -- "${xcode_args[@]}" -quiet

# The Xcode generator puts an iOS app under <config>-<sdk>.
APP_PATH="${BUILD_DIR}/${CONFIG}-${SDK}/${APP_NAME}.app"
if [[ ! -d "$APP_PATH" ]]; then
    echo "Build finished but the .app was not found at ${APP_PATH}" >&2
    exit 1
fi
echo "Built: ${APP_PATH}"

[[ "$RUN" == true ]] || exit 0

# ── Install and launch in the Simulator ──────────────────────────────────────
booted="$(xcrun simctl list devices booted | grep -Eo '\(([0-9A-F-]{36})\)' | head -n 1 | tr -d '()')" || true

if [[ -z "$booted" ]]; then
    wanted="${SSG_IOS_SIMULATOR:-}"
    if [[ -z "$wanted" ]]; then
        # The first iPad the installed runtime lists, which is its largest current model.
        wanted="$(xcrun simctl list devices available | grep -E '^\s+iPad' | head -n 1 | grep -Eo '\(([0-9A-F-]{36})\)' | tr -d '()')" || true
    fi
    if [[ -z "$wanted" ]]; then
        echo "No Simulator to boot. Install a runtime with: xcodebuild -downloadPlatform iOS" >&2
        exit 1
    fi
    echo "Booting Simulator ${wanted}"
    xcrun simctl boot "$wanted"
    booted="$(xcrun simctl list devices booted | grep -Eo '\(([0-9A-F-]{36})\)' | head -n 1 | tr -d '()')"
fi

open -a Simulator
xcrun simctl install "$booted" "$APP_PATH"
# simctl hands the app any variable prefixed SIMCTL_CHILD_.
if [[ -n "${SOUNDSHED_NANOQ_PROFILE:-}" ]]; then
    export SIMCTL_CHILD_SOUNDSHED_NANOQ_PROFILE="${SOUNDSHED_NANOQ_PROFILE}"
fi
xcrun simctl launch "$booted" "$BUNDLE_ID"
