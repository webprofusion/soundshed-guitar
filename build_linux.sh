#!/usr/bin/env bash
# build_linux.sh — Build Soundshed Guitar for Linux and stage a distribution folder.
#
# Usage: ./build_linux.sh [options]
#   --arch <arch>      Target architecture: native, x64, arm64, or all (default: native)
#   --skip-ts          Skip the TypeScript/UI build step
#   --skip-configure   Skip the CMake configure step
#   --skip-build       Skip CMake build step (only re-stage artifacts)
#   --lv2              Also build and stage the LV2 plugin bundle
#   --zip              After staging, create a .zip archive of the distribution
#   --dist-dir <p>     Override the output staging directory (default: linux-dist-<arch>)
#   --build-dir <p>    Override the CMake build directory (default: juce/builds-linux-<arch>)
#   --toolchain-file <p>
#                     Override the CMake toolchain file for cross-compiles
#   --help             Show this help text

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
BUILD_LV2=true
BUILD_ZIP=true
ARCH_REQUEST="native"
DIST_DIR_OVERRIDE=""
BUILD_DIR_OVERRIDE=""
TOOLCHAIN_FILE_OVERRIDE=""
PRODUCT="Soundshed Guitar"
LINUX_STANDALONE_DIR_NAME="soundshed-guitar"
LINUX_STANDALONE_EXECUTABLE_NAME="soundshed-guitar"
VERSION="$(cat "${SCRIPT_DIR}/juce/VERSION" 2>/dev/null || echo "1.0.0")"
UI_DIR="${SCRIPT_DIR}/core/ui"
JUCE_SUBMODULE_DIR="${SCRIPT_DIR}/juce/JUCE"
CLAP_EXTENSIONS_DIR="${SCRIPT_DIR}/juce/modules/clap-juce-extensions"
UI_ALREADY_BUILT=false

print_usage() {
        cat <<'EOF'
Usage: ./build_linux.sh [options]
    --arch <arch>      Target architecture: native, x64, arm64, or all (default: native)
    --skip-ts          Skip the TypeScript/UI build step
    --skip-configure   Skip the CMake configure step
    --skip-build       Skip CMake build step (only re-stage artifacts)
    --lv2              Also build and stage the LV2 plugin bundle
    --zip              After staging, create a .zip archive of the distribution
    --dist-dir <p>     Override the output staging directory (default: linux-dist-<arch>)
    --build-dir <p>    Override the CMake build directory (default: juce/builds-linux-<arch>)
    --toolchain-file <p>
                                        Override the CMake toolchain file for cross-compiles
    --help             Show this help text
EOF
}

normalize_arch() {
    local value="${1:-}"

    value="$(printf '%s' "$value" | tr '[:upper:]' '[:lower:]')"

    case "$value" in
        native|host)
            printf '%s\n' "native"
            ;;
        all|both)
            printf '%s\n' "all"
            ;;
        x64|x86_64|amd64)
            printf '%s\n' "x64"
            ;;
        arm64|aarch64)
            printf '%s\n' "arm64"
            ;;
        *)
            echo "Unsupported architecture: $1" >&2
            exit 1
            ;;
    esac
}

detect_host_arch() {
    normalize_arch "$(uname -m)"
}

default_build_dir_for_arch() {
    printf '%s\n' "${SCRIPT_DIR}/juce/builds-linux-$1"
}

default_dist_dir_for_arch() {
    printf '%s\n' "${SCRIPT_DIR}/linux-dist-$1"
}

default_toolchain_file_for_arch() {
    printf '%s\n' "${SCRIPT_DIR}/cmake/toolchains/linux-$1.cmake"
}

cross_compiler_prefix_for_arch() {
    case "$1" in
        x64) printf '%s\n' "x86_64-linux-gnu" ;;
        arm64) printf '%s\n' "aarch64-linux-gnu" ;;
        *) return 1 ;;
    esac
}

resolve_arch_dir() {
    local override_path="$1"
    local default_path="$2"
    local target_arch="$3"
    local requested_arch="$4"

    if [[ -z "$override_path" ]]; then
        printf '%s\n' "$default_path"
        return
    fi

    if [[ "$requested_arch" == "all" ]]; then
        printf '%s\n' "${override_path}-${target_arch}"
        return
    fi

    printf '%s\n' "$override_path"
}

resolve_toolchain_file() {
    local target_arch="$1"
    local host_arch="$2"

    if [[ -n "$TOOLCHAIN_FILE_OVERRIDE" ]]; then
        printf '%s\n' "$TOOLCHAIN_FILE_OVERRIDE"
        return
    fi

    if [[ "$target_arch" == "$host_arch" ]]; then
        return
    fi

    local default_toolchain
    default_toolchain="$(default_toolchain_file_for_arch "$target_arch")"
    if [[ -f "$default_toolchain" ]]; then
        printf '%s\n' "$default_toolchain"
    fi
}

ensure_cross_toolchain_ready() {
    local target_arch="$1"
    local toolchain_file="$2"

    if [[ -z "$toolchain_file" ]]; then
        echo "" >&2
        echo "✗ Cross-compiling for ${target_arch} requires a toolchain file." >&2
        echo "  Pass --toolchain-file, or use the repo defaults under cmake/toolchains/." >&2
        exit 1
    fi

    if [[ ! -f "$toolchain_file" ]]; then
        echo "" >&2
        echo "✗ Toolchain file not found: ${toolchain_file}" >&2
        exit 1
    fi

    local compiler_prefix
    compiler_prefix="$(cross_compiler_prefix_for_arch "$target_arch")"

    if ! command -v "${compiler_prefix}-gcc" >/dev/null 2>&1; then
        echo "" >&2
        echo "✗ Missing cross compiler: ${compiler_prefix}-gcc" >&2
        echo "  Install the ${target_arch} cross toolchain, then rerun this script." >&2
        exit 1
    fi

    if ! command -v "${compiler_prefix}-g++" >/dev/null 2>&1; then
        echo "" >&2
        echo "✗ Missing cross compiler: ${compiler_prefix}-g++" >&2
        echo "  Install the ${target_arch} cross toolchain, then rerun this script." >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)           ARCH_REQUEST="$(normalize_arch "$2")"; shift ;;
        --skip-ts)        SKIP_TS=true ;;
        --skip-configure) SKIP_CONFIGURE=true ;;
        --skip-build)     SKIP_BUILD=true ;;
        --lv2)            BUILD_LV2=true ;;
        --zip)            BUILD_ZIP=true ;;
        --dist-dir)       DIST_DIR_OVERRIDE="$2"; shift ;;
        --build-dir)      BUILD_DIR_OVERRIDE="$2"; shift ;;
        --toolchain-file) TOOLCHAIN_FILE_OVERRIDE="$2"; shift ;;
        --help|-h)        print_usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

HOST_ARCH="$(detect_host_arch)"

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

    if [[ ! -d "$search_dir" ]]; then
        return 0
    fi

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

rename_staged_linux_executable() {
    local app_dir="$1"
    local source_executable="$2"
    local destination_executable="$3"

    if [[ "$source_executable" == "$destination_executable" ]]; then
        return
    fi

    local source_path="${app_dir}/${source_executable}"
    local destination_path="${app_dir}/${destination_executable}"

    if [[ ! -f "$source_path" ]]; then
        echo "  ✗ Expected staged executable at ${source_path#"$SCRIPT_DIR/"}" >&2
        exit 1
    fi

    mv "$source_path" "$destination_path"
    echo "  ✓ Renamed standalone executable → ${destination_path#"$SCRIPT_DIR/"}"
}

check_linux_dependencies() {
    local target_arch="$1"
    local host_arch="$2"

    if ! command -v pkg-config >/dev/null 2>&1; then
        echo "" >&2
        echo "✗ pkg-config is required for Linux JUCE builds." >&2
        echo "  Install it first, then rerun this script." >&2
        exit 1
    fi

    if [[ "$target_arch" != "$host_arch" ]]; then
        echo "  • Cross-compile dependency validation is delegated to the selected toolchain/sysroot."
        return
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

build_for_arch() {
    local target_arch="$1"
    local build_dir="$2"
    local dist_dir="$3"
    local toolchain_file="$4"
    local artefacts_release="${build_dir}/SoundshedGuitar_artefacts/Release"
    local artefacts_fallback="${build_dir}/SoundshedGuitar_artefacts"
    local app_dst="${dist_dir}/opt/Soundshed/${LINUX_STANDALONE_DIR_NAME}"
    local vst3_dst="${dist_dir}/usr/lib/vst3"
    local clap_dst="${dist_dir}/usr/lib/clap"
    local lv2_dst="${dist_dir}/usr/lib/lv2"

    echo "═══════════════════════════════════════════════════"
    echo "  Soundshed Guitar — Linux Distribution Build"
    echo "  Host arch: ${HOST_ARCH}"
    echo "  Target arch: ${target_arch}"
    echo "  Build dir: ${build_dir}"
    echo "  Dist dir: ${dist_dir}"
    echo "  LV2 plugin: ${BUILD_LV2}"
    if [[ -n "$toolchain_file" ]]; then
        echo "  Toolchain: ${toolchain_file#"$SCRIPT_DIR/"}"
    else
        echo "  Toolchain: native"
    fi
    [[ "$BUILD_ZIP" == true ]] && echo "  Zip archive: yes"
    echo "═══════════════════════════════════════════════════"

    check_linux_dependencies "$target_arch" "$HOST_ARCH"

    if [[ "$SKIP_TS" == false && "$UI_ALREADY_BUILT" == false ]]; then
        echo ""
        echo "▶ Building UI (TypeScript)…"
        (cd "$UI_DIR" && npm run build)
        echo "  ✓ UI bundle built"
        UI_ALREADY_BUILT=true
    fi

    if [[ "$SKIP_CONFIGURE" == false ]]; then
        echo ""
        echo "▶ Configuring CMake for ${target_arch}…"
        mapfile -t generator_args < <(choose_generator_args)
        configure_args=(
            -Wno-dev
            -S "${SCRIPT_DIR}/juce"
            -B "$build_dir"
            "${generator_args[@]}"
            -DCMAKE_BUILD_TYPE=Release
            -DGUITARFX_ENABLE_LV2=$([[ "$BUILD_LV2" == true ]] && printf 'ON' || printf 'OFF')
            -DCMAKE_SUPPRESS_DEVELOPER_WARNINGS=ON
        )

        if [[ -n "$toolchain_file" ]]; then
            configure_args+=( -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" )
        fi

        cmake "${configure_args[@]}"
        echo "  ✓ Configure complete"
    fi

    if [[ "$SKIP_BUILD" == false ]]; then
        echo ""
        echo "▶ Building Standalone (${target_arch})…"
        cmake --build "$build_dir" --target SoundshedGuitar_Standalone --parallel

        echo ""
        echo "▶ Building VST3 (${target_arch})…"
        cmake --build "$build_dir" --target SoundshedGuitar_VST3 --parallel

        echo ""
        echo "▶ Building CLAP (${target_arch})…"
        cmake --build "$build_dir" --target SoundshedGuitar_CLAP --parallel

        if [[ "$BUILD_LV2" == true ]]; then
            echo ""
            echo "▶ Building LV2 (${target_arch})…"
            cmake --build "$build_dir" --target SoundshedGuitar_LV2 --parallel
        fi

        echo ""
        echo "  ✓ All requested Linux targets built for ${target_arch}"
    fi

    echo ""
    echo "▶ Staging distribution layout for ${target_arch}…"

    local artefacts_dir="$artefacts_release"
    if [[ ! -d "$artefacts_dir" ]]; then
        artefacts_dir="$artefacts_fallback"
    fi

    local standalone_src_dir="${artefacts_dir}/Standalone"
    local vst3_src
    local clap_src
    local lv2_src

    vst3_src="$(find_first_match "${artefacts_dir}/VST3" "*.vst3")"
    clap_src="$(find_first_match "${artefacts_dir}/CLAP" "*.clap")"
    lv2_src="$(find_first_match "${artefacts_dir}/LV2" "*.lv2")"

    if [[ ! -d "$standalone_src_dir" ]]; then
        echo "  ✗ Standalone artifacts not found at ${standalone_src_dir#"$SCRIPT_DIR/"}" >&2
        echo "    Run without --skip-build first, or override --build-dir." >&2
        exit 1
    fi

    rm -rf "$dist_dir"
    mkdir -p "$app_dst" "$vst3_dst" "$clap_dst"

    if [[ "$BUILD_LV2" == true ]]; then
        mkdir -p "$lv2_dst"
    fi

    cp -R "${standalone_src_dir}/." "$app_dst/"
    echo "  ✓ Standalone payload → ${app_dst#"$SCRIPT_DIR/"}"
    rename_staged_linux_executable "$app_dst" "$PRODUCT" "$LINUX_STANDALONE_EXECUTABLE_NAME"

    copy_artifact "$vst3_src" "$vst3_dst"
    copy_artifact "$clap_src" "$clap_dst"

    if [[ "$BUILD_LV2" == true ]]; then
        copy_artifact "$lv2_src" "$lv2_dst"
    fi

    DIST_DIR="$dist_dir"
    prune_staged_ui_payloads

    echo ""
    echo "═══════════════════════════════════════════════════"
    echo "  Distribution layout (${target_arch}):"
    find "$dist_dir" -mindepth 2 -maxdepth 8 \( -name "*.vst3" -o -name "*.clap" -o -name "*.lv2" -o -type f -name "$LINUX_STANDALONE_EXECUTABLE_NAME" \) \
        | sed "s|${SCRIPT_DIR}/||" | sort | sed 's/^/    /'
    echo "═══════════════════════════════════════════════════"

    if [[ "$BUILD_ZIP" == true ]]; then
        local zip_basename="SoundshedGuitar-${VERSION}-Linux-${target_arch}"
        local zip_path="${SCRIPT_DIR}/${zip_basename}.zip"

        echo ""
        echo "▶ Creating zip archive for ${target_arch}…"
        rm -f "$zip_path"

        if command -v 7z >/dev/null 2>&1; then
            (cd "$dist_dir" && 7z a -tzip "$zip_path" .)
        elif command -v zip >/dev/null 2>&1; then
            (cd "$dist_dir" && zip -r "$zip_path" .)
        else
            echo "  ✗ Neither 7z nor zip is installed; cannot create archive." >&2
            exit 1
        fi

        echo "  ✓ Archive created: ${zip_path#"$SCRIPT_DIR/"}"
    fi
}

build_requested_arch() {
    local requested_arch="$1"
    local target_arch="$requested_arch"
    local build_dir
    local dist_dir
    local toolchain_file

    if [[ "$requested_arch" == "native" ]]; then
        target_arch="$HOST_ARCH"
    fi

    build_dir="$(resolve_arch_dir "$BUILD_DIR_OVERRIDE" "$(default_build_dir_for_arch "$target_arch")" "$target_arch" "$ARCH_REQUEST")"
    dist_dir="$(resolve_arch_dir "$DIST_DIR_OVERRIDE" "$(default_dist_dir_for_arch "$target_arch")" "$target_arch" "$ARCH_REQUEST")"
    toolchain_file="$(resolve_toolchain_file "$target_arch" "$HOST_ARCH")"

    if [[ "$target_arch" != "$HOST_ARCH" ]]; then
        ensure_cross_toolchain_ready "$target_arch" "$toolchain_file"
    fi

    build_for_arch "$target_arch" "$build_dir" "$dist_dir" "$toolchain_file"
}

if [[ "$ARCH_REQUEST" == "all" && -n "$TOOLCHAIN_FILE_OVERRIDE" ]]; then
    echo "--toolchain-file can only be used with a single target architecture." >&2
    exit 1
fi

if [[ "$ARCH_REQUEST" == "all" ]]; then
    build_requested_arch x64
    build_requested_arch arm64
else
    build_requested_arch "$ARCH_REQUEST"
fi