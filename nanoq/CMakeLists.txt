# ── Soundshed Nano Q ────────────────────────────────────────────────────────────
# Soundshed Nano without JUCE: the same engine (core/) and the same client library
# (core/src/uiclient) as Soundshed Guitar Nano, hosted by Cycfi's QPlug and drawn with
# Cycfi's Elements. A separate app: juce/ and its Nano target are untouched, and the two
# install side by side (own plugin id, own name). They share the profile folder.
#
# Configure and build from a shell with the MSVC environment loaded (QPlug wants Ninja):
#   cmake -S nanoq -B nanoq/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build nanoq/build
# See docs/plans/nano-q.md.

cmake_minimum_required(VERSION 3.22)

# Before project(): Artist needs std::filesystem, from macOS 10.15, and its prebuilt bundles target 11.0.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(SoundshedNanoQ VERSION 1.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# ── Q, with QPlug and Elements ──────────────────────────────────────────────────
# Pinned to a commit, like every fetched dependency in this repo (tools/check-dependency-pins.mjs).
set(Q_BUILD_PLUG ON CACHE BOOL "" FORCE)
set(Q_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(Q_BUILD_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(q
    GIT_REPOSITORY https://github.com/cycfi/q.git
    GIT_TAG a08c4ca573bb92346ed616bdb6f850c864f59ab4)

# ── The engine ──────────────────────────────────────────────────────────────────
# nlohmann_json first, so that core and Q share one copy.
set(GUITARFX_CORE_FETCH_DEPS ON CACHE BOOL "" FORCE)
# No WebView here, so no web UI bundle to build (and no Node needed).
set(GUITARFX_CORE_BUILD_WEB_UI OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../core" "${CMAKE_BINARY_DIR}/core")

FetchContent_MakeAvailable(q)
q_plug_settings()

# ── Nano Q ──────────────────────────────────────────────────────────────────────
set(NANOQ_ID "com.soundshed.guitar.nanoq")
set(NANOQ_NAME "Soundshed Nano Q")

file(GLOB_RECURSE NANOQ_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# The CLAP entry is compiled by clap-wrapper once per format, not into the impl library.
list(FILTER NANOQ_SOURCES EXCLUDE REGEX "clap_entry[.]cpp$")

add_library(nanoq_impl STATIC ${NANOQ_SOURCES})
target_include_directories(nanoq_impl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # Only for nativeui/theme/ThemeTokens.generated.h: the colours generated from the web UI's
    # CSS, a plain header with no JUCE in it. Nothing else from juce/ is used.
    "${CMAKE_CURRENT_SOURCE_DIR}/../juce/source")
target_compile_definitions(nanoq_impl PRIVATE
    NANOQ_VERSION="${PROJECT_VERSION}"
    NANOQ_ID="${NANOQ_ID}"
    NANOQ_NAME="${NANOQ_NAME}")
# q_plug is PRIVATE on purpose (see Q's example CMakeLists): the format modules link it but must
# not inherit its compile options. clap is PUBLIC because the entry shim needs it.
target_link_libraries(nanoq_impl PRIVATE q_plug SoundshedGuitarCore SoundshedUiClient PUBLIC clap)
q_plug_add_host(nanoq_impl SoundshedNanoQ)

make_clapfirst_plugins(
    TARGET_NAME nanoq
    IMPL_TARGET nanoq_impl
    OUTPUT_NAME "${NANOQ_NAME}"
    # QPlug's shim, plus the host pointer the controller needs to ask for a values rescan.
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/clap_entry.cpp"
    BUNDLE_IDENTIFIER "${NANOQ_ID}"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    COPY_AFTER_BUILD FALSE
    WINDOWS_FOLDER_VST3 TRUE
    PLUGIN_FORMATS ${Q_PLUG_PLUGIN_FORMATS}
    STANDALONE_CONFIGURATIONS
        standalone "${NANOQ_NAME}" "${NANOQ_ID}"
    ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/products"
    AUV2_MANUFACTURER_NAME "Soundshed"
    AUV2_MANUFACTURER_CODE "Shed"
    AUV2_SUBTYPE_CODE "SGNQ"
    AUV2_INSTRUMENT_TYPE "aufx")

q_plug_separate_import_libs(nanoq)
q_plug_fix_auv2_plist(nanoq)

# ── Fonts and runtime data ──────────────────────────────────────────────────────
# Elements finds fonts only beside the plugin. Inter's static cuts are the ones Nano uses
# (juce/source/nativeui/theme/fonts, made by tools/gen-nano-fonts.mjs); Open Sans stays for
# anything Elements draws itself.
get_property(_nanoq_elements GLOBAL PROPERTY Q_PLUG_ELEMENTS_SOURCE_DIR)
set(_nanoq_fonts "${CMAKE_CURRENT_SOURCE_DIR}/../juce/source/nativeui/theme/fonts")
set(ELEMENTS_FONTS
    "${_nanoq_fonts}/Inter-Regular.ttf"
    "${_nanoq_fonts}/Inter-Medium.ttf"
    "${_nanoq_fonts}/Inter-SemiBold.ttf"
    "${_nanoq_elements}/resources/fonts/OpenSans-Regular.ttf")
q_plug_add_resources(nanoq)

# The engine reads factory presets, layouts, metronome clicks and demo audio from ui/ in its
# resource root, the same data Soundshed Guitar Nano ships (juce/cmake/SoundshedNano.cmake).
# It lands where the fonts do: "<name> Resources" beside a CLAP or the standalone app, and
# Contents/Resources inside a VST3 or AU bundle. Nano Q's own code finds either (Paths.cpp).
set(_NANOQ_UI_DATA_DIRS presets assets metronome demo data images)
foreach(_format clap vst3 auv2 standalone)
    set(_target nanoq_${_format})
    if(NOT TARGET ${_target})
        continue()
    endif()
    if(NOT APPLE AND _format MATCHES "^(clap|standalone)$")
        set(_dest "$<TARGET_FILE_DIR:${_target}>/$<TARGET_PROPERTY:${_target},OUTPUT_NAME> Resources")
    else()
        set(_dest "$<TARGET_FILE_DIR:${_target}>/../Resources")
    endif()
    set(_commands)
    foreach(_dir IN LISTS _NANOQ_UI_DATA_DIRS)
        list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/${_dir}" "${_dest}/ui/${_dir}")
    endforeach()
    add_custom_command(TARGET ${_target} POST_BUILD ${_commands}
        COMMENT "Copying engine data into ${_target}" VERBATIM)
endforeach()
