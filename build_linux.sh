#!/usr/bin/env bash
# build_linux.sh — Build Soundshed Guitar for Linux and stage a distribution folder.
#
# Usage: ./build_linux.sh [options]
#   --skip-ts          Skip the TypeScript/UI build step
#   --skip-configure   Skip the CMake configure step
#   --skip-build       Skip CMake build step (only re-stage artifacts)
#   --zip              After staging, create a .zip archive of the distribution
#   --dist-dir <p>     Override the output staging directory (default: linux-dist)
#   --build-dir <p>    Override the CMake build directory (default: juce/builds-linux)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must be run on Linux." >&2
    exit 1
fi

SKIP_TS=false
SKIP_CONFIGURE=false
SKIP_BUILD=false
BUILD_ZIP=false
DIST_DIR="${SCRIPT_DIR}/linux-dist"
BUILD_DIR="${SCRIPT_DIR}/juce/builds-linux"
PRODUCT="Soundshed Guitar"
VERSION="$(cat "${SCRIPT_DIR}/juce/VERSION" 2>/dev/null || echo "1.0.0")"
UI_DIR="${SCRIPT_DIR}/core/ui"
JUCE_SUBMODULE_DIR="${SCRIPT_DIR}/juce/JUCE"
CLAP_EXTENSIONS_DIR="${SCRIPT_DIR}/juce/modules/clap-juce-extensions"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-ts)        SKIP_TS=true ;;
        --skip-configure) SKIP_CONFIGURE=true ;;
        --skip-build)     SKIP_BUILD=true ;;
        --zip)            BUILD_ZIP=true ;;
        --dist-dir)       DIST_DIR="$2"; shift ;;
        --build-dir)      BUILD_DIR="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

ARTEFACTS_RELEASE="${BUILD_DIR}/SoundshedGuitar_artefacts/Release"
ARTEFACTS_FALLBACK="${BUILD_DIR}/SoundshedGuitar_artefacts"
APP_DST="${DIST_DIR}/opt/Soundshed/${PRODUCT}"
VST3_DST="${DIST_DIR}/usr/lib/vst3"
CLAP_DST="${DIST_DIR}/usr/lib/clap"

choose_generator_args() {
    if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
        printf '%s\n' "-G" "$CMAKE_GENERATOR"
    elif command -v ninja >/dev/null 2>&1; then
        printf '%s\n' "-G" "Ninja"
    fi
}

copy_artifact() {
    local src="$1"
    local dst_dir="$2"

    if [[ -e "$src" ]]; then
        cp -R "$src" "$dst_dir/"
        echo "  ✓ $(basename "$src") → ${dst_dir#"$SCRIPT_DIR/"}"
    else
        echo "  ⚠ Not found, skipping: ${src#"$SCRIPT_DIR/"}"
    fi
}

find_first_match() {
    local search_dir="$1"
    local pattern="$2"

    find "$search_dir" -mindepth 1 -maxdepth 1 -name "$pattern" | sort | head -n 1
}

prune_ui_payload() {
    local ui_root="$1"

    if [[ ! -d "$ui_root" ]]; then
        return
    fi

    rm -rf \
        "$ui_root/node_modules" \
        "$ui_root/tests" \
        "$ui_root/Testing" \
        "$ui_root/ts"

    rm -f \
        "$ui_root/package.json" \
        "$ui_root/package-lock.json" \
        "$ui_root/tsconfig.json"
}

prune_staged_ui_payloads() {
    while IFS= read -r ui_root; do
        prune_ui_payload "$ui_root"
        echo "  ✓ Pruned non-runtime UI files → ${ui_root#"$SCRIPT_DIR/"}"
    done < <(find "$DIST_DIR" -type d -path "*/resources/ui" | sort)
}

check_linux_dependencies() {
    if ! command -v pkg-config >/dev/null 2>&1; then
        echo "" >&2
        echo "✗ pkg-config is required for Linux JUCE builds." >&2
        echo "  Install it first, then rerun this script." >&2
        exit 1
    fi

    local missing_modules=()
    local required_modules=(
        alsa
        freetype2
        fontconfig
        gl
        libcurl
        x11
        xext
        xinerama
        xrandr
        gtk+-x11-3.0
    )

    local module
    for module in "${required_modules[@]}"; do
        if ! pkg-config --exists "$module"; then
            missing_modules+=("$module")
        fi
    done

    if ! pkg-config --exists webkit2gtk-4.1 && ! pkg-config --exists webkit2gtk-4.0; then
        missing_modules+=("webkit2gtk-4.1/webkit2gtk-4.0")
    fi

    if (( ${#missing_modules[@]} > 0 )); then
        echo "" >&2
        echo "✗ Missing Linux build dependencies: ${missing_modules[*]}" >&2
        echo "  Install the JUCE Linux packages, for example on Ubuntu:" >&2
        echo "  sudo apt-get update && sudo apt-get install libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libfreetype6-dev libfontconfig1-dev libcurl4-openssl-dev libgtk-3-dev libwebkit2gtk-4.1-dev libgl1-mesa-dev libglu1-mesa-dev ninja-build" >&2
        exit 1
    fi
}

echo "═══════════════════════════════════════════════════"
echo "  Soundshed Guitar — Linux Distribution Build"
echo "  Build dir: ${BUILD_DIR}"
echo "  Dist dir: ${DIST_DIR}"
[[ "$BUILD_ZIP" == true ]] && echo "  Zip archive: yes"
echo "═══════════════════════════════════════════════════"

if [[ ! -d "$JUCE_SUBMODULE_DIR" ]]; then
    echo "" >&2
    echo "✗ Missing JUCE submodule at ${JUCE_SUBMODULE_DIR#"$SCRIPT_DIR/"}" >&2
    echo "  Run: git -C juce submodule update --init --recursive" >&2
    echo "  If JUCE is still missing, clone it into juce/JUCE from the JUCE develop branch." >&2
    exit 1
fi

if [[ ! -d "$CLAP_EXTENSIONS_DIR" ]]; then
    echo "" >&2
    echo "✗ Missing clap-juce-extensions submodule at ${CLAP_EXTENSIONS_DIR#"$SCRIPT_DIR/"}" >&2
    echo "  Run: git -C juce submodule update --init --recursive" >&2
    exit 1
fi

check_linux_dependencies

if [[ "$SKIP_TS" == false ]]; then
    echo ""
    echo "▶ Building UI (TypeScript)…"
    (cd "$UI_DIR" && npm run build)
    echo "  ✓ UI bundle built"
fi

if [[ "$SKIP_CONFIGURE" == false ]]; then
    echo ""
    echo "▶ Configuring CMake…"
    mapfile -t generator_args < <(choose_generator_args)
    cmake -Wno-dev -S "${SCRIPT_DIR}/juce" -B "$BUILD_DIR" "${generator_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SUPPRESS_DEVELOPER_WARNINGS=ON
    echo "  ✓ Configure complete"
fi

if [[ "$SKIP_BUILD" == false ]]; then
    echo ""
    echo "▶ Building Standalone…"
    cmake --build "$BUILD_DIR" --target SoundshedGuitar_Standalone --parallel

    echo ""
    echo "▶ Building VST3…"
    cmake --build "$BUILD_DIR" --target SoundshedGuitar_VST3 --parallel

    echo ""
    echo "▶ Building CLAP…"
    cmake --build "$BUILD_DIR" --target SoundshedGuitar_CLAP --parallel

    echo ""
    echo "  ✓ All Linux targets built"
fi

echo ""
echo "▶ Staging distribution layout…"

ARTEFACTS_DIR="$ARTEFACTS_RELEASE"
if [[ ! -d "$ARTEFACTS_DIR" ]]; then
    ARTEFACTS_DIR="$ARTEFACTS_FALLBACK"
fi

STANDALONE_SRC_DIR="${ARTEFACTS_DIR}/Standalone"
VST3_SRC="$(find_first_match "${ARTEFACTS_DIR}/VST3" "*.vst3")"
CLAP_SRC="$(find_first_match "${ARTEFACTS_DIR}/CLAP" "*.clap")"

if [[ ! -d "$STANDALONE_SRC_DIR" ]]; then
    echo "  ✗ Standalone artifacts not found at ${STANDALONE_SRC_DIR#"$SCRIPT_DIR/"}" >&2
    echo "    Run without --skip-build first, or override --build-dir." >&2
    exit 1
fi

rm -rf "$DIST_DIR"
mkdir -p "$APP_DST" "$VST3_DST" "$CLAP_DST"

cp -R "${STANDALONE_SRC_DIR}/." "$APP_DST/"
echo "  ✓ Standalone payload → ${APP_DST#"$SCRIPT_DIR/"}"

copy_artifact "$VST3_SRC" "$VST3_DST"
copy_artifact "$CLAP_SRC" "$CLAP_DST"

prune_staged_ui_payloads

echo ""
echo "═══════════════════════════════════════════════════"
echo "  Distribution layout:"
find "$DIST_DIR" -mindepth 2 -maxdepth 8 \( -name "*.vst3" -o -name "*.clap" -o -type f -name "$PRODUCT" \) \
    | sed "s|${SCRIPT_DIR}/||" | sort | sed 's/^/    /'
echo "═══════════════════════════════════════════════════"

if [[ "$BUILD_ZIP" == true ]]; then
    ZIP_BASENAME="SoundshedGuitar-${VERSION}-Linux"
    ZIP_PATH="${SCRIPT_DIR}/${ZIP_BASENAME}.zip"

    echo ""
    echo "▶ Creating zip archive…"
    rm -f "$ZIP_PATH"

    if command -v 7z >/dev/null 2>&1; then
        (cd "$DIST_DIR" && 7z a -tzip "$ZIP_PATH" .)
    elif command -v zip >/dev/null 2>&1; then
        (cd "$DIST_DIR" && zip -r "$ZIP_PATH" .)
    else
        echo "  ✗ Neither 7z nor zip is installed; cannot create archive." >&2
        exit 1
    fi

    echo "  ✓ Archive created: ${ZIP_PATH#"$SCRIPT_DIR/"}"
fi