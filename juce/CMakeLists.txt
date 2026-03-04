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
# Valid choices: AAX Unity VST VST3 AU AUv3 Standalone
set(FORMATS Standalone AU VST3)

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
    CPMAddPackage(
        NAME intelipp_static_win_x64
        URL https://www.nuget.org/api/v2/package/intelipp.static.win-x64/2022.3.1.8
    )
endif()

# Signalsmith headers are provided transitively by SoundshedGuitarCore.

# Configures universal binaries and decides which version of macOS to support
include(PamplejuceMacOS)

# Shared core library (framework-agnostic DSP, presets, resources, controller)
set(GUITARFX_CORE_FETCH_DEPS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core
                 ${CMAKE_BINARY_DIR}/core)

# Couple tweaks that IMO should be JUCE defaults
include(JUCEDefaults)

# WebView2 static SDK package location (NuGet packages root)
if(WIN32)
    set(GUITARFX_WEBVIEW2_VERSION "1.0.3485.44" CACHE STRING "WebView2 NuGet package version")
    option(GUITARFX_AUTO_INSTALL_WEBVIEW2 "Auto-download WebView2 NuGet package when missing" ON)

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

    set(GUITARFX_ASIO_SDK_DIR "" CACHE PATH "Path to Steinberg ASIO SDK root folder (ASIOSDK)")
    if(GUITARFX_ASIO_SDK_DIR STREQUAL "" AND DEFINED ENV{ASIO_SDK_DIR} AND NOT "$ENV{ASIO_SDK_DIR}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{ASIO_SDK_DIR}" _asio_sdk_dir_from_env)
        set(GUITARFX_ASIO_SDK_DIR "${_asio_sdk_dir_from_env}" CACHE PATH "Path to Steinberg ASIO SDK root folder (ASIOSDK)" FORCE)
    endif()

    if(NOT GUITARFX_ASIO_SDK_DIR STREQUAL "" AND EXISTS "${GUITARFX_ASIO_SDK_DIR}")
        set(GUITARFX_JUCE_ASIO_ENABLED 1)
    else()
        set(GUITARFX_JUCE_ASIO_ENABLED 0)
    endif()
else()
    set(GUITARFX_JUCE_ASIO_ENABLED 0)
endif()

# Force JUCE to work with sccache
# instruct MSVC to embed debug info (/Z7) instead of emitting a .pdb
set(CMAKE_POLICY_DEFAULT_CMP0141        NEW      CACHE STRING "" FORCE)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE STRING "" FORCE)

# JUCE is setup as a submodule in the /JUCE folder
# Locally, you must run `git submodule update --init --recursive` once
# and later `git submodule update --remote --merge` to keep it up to date
# On Github Actions, this is done as a part of actions/checkout
add_subdirectory(JUCE)

# Add CLAP format
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

    # On MacOS, plugin is copied to /Users/yourname/Library/Audio/Plug-Ins/
    COPY_PLUGIN_AFTER_BUILD TRUE

    # Change me!
    # A four-character plugin id
    # First character MUST be uppercase for AU formats
    PLUGIN_MANUFACTURER_CODE Pamp

    # Change me!
    # A unique four-character plugin id
    # Note: this must have at least one upper-case character
    PLUGIN_CODE P001
    FORMATS "${FORMATS}"
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE

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

    # JUCE_WEB_BROWSER and JUCE_USE_CURL off by default
    JUCE_WEB_BROWSER=1  # If you set this to 1, add `NEEDS_WEB_BROWSER TRUE` to the `juce_add_plugin` call
    JUCE_USE_CURL=0     # If you set this to 1, add `NEEDS_CURL TRUE` to the `juce_add_plugin` call
    JUCE_VST3_CAN_REPLACE_VST2=0

    # Enable ASIO on Windows (requires Steinberg ASIO SDK)
    JUCE_ASIO=${GUITARFX_JUCE_ASIO_ENABLED}

    # Use WebView2 on Windows
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_WEBVIEW2_DEVTOOLS_ENABLED=1

    # Uncomment if you are paying for a an Indie/Pro license or releasing under GPLv3
    # JUCE_DISPLAY_SPLASH_SCREEN=0

    # lets the app known if we're Debug or Release
    CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    VERSION="${CURRENT_VERSION}"

    # JucePlugin_Name is for some reason doesn't use the nicer PRODUCT_NAME
    PRODUCT_NAME_WITHOUT_VERSION="Soundshed Guitar"
)

if(GUITARFX_JUCE_ASIO_ENABLED)
    target_compile_definitions(SharedCode
        INTERFACE
        JUCE_ASIO_PATH="${GUITARFX_ASIO_SDK_DIR}"
    )
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
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst_base}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_UI_SRC_DIR}/index.html" "${_dst_base}/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/dist"      "${_dst_base}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/css"       "${_dst_base}/css"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/images"    "${_dst_base}/images"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/data"      "${_dst_base}/data"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/assets"    "${_dst_base}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/metronome" "${_dst_base}/metronome"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/demo"      "${_dst_base}/demo"
    )
endfunction()

soundshed_copy_ui_resources(${PROJECT_NAME}_Standalone)
soundshed_copy_ui_resources(${PROJECT_NAME}_VST3)

# IPP support, comment out to disable
include(PamplejuceIPP)

# Propagate IPP availability to the core DSP library so it can use IPP code paths
if(DEFINED IPP_ROOT AND IS_DIRECTORY "${IPP_ROOT}")
    target_compile_definitions(SoundshedGuitarCore PUBLIC PAMPLEJUCE_IPP=1)
    target_include_directories(SoundshedGuitarCore PUBLIC "${IPP_INC}")
endif()

# Everything related to the tests target
# include(Tests)

# A separate target for Benchmarks (keeps the Tests target fast)
# include(Benchmarks)

# Output some config for CI (like our PRODUCT_NAME)
include(GitHubENV)
