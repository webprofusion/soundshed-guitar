# ── Nano Q's UI in Soundshed Guitar (experimental) ──────────────────────────────
# With SOUNDSHED_NANOQ_UI (on by default for every desktop build, juce/CMakeLists.txt) the JUCE
# app can draw itself with Nano Q's native UI (nanoq/src/ui, drawn with Cycfi's Elements) beside
# the WebView editor: source/NanoQUiEditor.cpp hosts it in a child window, and the
# "ui.nativeEditor" setting picks which editor PluginProcessorAdapter::createEditor builds. The UI
# sources are Nano Q's own, compiled here; nothing of QPlug comes along.
#
# Elements' host per platform: Win32 with Direct2D, Cocoa with Quartz, and on Linux X11 with
# Cairo (x11, xcursor, xrandr, cairo, cairo-xlib, cairo-ft, fontconfig, harfbuzz and libwebp from
# pkg-config). Not on iOS or Android, whose apps stay on the WebView.

if(SSG_MOBILE)
    message(FATAL_ERROR "SOUNDSHED_NANOQ_UI: desktop builds only (iOS and Android stay on the WebView)")
endif()

include(FetchContent)
set(_nanoq_dir "${CMAKE_CURRENT_SOURCE_DIR}/../nanoq")

# Elements at Nano Q's pin, with Nano Q's desktop patches, in its order (nanoq/CMakeLists.txt):
# fewer whole-window redraws, macOS's 10 ms view tick, and X11 drawing in this process. Each
# binary compiles Elements' host layer itself (ELEMENTS_HOST_IN_CONSUMER), as QPlug does, so its
# window class carries this product's name.
set(_nanoq_apply_patch -P ${_nanoq_dir}/ios/cmake/ApplyPatch.cmake)
FetchContent_Declare(cycfi_elements
    GIT_REPOSITORY https://github.com/cycfi/elements.git
    GIT_TAG fa180799a4c80d4cd70850c700df766f62917de7 # v1.0.2
    GIT_SUBMODULES lib/artist
    GIT_SUBMODULES_RECURSE OFF
    SOURCE_SUBDIR do-not-add
    PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${_nanoq_dir}/patches/elements-input-redraws.patch ${_nanoq_apply_patch}
        COMMAND ${CMAKE_COMMAND} -DPATCH=${_nanoq_dir}/patches/elements-macos-tick.patch ${_nanoq_apply_patch}
        COMMAND ${CMAKE_COMMAND} -DPATCH=${_nanoq_dir}/patches/elements-x11-present.patch ${_nanoq_apply_patch}
        COMMAND ${CMAKE_COMMAND} -DPATCH=${_nanoq_dir}/patches/elements-x11-skia.patch ${_nanoq_apply_patch})
FetchContent_MakeAvailable(cycfi_elements)

set(ELEMENTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ELEMENTS_HOST_IN_CONSUMER ON CACHE BOOL "" FORCE)
if(WIN32)
    set(ELEMENTS_HOST_UI_LIBRARY "win32" CACHE STRING "" FORCE)
elseif(APPLE)
    set(ELEMENTS_HOST_UI_LIBRARY "cocoa" CACHE STRING "" FORCE)
else()
    # A child of the plugin host's X11 window, as QPlug's Linux view is.
    set(ELEMENTS_HOST_UI_LIBRARY "x11" CACHE STRING "" FORCE)
endif()
add_subdirectory("${cycfi_elements_SOURCE_DIR}" "${cycfi_elements_BINARY_DIR}" EXCLUDE_FROM_ALL)

# The UI and the platform helpers it uses (the file picker's folder listing, paths, the log, the
# Jam tab's web view, the caption colour). Nano Q's engine, QPlug glue and dialogs stay out: the
# JUCE adapter is the engine's host.
file(GLOB_RECURSE _nanoq_ui_sources CONFIGURE_DEPENDS "${_nanoq_dir}/src/ui/*.cpp")
add_library(SoundshedNanoQUi STATIC
    ${_nanoq_ui_sources}
    "${_nanoq_dir}/src/FileBrowsing.cpp"
    "${_nanoq_dir}/src/Log.cpp"
    "${_nanoq_dir}/src/Paths.cpp"
    "${_nanoq_dir}/src/WebView.cpp"
    "${_nanoq_dir}/src/WindowChrome.cpp"
    ${ELEMENTS_HOST_SOURCES})
if(APPLE)
    # EmbeddedUi's Cocoa side; EmbeddedUi.cpp is empty there.
    target_sources(SoundshedNanoQUi PRIVATE "${_nanoq_dir}/src/ui/EmbeddedUiCocoa.mm")
    set_source_files_properties("${_nanoq_dir}/src/ui/EmbeddedUiCocoa.mm"
        TARGET_DIRECTORY SoundshedNanoQUi PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(SoundshedNanoQUi PRIVATE "-framework Cocoa")
endif()
target_include_directories(SoundshedNanoQUi PUBLIC "${_nanoq_dir}/src")
target_compile_definitions(SoundshedNanoQUi PRIVATE
    ELEMENTS_CLASS_PREFIX=SoundshedGuitar
    NANOQ_NAME="Soundshed Guitar"
    NANOQ_ID="${BUNDLE_ID}"
    NANOQ_VERSION="${CURRENT_VERSION}")
target_link_libraries(SoundshedNanoQUi PRIVATE elements SoundshedGuitarCore SoundshedUiClient)
set_target_properties(SoundshedNanoQUi PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# As Nano Q: Elements and ui/shell/Shell.h precompiled, for the sources that include Shell.h.
target_precompile_headers(SoundshedNanoQUi PRIVATE <elements.hpp> "${_nanoq_dir}/src/ui/shell/Shell.h")
foreach(_src IN LISTS _nanoq_ui_sources)
    file(STRINGS "${_src}" _uses_shell REGEX "^#include \"ui/shell/Shell.h\"")
    if(NOT _uses_shell)
        set_source_files_properties("${_src}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()
set_source_files_properties(
    "${_nanoq_dir}/src/FileBrowsing.cpp" "${_nanoq_dir}/src/Log.cpp" "${_nanoq_dir}/src/Paths.cpp" "${_nanoq_dir}/src/WebView.cpp"
    "${_nanoq_dir}/src/WindowChrome.cpp" ${ELEMENTS_HOST_SOURCES}
    TARGET_DIRECTORY SoundshedNanoQUi
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_sources(SharedCode INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/source/NanoQUiEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/NanoQUiEditor.cpp")
target_compile_definitions(SharedCode INTERFACE SOUNDSHED_NANOQ_UI=1)
target_link_libraries(SharedCode INTERFACE SoundshedNanoQUi)

# The fonts the UI draws with, where Elements looks for them: "resources" beside the binary,
# which is where the WebView build already keeps resources/ui; on macOS the bundle's Resources,
# which Elements also activates the fonts from.
file(GLOB _nanoq_ui_font_cuts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../core/nativeui/fonts/*.ttf")
set(_nanoq_ui_fonts
    ${_nanoq_ui_font_cuts}
    "${cycfi_elements_SOURCE_DIR}/resources/fonts/OpenSans-Regular.ttf"
    "${cycfi_elements_SOURCE_DIR}/resources/fonts/elements_basic.ttf")
foreach(_format Standalone VST3 CLAP AU AAX LV2)
    set(_target ${PROJECT_NAME}_${_format})
    if(NOT TARGET ${_target})
        continue()
    endif()
    if(APPLE)
        set(_font_dir "$<TARGET_BUNDLE_DIR:${_target}>/Contents/Resources")
    else()
        set(_font_dir "$<TARGET_FILE_DIR:${_target}>/resources")
    endif()
    set(_commands COMMAND ${CMAKE_COMMAND} -E make_directory "${_font_dir}")
    foreach(_font IN LISTS _nanoq_ui_fonts)
        list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_font}" "${_font_dir}")
    endforeach()
    add_custom_command(TARGET ${_target} POST_BUILD ${_commands}
        COMMENT "Copying Nano Q UI fonts into ${_target}" VERBATIM)
endforeach()
