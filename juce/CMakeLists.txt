cmake_minimum_required(VERSION 3.25)

# This tells cmake we have goodies in the /cmake folder
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include (PamplejuceVersion)

# Change me!
# This is the internal name of the project and the name of JUCE's shared code target
# Note: This cannot have spaces (it may be 2024, but you can't have it all!)
# Worry not, JUCE's PRODUCT_NAME can have spaces (and is what DAWs display)
set(PROJECT_NAME "SoundshedGuitar")

# Worry not, JUCE's PRODUCT_NAME can have spaces (and is what DAWs will display)
# You can also just have it be the same thing as PROJECT_NAME
# You may want to append the major version on the end of this (and PROJECT_NAME) ala:
#   set(PROJECT_NAME "MyPlugin_v${MAJOR_VERSION}")
# Doing so enables major versions to show up in IDEs and DAWs as separate plugins
# allowing you to change parameters and behavior without breaking existing user projects
set(PRODUCT_NAME "Soundshed Guitar")

# Change me! Used for the MacOS bundle name and Installers
set(COMPANY_NAME "Soundshed")

# Change me! Used for the MacOS bundle identifier (and signing)
set(BUNDLE_ID "com.soundshed.guitar")

# Change me! Set the plugin formats you want built
# Valid choices: AAX Unity VST VST3 AU AUv3 Standalone LV2
set(FORMATS Standalone AU AAX VST3)
option(GUITARFX_ENABLE_LV2 "Build the LV2 plugin target on Linux" OFF)

if(LINUX AND GUITARFX_ENABLE_LV2)
    list(APPEND FORMATS LV2)
endif()

set(GUITARFX_LV2_URI "urn:soundshed:guitar" CACHE STRING "Stable LV2 URI for Soundshed Guitar")

set(GUITARFX_VST3_AUTO_MANIFEST TRUE)

if(WIN32
    AND CMAKE_GENERATOR_PLATFORM MATCHES "^ARM64(EC)?$"
   AND NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "ARM64"
   AND NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(GUITARFX_VST3_AUTO_MANIFEST FALSE)
endif()

# Register soundshed:// URL scheme on macOS for browser-to-app handoff.
set(GUITARFX_XCODE_EXTRA_PLIST_ENTRIES "")
if(APPLE)
        set(GUITARFX_XCODE_EXTRA_PLIST_ENTRIES [=[
<key>CFBundleURLTypes</key>
<array>
    <dict>
        <key>CFBundleURLName</key>
        <string>com.soundshed.guitar</string>
        <key>CFBundleURLSchemes</key>
        <array>
            <string>soundshed</string>
        </array>
    </dict>
</array>
]=])
endif()

# For simplicity, the name of the CMake project is also the name of the target
project(${PROJECT_NAME} VERSION ${CURRENT_VERSION})

# Modern concise way to add dependencies to your project
include (CPM)
include(FetchContent)

CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    VERSION 3.11.3
)

if (WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/../core/cmake/GuitarfxIPPConfig.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../core/cmake/GuitarfxWindowsArch.cmake")
    guitarfx_detect_windows_arch(_guitarfx_windows_target_arch)

    if(_guitarfx_windows_target_arch MATCHES "^(x64|amd64|x86_64)$")
        CPMAddPackage(
            NAME intelipp_static_win_x64
            URL https://www.nuget.org/api/v2/package/intelipp.static.win-x64/${GUITARFX_IPP_WIN_X64_VERSION}
        )
    elseif(_guitarfx_windows_target_arch MATCHES "^(win32|x86|i386)$")
        CPMAddPackage(
            NAME intelipp_static_win_x86
            URL https://www.nuget.org/api/v2/package/intelipp.static.win-x86/${GUITARFX_IPP_WIN_X86_VERSION}
        )
    endif()
endif()

include(PamplejuceIPP)
if(GUITARFX_IPP_AVAILABLE)
    set(SIGNALSMITH_USE_IPP ON CACHE BOOL "Use Intel IPP backend for Signalsmith Linear" FORCE)
endif()

# Signalsmith headers are provided transitively by SoundshedGuitarCore.

# The shared detector lowercases Visual Studio's canonical Win32 platform name for comparisons.
if(WIN32 AND _guitarfx_windows_target_arch MATCHES "^(win32|x86|i386)$")
    set(GUITARFX_CORE_ENABLE_WASM_EFFECTS OFF CACHE BOOL "Build the generic WASM effect host" FORCE)
    message(STATUS "Windows x86 build detected; disabling WASM effects because Wasmtime does not provide a Windows x86 C API bundle.")
endif()

# Configures universal binaries and decides which version of macOS to support
include(PamplejuceMacOS)

if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
    set(GUITARFX_CORE_ENABLE_WASM_EFFECTS OFF CACHE BOOL "Build the generic WASM effect host" FORCE)
    message(STATUS "Universal macOS build detected; disabling WASM effects because Wasmtime C API bundles are single-architecture.")
endif()

# Shared core library (framework-agnostic DSP, presets, resources, controller)
set(GUITARFX_CORE_FETCH_DEPS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core
                 ${CMAKE_BINARY_DIR}/core)

# Couple tweaks that IMO should be JUCE defaults
include(JUCEDefaults)

option(GUITARFX_WKWEBVIEW_INLINE_MEDIA_PLAYBACK "Enable inline media playback on Apple WKWebView where supported" ON)
option(GUITARFX_WKWEBVIEW_DISABLE_MEDIA_USER_ACTION "Disable Apple WKWebView user-action requirement for media playback where supported" ON)

set(_guitarfx_enable_jam_default ON)
if(APPLE)
    set(_guitarfx_enable_jam_default OFF)
endif()
option(GUITARFX_ENABLE_JAM "Enable the Jam panel in the packaged UI" ${_guitarfx_enable_jam_default})

set(_GUITARFX_GENERATED_UI_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/ui")
file(MAKE_DIRECTORY "${_GUITARFX_GENERATED_UI_DIR}")
if(GUITARFX_ENABLE_JAM)
    set(GUITARFX_ENABLE_JAM_JS "true")
else()
    set(GUITARFX_ENABLE_JAM_JS "false")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ui-build-flags.js.in"
    "${_GUITARFX_GENERATED_UI_DIR}/build-flags.js"
    @ONLY)

# WebView2 static SDK package location (NuGet packages root)
if(WIN32)
    set(GUITARFX_WEBVIEW2_VERSION "1.0.3856.49" CACHE STRING "WebView2 NuGet package version")
    option(GUITARFX_AUTO_INSTALL_WEBVIEW2 "Auto-download WebView2 NuGet package when missing" ON)
    set(GUITARFX_WEBVIEW2_STATIC_LINKING 1)
    set(GUITARFX_WEBVIEW2_LOADER_ARCH "x64")
    set(GUITARFX_WINDOWS_PACKAGE_ARCH "x86_64-win")

    if(_guitarfx_windows_target_arch MATCHES "^(arm64ec)$")
        set(GUITARFX_WEBVIEW2_STATIC_LINKING 0)
        set(GUITARFX_WEBVIEW2_LOADER_ARCH "arm64")
        set(GUITARFX_WINDOWS_PACKAGE_ARCH "arm64ec-win")
    elseif(_guitarfx_windows_target_arch MATCHES "^(arm64|aarch64)$")
        set(GUITARFX_WEBVIEW2_LOADER_ARCH "arm64")
        set(GUITARFX_WINDOWS_PACKAGE_ARCH "arm64-win")
    elseif(_guitarfx_windows_target_arch MATCHES "^(win32|x86|i386)$")
        set(GUITARFX_WEBVIEW2_LOADER_ARCH "x86")
        set(GUITARFX_WINDOWS_PACKAGE_ARCH "x86-win")
    endif()

    set(_nuget_packages_root "")

    if(DEFINED ENV{JUCE_WEBVIEW2_PACKAGE_LOCATION} AND NOT "$ENV{JUCE_WEBVIEW2_PACKAGE_LOCATION}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{JUCE_WEBVIEW2_PACKAGE_LOCATION}" _nuget_packages_root)
    elseif(DEFINED ENV{USERPROFILE} AND EXISTS "$ENV{USERPROFILE}/AppData/Local/PackageManagement/NuGet/Packages")
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}/AppData/Local/PackageManagement/NuGet/Packages" _nuget_packages_root)
    elseif(DEFINED ENV{NUGET_PACKAGES} AND NOT "$ENV{NUGET_PACKAGES}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{NUGET_PACKAGES}" _nuget_packages_root)
    elseif(DEFINED ENV{USERPROFILE} AND EXISTS "$ENV{USERPROFILE}/.nuget/packages")
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.nuget/packages" _nuget_packages_root)
    endif()

    if((NOT DEFINED JUCE_WEBVIEW2_PACKAGE_LOCATION OR JUCE_WEBVIEW2_PACKAGE_LOCATION STREQUAL "") AND NOT _nuget_packages_root STREQUAL "")
        set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${_nuget_packages_root}" CACHE PATH "WebView2 NuGet packages root")
    endif()

    if((NOT DEFINED WebView2_root_dir OR WebView2_root_dir STREQUAL "") AND NOT JUCE_WEBVIEW2_PACKAGE_LOCATION STREQUAL "")
        set(WebView2_root_dir "${JUCE_WEBVIEW2_PACKAGE_LOCATION}/microsoft.web.webview2/${GUITARFX_WEBVIEW2_VERSION}" CACHE PATH "WebView2 package directory")
    endif()

    set(_webview2_header_hint "${WebView2_root_dir}/build/native/include/WebView2.h")
    if((NOT DEFINED WebView2_root_dir OR WebView2_root_dir STREQUAL "" OR NOT EXISTS "${_webview2_header_hint}") AND GUITARFX_AUTO_INSTALL_WEBVIEW2)
        set(_webview2_download_root "${CMAKE_BINARY_DIR}/_deps/microsoft.web.webview2/${GUITARFX_WEBVIEW2_VERSION}")
        set(_webview2_header_downloaded "${_webview2_download_root}/build/native/include/WebView2.h")

        if(NOT EXISTS "${_webview2_header_downloaded}")
            file(MAKE_DIRECTORY "${_webview2_download_root}")
            set(_webview2_nupkg "${CMAKE_BINARY_DIR}/_deps/microsoft.web.webview2.${GUITARFX_WEBVIEW2_VERSION}.nupkg")
            file(DOWNLOAD
                "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${GUITARFX_WEBVIEW2_VERSION}"
                "${_webview2_nupkg}"
                STATUS _webview2_download_status
                SHOW_PROGRESS
                TLS_VERIFY ON)

            list(GET _webview2_download_status 0 _webview2_download_code)
            if(_webview2_download_code EQUAL 0)
                file(ARCHIVE_EXTRACT INPUT "${_webview2_nupkg}" DESTINATION "${_webview2_download_root}")
            else()
                list(GET _webview2_download_status 1 _webview2_download_message)
                message(WARNING "WebView2 NuGet auto-download failed: ${_webview2_download_message}")
            endif()
        endif()

        if(EXISTS "${_webview2_header_downloaded}")
            set(WebView2_root_dir "${_webview2_download_root}" CACHE PATH "WebView2 package directory" FORCE)
            message(STATUS "Using auto-downloaded WebView2 package at ${WebView2_root_dir}")
        endif()
    endif()

    set(GUITARFX_WEBVIEW2_LOADER_DLL "${WebView2_root_dir}/build/native/${GUITARFX_WEBVIEW2_LOADER_ARCH}/WebView2Loader.dll")

    set(GUITARFX_ASIO_SDK_DIR "" CACHE PATH "Optional path to a custom Steinberg ASIO SDK root folder")
    set(GUITARFX_JUCE_ASIO_ENABLED 1)
    set(GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK 0)
    set(_guitarfx_asio_external_include_dirs "")

    if(GUITARFX_ASIO_SDK_DIR STREQUAL "" AND DEFINED ENV{ASIO_SDK_DIR} AND NOT "$ENV{ASIO_SDK_DIR}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{ASIO_SDK_DIR}" _asio_sdk_dir_from_env)
        set(GUITARFX_ASIO_SDK_DIR "${_asio_sdk_dir_from_env}" CACHE PATH "Optional path to a custom Steinberg ASIO SDK root folder" FORCE)
    endif()

    if(NOT GUITARFX_ASIO_SDK_DIR STREQUAL "")
        set(_guitarfx_asio_header_candidates
            "${GUITARFX_ASIO_SDK_DIR}/iasiodrv.h"
            "${GUITARFX_ASIO_SDK_DIR}/host/iasiodrv.h"
            "${GUITARFX_ASIO_SDK_DIR}/common/iasiodrv.h")

        foreach(_guitarfx_asio_header IN LISTS _guitarfx_asio_header_candidates)
            if(EXISTS "${_guitarfx_asio_header}")
                set(GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK 1)
                break()
            endif()
        endforeach()

        if(GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK)
            list(APPEND _guitarfx_asio_external_include_dirs
                "${GUITARFX_ASIO_SDK_DIR}"
                "${GUITARFX_ASIO_SDK_DIR}/host"
                "${GUITARFX_ASIO_SDK_DIR}/common")
            list(REMOVE_DUPLICATES _guitarfx_asio_external_include_dirs)
            message(STATUS "Windows ASIO enabled using external SDK at ${GUITARFX_ASIO_SDK_DIR}")
        else()
            message(WARNING "GUITARFX_ASIO_SDK_DIR='${GUITARFX_ASIO_SDK_DIR}' does not contain iasiodrv.h; falling back to JUCE bundled ASIO sources.")
        endif()
    else()
        message(STATUS "Windows ASIO enabled using JUCE bundled ASIO sources")
    endif()
else()
    set(GUITARFX_JUCE_ASIO_ENABLED 0)
    set(GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK 0)
    set(_guitarfx_asio_external_include_dirs "")
endif()

# Force JUCE to work with sccache
# instruct MSVC to embed debug info (/Z7) instead of emitting a .pdb
set(CMAKE_POLICY_DEFAULT_CMP0141        NEW      CACHE STRING "" FORCE)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE STRING "" FORCE)

# JUCE is setup in the /JUCE folder and this build enforces a pinned JUCE version.
# If your checkout drifts, configure will fail with an actionable message.
set(GUITARFX_REQUIRED_JUCE_VERSION "8.0.13")
set(_guitarfx_juce_core_header "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/modules/juce_core/juce_core.h")
if(NOT EXISTS "${_guitarfx_juce_core_header}")
    message(FATAL_ERROR "Missing JUCE checkout at ${_guitarfx_juce_core_header}. Ensure JUCE is available in juce/JUCE.")
endif()
file(READ "${_guitarfx_juce_core_header}" _guitarfx_juce_core_contents)
string(REGEX MATCH "version:[ \t]*([0-9]+\\.[0-9]+\\.[0-9]+)" _guitarfx_juce_version_match "${_guitarfx_juce_core_contents}")
if(NOT _guitarfx_juce_version_match)
    message(FATAL_ERROR "Unable to detect JUCE version from ${_guitarfx_juce_core_header}")
endif()
set(_guitarfx_detected_juce_version "${CMAKE_MATCH_1}")
if(NOT _guitarfx_detected_juce_version STREQUAL GUITARFX_REQUIRED_JUCE_VERSION)
    message(FATAL_ERROR
        "JUCE version mismatch: required ${GUITARFX_REQUIRED_JUCE_VERSION}, found ${_guitarfx_detected_juce_version}. "
        "Please checkout tag ${GUITARFX_REQUIRED_JUCE_VERSION} in juce/JUCE.")
endif()
message(STATUS "Using pinned JUCE version ${_guitarfx_detected_juce_version}")

# On Github Actions, JUCE checkout provisioning is handled by the workflow checkout step.
add_subdirectory(JUCE)

# Add CLAP format
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()
add_subdirectory(modules/clap-juce-extensions EXCLUDE_FROM_ALL)

# Add any other modules you want modules here, before the juce_add_plugin call
# juce_add_module(modules/my_module)

# See `docs/CMake API.md` in the JUCE repo for all config options
juce_add_plugin("${PROJECT_NAME}"
    # Icons for the standalone app
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/images/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/images/icon_sm.png"

    # Change me!
    COMPANY_NAME "${COMPANY_NAME}"
    BUNDLE_ID "${BUNDLE_ID}"

    # Keep build artefacts local; installers handle deployment to system plugin folders.
    COPY_PLUGIN_AFTER_BUILD FALSE

    # Change me!
    # A four-character plugin id
    # First character MUST be uppercase for AU formats
    PLUGIN_MANUFACTURER_CODE Shed

    # Change me!
    # A unique four-character plugin id
    # Note: this must have at least one upper-case character
    PLUGIN_CODE SND1
    LV2URI "${GUITARFX_LV2_URI}"
    FORMATS "${FORMATS}"
    VST3_AUTO_MANIFEST ${GUITARFX_VST3_AUTO_MANIFEST}
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Soundshed Guitar needs microphone access to record and process live audio input."
    XCODE_EXTRA_PLIST_ENTRIES "${GUITARFX_XCODE_EXTRA_PLIST_ENTRIES}"

    # The name of your final executable
    # This is how it's listed in the DAW
    # This can be different from PROJECT_NAME and can have spaces!
    # You might want to use v${MAJOR_VERSION} here once you go to v2...
    PRODUCT_NAME "${PRODUCT_NAME}")

# This lets us use our code in both the JUCE targets and our Test target
# Without running into ODR violations
add_library(SharedCode INTERFACE)

clap_juce_extensions_plugin(TARGET "${PROJECT_NAME}"
    CLAP_ID "${BUNDLE_ID}"
    CLAP_FEATURES audio-effect)

# Enable fast math, C++20 and a few other target defaults
include(SharedCodeDefaults)


# JUCE-specific source files.  Shared DSP, presets, resources, models, network
# and utility code are now provided by the SoundshedGuitarCore library.
# Only the JUCE adapter and editor sources are compiled here.
set(SourceFiles
    "${CMAKE_CURRENT_SOURCE_DIR}/source/Main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/JuceHostedPluginEffect.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/JuceHostedPluginEffect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/PluginEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/PluginEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/PluginProcessorAdapter.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/PluginProcessorAdapter.cpp"
)
target_sources(SharedCode INTERFACE ${SourceFiles})

target_include_directories(SharedCode INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/source)


# MacOS only: Cleans up folder and target organization on Xcode.
include(XcodePrettify)

# This is where you can set preprocessor definitions for JUCE and your plugin
target_compile_definitions(SharedCode
    INTERFACE

    # Use custom standalone app entry point (Main.cpp)
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1

    # JUCE_WEB_BROWSER and JUCE_USE_CURL off by default
    JUCE_WEB_BROWSER=1  # If you set this to 1, add `NEEDS_WEB_BROWSER TRUE` to the `juce_add_plugin` call
    JUCE_USE_CURL=0     # If you set this to 1, add `NEEDS_CURL TRUE` to the `juce_add_plugin` call
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LV2=1
    
    # Enable ASIO on Windows. JUCE 8.0.11+ uses bundled ASIO sources by default.
    JUCE_ASIO=${GUITARFX_JUCE_ASIO_ENABLED}
    JUCE_ASIO_USE_EXTERNAL_SDK=${GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK}

    # Use WebView2 on Windows
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=${GUITARFX_WEBVIEW2_STATIC_LINKING}
    JUCE_WEBVIEW2_DEVTOOLS_ENABLED=1
    # JUCE disables WebView2 DevTools when JUCE_DEBUG=0; restore prior behavior in release-family Windows builds.
    JUCE_FORCE_DEBUG=1
    GUITARFX_WKWEBVIEW_INLINE_MEDIA_PLAYBACK=${GUITARFX_WKWEBVIEW_INLINE_MEDIA_PLAYBACK}
    GUITARFX_WKWEBVIEW_DISABLE_MEDIA_USER_ACTION=${GUITARFX_WKWEBVIEW_DISABLE_MEDIA_USER_ACTION}


    # Uncomment if you are paying for a an Indie/Pro license or releasing under GPLv3
    # JUCE_DISPLAY_SPLASH_SCREEN=0

    # lets the app known if we're Debug or Release
    CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    VERSION="${CURRENT_VERSION}"

    # JucePlugin_Name is for some reason doesn't use the nicer PRODUCT_NAME
    PRODUCT_NAME_WITHOUT_VERSION="Soundshed Guitar"
)

if(GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK)
    target_include_directories(SharedCode INTERFACE ${_guitarfx_asio_external_include_dirs})
endif()

# Link to any other modules you added (with juce_add_module) here!
# Usually JUCE modules must have PRIVATE visibility
# See https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md#juce_add_module
# However, with Pamplejuce, you'll link modules to SharedCode with INTERFACE visibility
# This allows the JUCE plugin targets and the Tests target to link against it
target_link_libraries(SharedCode
    INTERFACE
    SoundshedGuitarCore
    nlohmann_json::nlohmann_json
    juce_audio_utils
    juce_audio_processors
    juce_dsp
    juce_gui_basics
    juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Link the JUCE plugin targets our SharedCode target
target_link_libraries("${PROJECT_NAME}" PRIVATE SharedCode)

# ── Copy core UI resources next to built artefacts ──────────────────────
# The WebView editor resolves resources from a "resources/ui/" directory
# relative to the executable.  We selectively copy the built UI output from
# core/ui (index.html, dist/, css/, images/, data/, assets/, metronome/)
# into the Standalone artefact directory after every build.
set(_UI_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui")

# ── Build TypeScript UI bundle before copying resources ──────────────────
# Runs "npm run build" in core/ui so dist/ is always fresh.
find_program(_NPM_EXECUTABLE npm)
if(_NPM_EXECUTABLE)
    add_custom_target(SoundshedBuildTypeScript
        COMMAND "${_NPM_EXECUTABLE}" run build
        WORKING_DIRECTORY "${_UI_SRC_DIR}"
        COMMENT "Building TypeScript UI bundle (core/ui)"
    )
else()
    message(WARNING "npm not found – TypeScript UI bundle will not be rebuilt automatically")
endif()

# Helper: create a post-build step that copies the runtime UI bundle for a
# given JUCE format target.
function(soundshed_copy_ui_resources FORMAT_TARGET)
    if(NOT TARGET ${FORMAT_TARGET})
        return()
    endif()

    # Ensure TypeScript is compiled before this target links
    if(TARGET SoundshedBuildTypeScript)
        add_dependencies(${FORMAT_TARGET} SoundshedBuildTypeScript)
    endif()

    if(APPLE)
        set(_dst_base "$<TARGET_BUNDLE_DIR:${FORMAT_TARGET}>/Contents/Resources/ui")
    else()
        set(_dst_base "$<TARGET_FILE_DIR:${FORMAT_TARGET}>/resources/ui")
    endif()

    add_custom_command(TARGET ${FORMAT_TARGET} POST_BUILD
        COMMENT "Copying core UI resources for ${FORMAT_TARGET}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}" "${_dst_base}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_GUITARFX_GENERATED_UI_DIR}/build-flags.js" "${_dst_base}/build-flags.js"
    )

    if(WIN32 AND NOT GUITARFX_WEBVIEW2_STATIC_LINKING AND EXISTS "${GUITARFX_WEBVIEW2_LOADER_DLL}")
        add_custom_command(TARGET ${FORMAT_TARGET} POST_BUILD
            COMMENT "Copying WebView2 loader for ${FORMAT_TARGET}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GUITARFX_WEBVIEW2_LOADER_DLL}" "$<TARGET_FILE_DIR:${FORMAT_TARGET}>/WebView2Loader.dll"
        )
    endif()

endfunction()

soundshed_copy_ui_resources(${PROJECT_NAME}_Standalone)
soundshed_copy_ui_resources(${PROJECT_NAME}_VST3)
soundshed_copy_ui_resources(${PROJECT_NAME}_AU)
soundshed_copy_ui_resources(${PROJECT_NAME}_CLAP)
soundshed_copy_ui_resources(${PROJECT_NAME}_LV2)

# Propagate IPP availability to the JUCE shared code and core DSP library.
if(GUITARFX_IPP_AVAILABLE)
    guitarfx_enable_ipp_for_target(SharedCode)
    guitarfx_enable_ipp_for_target(SoundshedGuitarCore)
endif()

# Everything related to the tests target
if(WIN32)
    enable_testing()

    add_executable(HostedPluginGrapheneStateTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/HostedPluginGrapheneStateTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/JuceHostedPluginEffect.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/JuceHostedPluginEffect.h"
    )

    target_include_directories(HostedPluginGrapheneStateTests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/source"
    )

    target_compile_definitions(HostedPluginGrapheneStateTests
        PRIVATE
            JUCE_PLUGINHOST_VST3=1
            JUCE_VST3_CAN_REPLACE_VST2=0
            GUITARFX_ENABLE_PLUGIN_HOST_TEST_API=1
    )

    target_link_libraries(HostedPluginGrapheneStateTests
        PRIVATE
            SoundshedGuitarCore
            nlohmann_json::nlohmann_json
            juce_audio_utils
            juce_audio_processors
            juce_dsp
            juce_gui_basics
            juce_gui_extra
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )

    set_target_properties(HostedPluginGrapheneStateTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "GuitarFX/Tests"
    )

    add_test(NAME HostedPluginGrapheneStateTests COMMAND HostedPluginGrapheneStateTests)
    set_tests_properties(HostedPluginGrapheneStateTests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(HostedPluginBatchScanTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/HostedPluginBatchScanTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/JuceHostedPluginEffect.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/JuceHostedPluginEffect.h"
    )

    target_include_directories(HostedPluginBatchScanTests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/source"
    )

    target_compile_definitions(HostedPluginBatchScanTests
        PRIVATE
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_LV2=1
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_MODAL_LOOPS_PERMITTED=1
            GUITARFX_ENABLE_PLUGIN_HOST_TEST_API=1
            _CRT_SECURE_NO_WARNINGS
    )

    target_link_libraries(HostedPluginBatchScanTests
        PRIVATE
            SoundshedGuitarCore
            nlohmann_json::nlohmann_json
            juce_audio_utils
            juce_audio_processors
            juce_dsp
            juce_gui_basics
            juce_gui_extra
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )

    set_target_properties(HostedPluginBatchScanTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "GuitarFX/Tests"
    )

    add_test(NAME HostedPluginBatchScanTests COMMAND HostedPluginBatchScanTests)
    set_tests_properties(HostedPluginBatchScanTests PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "pluginscan"
        TIMEOUT 600
    )
endif()

# A separate target for Benchmarks (keeps the Tests target fast)
# include(Benchmarks)

# Output some config for CI (like our PRODUCT_NAME)
include(GitHubENV)
