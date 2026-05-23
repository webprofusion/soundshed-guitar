#!/usr/bin/env bash
# build_macos.sh — Build Soundshed Guitar for macOS and stage a distribution folder.
#
# Usage: ./build_macos.sh [options]
#   --skip-ts       Skip the TypeScript/UI build step
#   --skip-build    Skip CMake build step (only re-stage artifacts)
#   --universal     Build universal binary (arm64 + x86_64) (default)
#   --native        Build only for the host architecture
#   --pkg           After staging, assemble a distributable macOS installer .pkg (default)
#   --no-pkg        Skip macOS installer .pkg assembly
#   --dist-dir <p>  Override the output staging directory (default: macos-dist)

set -euo pipefail

# ── Resolve workspace root (directory containing this script) ─────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Defaults ──────────────────────────────────────────────────────────────────
SKIP_TS=false
SKIP_BUILD=false
UNIVERSAL=true
BUILD_PKG=true
DIST_DIR="${SCRIPT_DIR}/macos-dist"

normalize_arch() {
    case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
        arm64|aarch64)
            printf '%s\n' "arm64"
            ;;
        x64|x86_64|amd64)
            printf '%s\n' "x86_64"
            ;;
        *)
            printf '%s\n' "$1"
            ;;
    esac
}

choose_generator_args() {
    if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
        printf '%s\n' "-G" "$CMAKE_GENERATOR"
    elif command -v ninja >/dev/null 2>&1; then
        printf '%s\n' "-G" "Ninja"
    fi
}

HOST_ARCH="$(normalize_arch "$(uname -m)")"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-ts)    SKIP_TS=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --universal)  UNIVERSAL=true ;;
        --native)     UNIVERSAL=false ;;
        --pkg)        BUILD_PKG=true ;;
        --no-pkg)     BUILD_PKG=false ;;
        --dist-dir)   DIST_DIR="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

# ── Paths ─────────────────────────────────────────────────────────────────────
JUCE_BUILDS="${SCRIPT_DIR}/juce/builds"
ARTEFACTS="${JUCE_BUILDS}/SoundshedGuitar_artefacts/Release"
UI_DIR="${SCRIPT_DIR}/core/ui"

# macOS system destinations (relative inside DIST_DIR)
APP_DST="${DIST_DIR}/Applications"
VST3_DST="${DIST_DIR}/Library/Audio/Plug-Ins/VST3"
AU_DST="${DIST_DIR}/Library/Audio/Plug-Ins/Components"
CLAP_DST="${DIST_DIR}/Library/Audio/Plug-Ins/CLAP"
AAX_DST="${DIST_DIR}/Library/Application Support/Avid/Audio/Plug-Ins"

PRODUCT="Soundshed Guitar"

echo "═══════════════════════════════════════════════════"
echo "  Soundshed Guitar — macOS Distribution Build"
echo "  Dist dir: ${DIST_DIR}"
[[ "$UNIVERSAL" == true ]] && echo "  Architecture: universal (arm64 + x86_64)" || echo "  Architecture: native host (${HOST_ARCH})"
[[ "$BUILD_PKG"  == true ]] && echo "  Installer .pkg: yes"
echo "═══════════════════════════════════════════════════"

# ── Step 1: TypeScript / UI bundle ────────────────────────────────────────────
if [[ "$SKIP_TS" == false ]]; then
    echo ""
    echo "▶ Building UI (TypeScript)…"
    (cd "$UI_DIR" && npm run build)
    echo "  ✓ UI bundle built"
fi

# ── Step 2: CMake build ───────────────────────────────────────────────────────
if [[ "$SKIP_BUILD" == false ]]; then
    if [[ "$UNIVERSAL" == true ]]; then
        TARGET_ARCHITECTURES="arm64;x86_64"
        echo ""
        echo "▶ Configuring for universal binary (arm64 + x86_64)…"
    else
        TARGET_ARCHITECTURES="${HOST_ARCH}"
        echo ""
        echo "▶ Configuring for native host build (${HOST_ARCH})…"
    fi

    generator_args=()
    while IFS= read -r generator_arg; do
        generator_args+=("$generator_arg")
    done < <(choose_generator_args)

    cmake -B "$JUCE_BUILDS" -S "${SCRIPT_DIR}/juce" \
        "${generator_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="${TARGET_ARCHITECTURES}"
    echo "  ✓ Configured"

    if [[ "$UNIVERSAL" == true ]]; then
        echo ""
        echo "  Architecture verification will run after staging."
    fi

    echo ""
    echo "▶ Building Standalone…"
    cmake --build "$JUCE_BUILDS" --config Release --target SoundshedGuitar_Standalone --parallel

    echo ""
    echo "▶ Building AU…"
    cmake --build "$JUCE_BUILDS" --config Release --target SoundshedGuitar_AU --parallel

    echo ""
    echo "▶ Building VST3…"
    cmake --build "$JUCE_BUILDS" --config Release --target SoundshedGuitar_VST3 --parallel

    echo ""
    echo "▶ Building CLAP…"
    cmake --build "$JUCE_BUILDS" --config Release --target SoundshedGuitar_CLAP --parallel

    echo ""
    echo "▶ Building AAX…"
    cmake --build "$JUCE_BUILDS" --config Release --target SoundshedGuitar_AAX --parallel

    echo ""
    echo "  ✓ All targets built"
fi

# ── Step 3: Stage distribution folder ────────────────────────────────────────
echo ""
echo "▶ Staging distribution layout…"

# Clean previous staging
rm -rf "$DIST_DIR"

mkdir -p "$APP_DST"
mkdir -p "$VST3_DST"
mkdir -p "$AU_DST"
mkdir -p "$CLAP_DST"
mkdir -p "$AAX_DST"

# Copy each artifact (use -R to handle .app / .vst3 / .component bundles)
SRC_APP="${ARTEFACTS}/Standalone/${PRODUCT}.app"
SRC_VST3="${ARTEFACTS}/VST3/${PRODUCT}.vst3"
SRC_AU="${ARTEFACTS}/AU/${PRODUCT}.component"
SRC_CLAP="${ARTEFACTS}/CLAP/${PRODUCT}.clap"
SRC_AAX="${ARTEFACTS}/AAX/${PRODUCT}.aaxplugin"

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

copy_artifact "$SRC_APP"  "$APP_DST"
copy_artifact "$SRC_VST3" "$VST3_DST"
copy_artifact "$SRC_AU"   "$AU_DST"
copy_artifact "$SRC_CLAP" "$CLAP_DST"
copy_artifact "$SRC_AAX"  "$AAX_DST"

echo ""
echo "═══════════════════════════════════════════════════"
echo "  Distribution layout:"
find "$DIST_DIR" -mindepth 2 -maxdepth 8 \( -name "*.app" -o -name "*.vst3" -o -name "*.component" -o -name "*.clap" -o -name "*.aaxplugin" \) \
    | sed "s|${SCRIPT_DIR}/||" | sort | sed 's/^/    /'

# When a universal build was requested, confirm each main binary contains both slices.
if [[ "$UNIVERSAL" == true ]]; then
    echo ""
    echo "  Architecture verification (lipo):"
    while IFS= read -r bundle; do
        # Find the main executable inside the bundle (deepest Mach-O)
        exe=$(find "$bundle" -type f -perm +0111 | head -1)
        if [[ -n "$exe" ]]; then
            archs=$(lipo -archs "$exe" 2>/dev/null || echo "unknown")
            printf "    %-45s  %s\n" "$(basename "$bundle")" "$archs"
        fi
    done < <(find "$DIST_DIR" -mindepth 3 -maxdepth 8 \( -name "*.app" -o -name "*.vst3" -o -name "*.component" -o -name "*.clap" -o -name "*.aaxplugin" \))
fi

echo "═══════════════════════════════════════════════════"

# ── Step 4: Build installer .pkg ─────────────────────────────────────────────
if [[ "$BUILD_PKG" == true ]]; then
    VERSION="$(cat "${SCRIPT_DIR}/juce/VERSION" 2>/dev/null || echo "1.0.0")"
    PKG_STAGING="${DIST_DIR}/pkg-staging"
    COMPONENTS_DIR="${PKG_STAGING}/components"
    SCRIPTS_DIR="${PKG_STAGING}/scripts"
    PKG_OUT="${DIST_DIR}/SoundshedGuitar-${VERSION}.pkg"

    echo ""
    echo "▶ Building installer package (v${VERSION})…"

    # Verify staging is complete before proceeding
    if [[ ! -e "${APP_DST}/${PRODUCT}.app" ]] && \
       [[ ! -e "${VST3_DST}/${PRODUCT}.vst3" ]]; then
        echo "  ✗ No staged artifacts found in ${DIST_DIR#"$SCRIPT_DIR/"}." >&2
        echo "    Run without --skip-build first, or without --pkg if build is missing." >&2
        exit 1
    fi

    rm -rf "$PKG_STAGING"
    mkdir -p "$COMPONENTS_DIR" "$SCRIPTS_DIR"

    # Payload roots — each mirrors the filesystem layout from / so pkgbuild can
    # install each bundle to its correct system destination.
    ROOT_STANDALONE="${PKG_STAGING}/root-standalone"
    ROOT_VST3="${PKG_STAGING}/root-vst3"
    ROOT_AU="${PKG_STAGING}/root-au"
    ROOT_CLAP="${PKG_STAGING}/root-clap"
    ROOT_AAX="${PKG_STAGING}/root-aax"
    ROOT_SHARED="${PKG_STAGING}/root-shared"

    mkdir -p "${ROOT_STANDALONE}/Applications"
    mkdir -p "${ROOT_VST3}/Library/Audio/Plug-Ins/VST3"
    mkdir -p "${ROOT_AU}/Library/Audio/Plug-Ins/Components"
    mkdir -p "${ROOT_CLAP}/Library/Audio/Plug-Ins/CLAP"
    mkdir -p "${ROOT_AAX}/Library/Application Support/Avid/Audio/Plug-Ins"
    mkdir -p "${ROOT_SHARED}/Library/Application Support/Soundshed/Guitar"

    # Copy a bundle into a pkg root, stripping Contents/Resources/ui/ so the
    # UI is not duplicated across the component packages.
    stage_stripped() {
        local src="$1"
        local dst_dir="$2"
        if [[ -e "$src" ]]; then
            cp -R "$src" "${dst_dir}/"
            rm -rf "${dst_dir}/$(basename "$src")/Contents/Resources/ui"
            echo "  ✓ Staged (ui stripped): $(basename "$src")"
        else
            echo "  ⚠ Skipping (not found): $(basename "$src")"
        fi
    }

    stage_stripped "${APP_DST}/${PRODUCT}.app"      "${ROOT_STANDALONE}/Applications"
    stage_stripped "${VST3_DST}/${PRODUCT}.vst3"    "${ROOT_VST3}/Library/Audio/Plug-Ins/VST3"
    stage_stripped "${AU_DST}/${PRODUCT}.component" "${ROOT_AU}/Library/Audio/Plug-Ins/Components"
    stage_stripped "${CLAP_DST}/${PRODUCT}.clap"    "${ROOT_CLAP}/Library/Audio/Plug-Ins/CLAP"
    stage_stripped "${AAX_DST}/${PRODUCT}.aaxplugin" "${ROOT_AAX}/Library/Application Support/Avid/Audio/Plug-Ins"

    # Stage the shared UI resources (sourced from the standalone app — all
    # bundles carry identical copies so the source doesn't matter).
    SRC_UI="${APP_DST}/${PRODUCT}.app/Contents/Resources/ui"
    if [[ ! -d "$SRC_UI" ]]; then
        echo "  ✗ Cannot locate shared ui/ at: ${SRC_UI#"$SCRIPT_DIR/"}" >&2
        exit 1
    fi
    SHARED_UI_ROOT="${ROOT_SHARED}/Library/Application Support/Soundshed/Guitar"
    cp -R "$SRC_UI" "${SHARED_UI_ROOT}/ui"
    echo "  ✓ Staged shared ui/ ($(du -sh "${SHARED_UI_ROOT}/ui" | cut -f1))"

    # ── Postinstall script ────────────────────────────────────────────────────
    # Runs after the shared-resources component is installed (last in the
    # package order). Copies the shared ui/ tree into every plugin bundle the
    # user chose to install, skipping any that were deselected.
    cat > "${SCRIPTS_DIR}/postinstall" <<'POSTINSTALL'
#!/usr/bin/env bash
set -euo pipefail
SHARED_UI="/Library/Application Support/Soundshed/Guitar/ui"

BUNDLE_UI_DIRS=(
    "/Applications/Soundshed Guitar.app/Contents/Resources/ui"
    "/Library/Audio/Plug-Ins/VST3/Soundshed Guitar.vst3/Contents/Resources/ui"
    "/Library/Audio/Plug-Ins/Components/Soundshed Guitar.component/Contents/Resources/ui"
    "/Library/Audio/Plug-Ins/CLAP/Soundshed Guitar.clap/Contents/Resources/ui"
    "/Library/Application Support/Avid/Audio/Plug-Ins/Soundshed Guitar.aaxplugin/Contents/Resources/ui"
)

for ui_dst in "${BUNDLE_UI_DIRS[@]}"; do
    # Only populate bundles that were actually installed
    bundle_contents="$(dirname "$(dirname "$ui_dst")")"
    if [[ -d "$bundle_contents" ]]; then
        rm -rf "$ui_dst"
        cp -R "$SHARED_UI" "$ui_dst"
    fi
done
POSTINSTALL
    chmod +x "${SCRIPTS_DIR}/postinstall"

    # ── Component packages ────────────────────────────────────────────────────
    echo ""
    echo "  Building component packages…"

    pkgbuild \
        --root "$ROOT_STANDALONE" \
        --identifier "com.soundshed.guitar.standalone" \
        --version "$VERSION" \
        --install-location "/" \
        "${COMPONENTS_DIR}/standalone.pkg"
    echo "  ✓ standalone.pkg"

    pkgbuild \
        --root "$ROOT_VST3" \
        --identifier "com.soundshed.guitar.vst3" \
        --version "$VERSION" \
        --install-location "/" \
        "${COMPONENTS_DIR}/vst3.pkg"
    echo "  ✓ vst3.pkg"

    pkgbuild \
        --root "$ROOT_AU" \
        --identifier "com.soundshed.guitar.au" \
        --version "$VERSION" \
        --install-location "/" \
        "${COMPONENTS_DIR}/au.pkg"
    echo "  ✓ au.pkg"

    pkgbuild \
        --root "$ROOT_CLAP" \
        --identifier "com.soundshed.guitar.clap" \
        --version "$VERSION" \
        --install-location "/" \
        "${COMPONENTS_DIR}/clap.pkg"
    echo "  ✓ clap.pkg"

    pkgbuild \
        --root "$ROOT_AAX" \
        --identifier "com.soundshed.guitar.aax" \
        --version "$VERSION" \
        --install-location "/" \
        "${COMPONENTS_DIR}/aax.pkg"
    echo "  ✓ aax.pkg"

    # shared.pkg is built LAST so its postinstall runs after all bundles exist.
    pkgbuild \
        --root "$ROOT_SHARED" \
        --identifier "com.soundshed.guitar.shared" \
        --version "$VERSION" \
        --install-location "/" \
        --scripts "$SCRIPTS_DIR" \
        "${COMPONENTS_DIR}/shared.pkg"
    echo "  ✓ shared.pkg (with postinstall)"

    # ── Distribution XML ──────────────────────────────────────────────────────
    DIST_XML="${PKG_STAGING}/distribution.xml"
    cat > "$DIST_XML" <<DISTXML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Soundshed Guitar ${VERSION}</title>
    <organization>com.soundshed</organization>
    <domains enable_localSystem="true"/>
    <options customize="always" require-scripts="true" rootVolumeOnly="true"/>

    <choices-outline>
        <line choice="choice-standalone"/>
        <line choice="choice-vst3"/>
        <line choice="choice-au"/>
        <line choice="choice-clap"/>
        <line choice="choice-aax"/>
        <line choice="choice-shared"/>
    </choices-outline>

    <!-- Optional formats: user can deselect any of these -->
    <choice id="choice-standalone" title="Standalone App"
            description="Soundshed Guitar standalone application."
            selected="true">
        <pkg-ref id="com.soundshed.guitar.standalone"/>
    </choice>
    <choice id="choice-vst3" title="VST3 Plugin"
            description="VST3 plugin for use in a DAW."
            selected="true">
        <pkg-ref id="com.soundshed.guitar.vst3"/>
    </choice>
    <choice id="choice-au" title="Audio Unit (AU) Plugin"
            description="Audio Unit plugin for Logic Pro and other AU hosts."
            selected="true">
        <pkg-ref id="com.soundshed.guitar.au"/>
    </choice>
    <choice id="choice-clap" title="CLAP Plugin"
            description="CLAP plugin for CLAP-compatible DAWs."
            selected="true">
        <pkg-ref id="com.soundshed.guitar.clap"/>
    </choice>
    <choice id="choice-aax" title="AAX Plugin"
            description="AAX plugin for Avid Pro Tools."
            selected="true">
        <pkg-ref id="com.soundshed.guitar.aax"/>
    </choice>

    <!-- Shared resources: required, greyed-out in the UI.
         Installed last so its postinstall finds all selected bundles. -->
    <choice id="choice-shared" title="Shared Resources"
            description="Shared UI resources required by all formats."
            enabled="false" selected="true" start_selected="true">
        <pkg-ref id="com.soundshed.guitar.shared"/>
    </choice>

    <pkg-ref id="com.soundshed.guitar.standalone">#standalone.pkg</pkg-ref>
    <pkg-ref id="com.soundshed.guitar.vst3">#vst3.pkg</pkg-ref>
    <pkg-ref id="com.soundshed.guitar.au">#au.pkg</pkg-ref>
    <pkg-ref id="com.soundshed.guitar.clap">#clap.pkg</pkg-ref>
    <pkg-ref id="com.soundshed.guitar.aax">#aax.pkg</pkg-ref>
    <pkg-ref id="com.soundshed.guitar.shared">#shared.pkg</pkg-ref>
</installer-gui-script>
DISTXML

    # ── Assemble final distribution package ──────────────────────────────────
    echo ""
    echo "  Assembling distribution package…"
    productbuild \
        --distribution "$DIST_XML" \
        --package-path "$COMPONENTS_DIR" \
        "$PKG_OUT"

    echo ""
    echo "  ✓ Installer: ${PKG_OUT#"$SCRIPT_DIR/"}"
    echo "    Size: $(du -sh "$PKG_OUT" | cut -f1)"
    echo "═══════════════════════════════════════════════════"
fi

echo "  Done."
