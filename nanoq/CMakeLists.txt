# ── Soundshed Nano Q ────────────────────────────────────────────────────────────
# The native UI for Soundshed Guitar's engine (core/) and client library (core/src/uiclient),
# hosted by Cycfi's QPlug and drawn with Cycfi's Elements, with no JUCE. A separate app: it
# installs beside Soundshed Guitar (own plugin id, own name) and shares its profile folder.
#
# Configure and build from a shell with the MSVC environment loaded (QPlug wants Ninja):
#   cmake -S nanoq -B nanoq/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build nanoq/build
# See docs/plans/nano-q.md.
#
# iOS and iPadOS: the Xcode generator with -DCMAKE_SYSTEM_NAME=iOS, through nanoq/ios/build.sh,
# which builds the app in nanoq/ios/cmake/NanoQIos.cmake instead of the plugin formats. See
# docs/plans/nano-q-ios.md.

cmake_minimum_required(VERSION 3.22)

# Before project(). macOS: Artist needs std::filesystem, from 10.15, and its prebuilt bundles target
# 11.0. iOS: 15.0, as Soundshed Guitar Nano (docs/ios-build.md); a legacy build passes 12.0.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS version")
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SoundshedNanoQ VERSION 1.7.0 LANGUAGES C CXX)

# Objective-C++ before the engine is added: the native UIs' shared HTTP (core/src/net) uses
# NSURLSession on Apple platforms, and core picks its transport by the languages enabled.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(NANOQ_IOS ON)
else()
    set(NANOQ_IOS OFF)
endif()

# ── Q, with QPlug and Elements ──────────────────────────────────────────────────
# Pinned to a commit, like every fetched dependency in this repo (tools/check-dependency-pins.mjs).
set(Q_BUILD_PLUG ON CACHE BOOL "" FORCE)
set(Q_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(Q_BUILD_TEST OFF CACHE BOOL "" FORCE)

# iOS: the libraries are patched for UIKit (nanoq/ios/patches, applied by nanoq/ios/cmake/
# ApplyPatch.cmake), and Q's IO library is left out: RtAudio and libremidi have no iOS backend,
# and the app plays through RemoteIO instead. A desktop build fetches them as published.
set(_nanoq_q_patch)
if(NANOQ_IOS)
    set(Q_BUILD_IO OFF CACHE BOOL "" FORCE)
    set(_nanoq_q_patch PATCH_COMMAND ${CMAKE_COMMAND}
        -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/ios/patches/q.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ios/cmake/ApplyPatch.cmake)
endif()
FetchContent_Declare(q
    GIT_REPOSITORY https://github.com/cycfi/q.git
    GIT_TAG efe26114f39e4a78bd67c1456a25487d356fb910 # v1.5.4
    ${_nanoq_q_patch})

# Elements is Q's to fetch (q_plug/CMakeLists.txt). Nano Q declares it first, with the same
# options, to patch it: the first declaration of a name is the one FetchContent uses. Q v1.5.4
# pins Elements' q-v1.5.3 branch; Nano Q takes Elements' own release instead, v1.0.2, for the
# fixes since (among them quill's idle sleep, which at 500 ns took ~60% of a core on macOS).
# Every build takes patches/elements-input-redraws.patch: a click no longer redraws the whole
# window, nor a press over a scroller's content the whole scrolled area (on iOS, where Quartz
# draws on the CPU, each tap cost two whole-window draws). And patches/elements-macos-tick.patch:
# 1.0 polls a macOS view every 1 ms instead of 10, which cost ~6% of a core with the window idle.
# An iOS build takes Artist's patch too.
set(_nanoq_elements_patch PATCH_COMMAND ${CMAKE_COMMAND}
    -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/elements-input-redraws.patch
    -P ${CMAKE_CURRENT_SOURCE_DIR}/ios/cmake/ApplyPatch.cmake
    COMMAND ${CMAKE_COMMAND}
    -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/elements-macos-tick.patch
    -P ${CMAKE_CURRENT_SOURCE_DIR}/ios/cmake/ApplyPatch.cmake)
if(NANOQ_IOS)
    # elements-log-clock.patch: Elements' quill loggers stamp messages with quill's TSC, which
    # on arm64 reads cntvct_el0; iOS 12 traps that (SIGILL at launch), so iOS takes the system
    # clock.
    list(APPEND _nanoq_elements_patch COMMAND ${CMAKE_COMMAND}
        -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/ios/patches/elements.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ios/cmake/ApplyPatch.cmake
        COMMAND ${CMAKE_COMMAND}
        -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/ios/patches/elements-log-clock.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ios/cmake/ApplyPatch.cmake)
endif()
FetchContent_Declare(cycfi_elements
    GIT_REPOSITORY https://github.com/cycfi/elements.git
    GIT_TAG fa180799a4c80d4cd70850c700df766f62917de7 # v1.0.2
    GIT_SUBMODULES lib/artist
    GIT_SUBMODULES_RECURSE OFF
    SOURCE_SUBDIR do-not-add
    ${_nanoq_elements_patch})

if(NANOQ_IOS)
    # As Soundshed Guitar's iOS build (juce/CMakeLists.txt): Wasmtime has no iOS C API bundle and
    # iOS allows no JIT, and the SIMD level matters only to the x86_64 Simulator.
    set(GUITARFX_CORE_ENABLE_WASM_EFFECTS OFF CACHE BOOL "" FORCE)
    set(GUITARFX_CORE_SIMD_LEVEL "sse2" CACHE STRING "" FORCE)
    # Below iOS 13 (ios/build.sh legacy): std::filesystem built into the app. Before the core.
    include("${CMAKE_CURRENT_SOURCE_DIR}/../ios/cmake/IosFilesystemBackDeploy.cmake")

    # Drawing on the GPU: Artist's Skia backend on Metal, with the Skia that ios/build-skia.sh
    # builds (arm64, so device builds only; ios/build.sh passes it). Without it Artist draws with
    # Quartz on the CPU, which on an iPad mini 2 took over a second to redraw the window.
    set(NANOQ_IOS_SKIA "" CACHE PATH "Skia for iOS (vcpkg's installed tree), to draw with Skia on Metal")
    if(NANOQ_IOS_SKIA)
        set(ARTIST_SKIA ON CACHE BOOL "" FORCE)
        set(ARTIST_QUARTZ_2D OFF CACHE BOOL "" FORCE)
        set(ARTIST_SKIA_PREBUILT OFF CACHE BOOL "" FORCE)
        list(PREPEND CMAKE_PREFIX_PATH "${NANOQ_IOS_SKIA}")
        list(PREPEND CMAKE_FIND_ROOT_PATH "${NANOQ_IOS_SKIA}")
    else()
        set(ARTIST_SKIA OFF CACHE BOOL "" FORCE)
        set(ARTIST_QUARTZ_2D ON CACHE BOOL "" FORCE)
    endif()
endif()

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
# src/standalone/ talks to clap-wrapper's standalone host, which only the standalone executable
# has: it is compiled into that target alone (below), so the plugin formats never reference it.
list(FILTER NANOQ_SOURCES EXCLUDE REGEX "/src/standalone/[^/]*$")
# The UI as a child of another framework's window, for Soundshed Guitar's JUCE editor
# (juce/cmake/NanoQUi.cmake). QPlug gives Nano Q its view, so Nano Q does not need it.
list(FILTER NANOQ_SOURCES EXCLUDE REGEX "/src/ui/EmbeddedUi[.]cpp$")

# macOS: the open and save dialogs are AppKit panels (src/FileDialogs_mac.mm), built with ARC so
# the panel outlives the call that shows it. iOS: a document picker (src/FileDialogs_ios.mm), and
# QPlug's view glue for UIKit.
if(NANOQ_IOS)
    set(NANOQ_APPLE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/FileDialogs_ios.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/ios/q_plug_host_view.mm")
    set_source_files_properties(${NANOQ_APPLE_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # UniformTypeIdentifiers is iOS 14: weak, so a legacy (iOS 12) build still loads.
    set(NANOQ_APPLE_FRAMEWORKS "-framework UIKit" "-weak_framework UniformTypeIdentifiers")
    if(NANOQ_IOS_SKIA)
        list(APPEND NANOQ_APPLE_FRAMEWORKS "-framework Metal" "-framework QuartzCore")
    endif()
    list(APPEND NANOQ_SOURCES ${NANOQ_APPLE_SOURCES})
elseif(APPLE)
    enable_language(OBJCXX)
    set(NANOQ_APPLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FileDialogs_mac.mm")
    set_source_files_properties(${NANOQ_APPLE_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(NANOQ_APPLE_FRAMEWORKS "-framework AppKit" "-framework UniformTypeIdentifiers")
    list(APPEND NANOQ_SOURCES ${NANOQ_APPLE_SOURCES})
endif()

add_library(nanoq_impl STATIC ${NANOQ_SOURCES})
target_include_directories(nanoq_impl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(nanoq_impl PRIVATE
    NANOQ_VERSION="${PROJECT_VERSION}"
    NANOQ_ID="${NANOQ_ID}"
    NANOQ_NAME="${NANOQ_NAME}")
# q_plug is PRIVATE on purpose (see Q's example CMakeLists): the format modules link it but must
# not inherit its compile options. clap is PUBLIC because the entry shim needs it.
target_link_libraries(nanoq_impl PRIVATE q_plug SoundshedGuitarCore SoundshedUiClient PUBLIC clap)
if(APPLE)
    target_link_libraries(nanoq_impl PRIVATE ${NANOQ_APPLE_FRAMEWORKS})
endif()

# Windows: the Jam tab's YouTube player and scale explorer are web pages, shown in a WebView2
# child window (src/WebView_win.cpp). The SDK comes from NuGet at the version the JUCE build uses;
# the loader links statically, so no WebView2Loader.dll has to sit beside the plugin. Without it
# the Jam tab opens those pages in the browser instead.
if(WIN32)
    set(NANOQ_WEBVIEW2_VERSION "1.0.3856.49" CACHE STRING "WebView2 NuGet package version")
    set(_nanoq_wv2 "${CMAKE_BINARY_DIR}/_deps/microsoft.web.webview2/${NANOQ_WEBVIEW2_VERSION}")
    if(NOT EXISTS "${_nanoq_wv2}/build/native/include/WebView2.h")
        set(_nanoq_wv2_pkg "${CMAKE_BINARY_DIR}/_deps/microsoft.web.webview2.${NANOQ_WEBVIEW2_VERSION}.nupkg")
        file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${NANOQ_WEBVIEW2_VERSION}" "${_nanoq_wv2_pkg}"
             STATUS _nanoq_wv2_status TLS_VERIFY ON)
        list(GET _nanoq_wv2_status 0 _nanoq_wv2_code)
        if(_nanoq_wv2_code EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${_nanoq_wv2_pkg}" DESTINATION "${_nanoq_wv2}")
        endif()
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)$")
        set(_nanoq_wv2_arch arm64)
    else()
        set(_nanoq_wv2_arch x64)
    endif()
    set(_nanoq_wv2_lib "${_nanoq_wv2}/build/native/${_nanoq_wv2_arch}/WebView2LoaderStatic.lib")
    if(EXISTS "${_nanoq_wv2}/build/native/include/WebView2.h" AND EXISTS "${_nanoq_wv2_lib}")
        target_include_directories(nanoq_impl PRIVATE "${_nanoq_wv2}/build/native/include")
        target_link_libraries(nanoq_impl PRIVATE "${_nanoq_wv2_lib}" version)
        target_compile_definitions(nanoq_impl PRIVATE NANOQ_WEBVIEW2=1)
    else()
        message(WARNING "WebView2 SDK not found: the Jam tab opens its web pages in the browser")
    endif()
endif()
# iOS: Elements has no UIKit host of its own. QPlug compiles the host layer from this property, so
# pointing it at Nano Q's (nanoq/ios/elements_host) is all it takes.
if(NANOQ_IOS)
    set_property(GLOBAL PROPERTY Q_PLUG_ELEMENTS_HOST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/ios/elements_host/base_view.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/ios/elements_host/key.mm")
endif()
q_plug_add_host(nanoq_impl SoundshedNanoQ)

# A precompiled header for the UI. Parsing Elements and ui/Shell.h was ~9 s of the ~10 s each UI
# file took to compile on its own; with them precompiled a file takes ~1.5 s. Only the sources that
# include ui/Shell.h use it (read at configure time), so the engine, the platform glue and the
# Elements host layer QPlug adds above never see its names. A source that starts including
# Shell.h compiles without the PCH until the next configure.
target_precompile_headers(nanoq_impl PRIVATE <elements.hpp> "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/Shell.h")
get_target_property(_nanoq_impl_sources nanoq_impl SOURCES)
foreach(_src IN LISTS _nanoq_impl_sources)
    set(_uses_shell)
    if(EXISTS "${_src}")
        file(STRINGS "${_src}" _uses_shell REGEX "^#include \"ui/Shell.h\"")
    endif()
    if(NOT _uses_shell)
        set_source_files_properties("${_src}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()

# Fonts Nano Q draws with. Elements finds fonts only beside the plugin. Inter's static cuts are the
# ones Nano uses (core/nativeui/fonts, made by tools/gen-nano-fonts.mjs); Open Sans
# stays for anything Elements draws itself.
get_property(_nanoq_elements GLOBAL PROPERTY Q_PLUG_ELEMENTS_SOURCE_DIR)
set(_nanoq_fonts "${CMAKE_CURRENT_SOURCE_DIR}/../core/nativeui/fonts")
set(ELEMENTS_FONTS
    "${_nanoq_fonts}/Inter-Regular.ttf"
    "${_nanoq_fonts}/Inter-Medium.ttf"
    "${_nanoq_fonts}/Inter-SemiBold.ttf"
    "${_nanoq_elements}/resources/fonts/OpenSans-Regular.ttf")

# The engine's runtime data: factory presets, layouts, metronome clicks and demo audio from ui/ in
# its resource root (the web UI itself is not shipped).
set(_NANOQ_UI_DATA_DIRS presets assets metronome demo data images)

if(NANOQ_IOS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/ios/cmake/NanoQIos.cmake")
    return()
endif()

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

# The standalone app's audio and MIDI device controls (src/StandaloneDevices.h): the backend over
# clap-wrapper's standalone host is built into the standalone executable only, where that host
# (getStandaloneHost, RtAudio, RtMidi) exists, and registers itself at static initialisation. A
# plugin format links no backend, so it offers no device settings and needs none of those symbols.
if(TARGET nanoq_standalone)
    target_sources(nanoq_standalone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/standalone/ClapWrapperDevices.cpp")
    target_include_directories(nanoq_standalone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
q_plug_fix_auv2_plist(nanoq)

# ── Fonts and runtime data ──────────────────────────────────────────────────────
q_plug_add_resources(nanoq)

# The engine's data lands where the fonts do: "<name> Resources" beside a CLAP or the standalone
# app, and Contents/Resources inside a VST3 or AU bundle. Nano Q's own code finds either
# (Paths.cpp).
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

# ── Tests ───────────────────────────────────────────────────────────────────────
# What can run with no host and no view: the engine and the threads it does not own. Built from
# the sources they need rather than nanoq_impl, which would bring QPlug and Elements along.
enable_testing()

add_executable(NanoQEngineDialogTests
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/EngineDialogLifetimeTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FileDialogs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Log.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Paths.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/StandaloneDevices.cpp")
target_include_directories(NanoQEngineDialogTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(NanoQEngineDialogTests PRIVATE
    NANOQ_VERSION="${PROJECT_VERSION}"
    NANOQ_ID="${NANOQ_ID}"
    NANOQ_NAME="${NANOQ_NAME}")
target_link_libraries(NanoQEngineDialogTests PRIVATE SoundshedGuitarCore SoundshedUiClient)
if(APPLE)
    target_sources(NanoQEngineDialogTests PRIVATE ${NANOQ_APPLE_SOURCES})
    target_link_libraries(NanoQEngineDialogTests PRIVATE ${NANOQ_APPLE_FRAMEWORKS})
endif()
if(WIN32)
    target_link_libraries(NanoQEngineDialogTests PRIVATE ole32 uuid)
endif()
add_test(NAME NanoQEngineDialogTests COMMAND NanoQEngineDialogTests)
