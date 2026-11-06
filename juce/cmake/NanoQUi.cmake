# ── Nano Q's UI in Soundshed Guitar (spike) ─────────────────────────────────────
# With -DSOUNDSHED_NANOQ_UI=ON the JUCE app's editor is Nano Q's native UI (nanoq/src/ui, drawn
# with Cycfi's Elements) instead of the WebView: source/NanoQUiEditor.cpp hosts it in a child
# window. The UI sources are Nano Q's own, compiled here; nothing of QPlug comes along.
# Windows only. Use a build tree of its own, so the WebView build is untouched:
#   cmake -G "Visual Studio 18 2026" -A x64 -S juce -B juce/builds-nanoq-ui -DSOUNDSHED_NANOQ_UI=ON
#   cmake --build juce/builds-nanoq-ui --config Release --target SoundshedGuitar_Standalone

if(NOT WIN32)
    message(FATAL_ERROR "SOUNDSHED_NANOQ_UI: Windows only so far")
endif()

include(FetchContent)
set(_nanoq_dir "${CMAKE_CURRENT_SOURCE_DIR}/../nanoq")

# Elements at Nano Q's pin, with Nano Q's input-redraws patch (nanoq/CMakeLists.txt). Each
# binary compiles Elements' host layer itself (ELEMENTS_HOST_IN_CONSUMER), as QPlug does, so its
# window class carries this product's name.
FetchContent_Declare(cycfi_elements
    GIT_REPOSITORY https://github.com/cycfi/elements.git
    GIT_TAG fa180799a4c80d4cd70850c700df766f62917de7 # v1.0.2
    GIT_SUBMODULES lib/artist
    GIT_SUBMODULES_RECURSE OFF
    SOURCE_SUBDIR do-not-add
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DPATCH=${_nanoq_dir}/patches/elements-input-redraws.patch
        -P ${_nanoq_dir}/ios/cmake/ApplyPatch.cmake)
FetchContent_MakeAvailable(cycfi_elements)

set(ELEMENTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ELEMENTS_HOST_IN_CONSUMER ON CACHE BOOL "" FORCE)
add_subdirectory("${cycfi_elements_SOURCE_DIR}" "${cycfi_elements_BINARY_DIR}" EXCLUDE_FROM_ALL)

# The UI and the platform helpers it uses (paths, the log, the Jam tab's web view, the caption
# colour). Nano Q's engine, QPlug glue and dialogs stay out: the JUCE adapter is the engine's host.
file(GLOB_RECURSE _nanoq_ui_sources CONFIGURE_DEPENDS "${_nanoq_dir}/src/ui/*.cpp")
add_library(SoundshedNanoQUi STATIC
    ${_nanoq_ui_sources}
    "${_nanoq_dir}/src/Log.cpp"
    "${_nanoq_dir}/src/Paths.cpp"
    "${_nanoq_dir}/src/WebView.cpp"
    "${_nanoq_dir}/src/WindowChrome.cpp"
    ${ELEMENTS_HOST_SOURCES})
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
    "${_nanoq_dir}/src/Log.cpp" "${_nanoq_dir}/src/Paths.cpp" "${_nanoq_dir}/src/WebView.cpp"
    "${_nanoq_dir}/src/WindowChrome.cpp" ${ELEMENTS_HOST_SOURCES}
    TARGET_DIRECTORY SoundshedNanoQUi
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_sources(SharedCode INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/source/NanoQUiEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/NanoQUiEditor.cpp")
target_compile_definitions(SharedCode INTERFACE SOUNDSHED_NANOQ_UI=1)
target_link_libraries(SharedCode INTERFACE SoundshedNanoQUi)

# The fonts the UI draws with, where Elements looks for them: "resources" beside the binary,
# which is where the WebView build already keeps resources/ui.
set(_nanoq_ui_fonts
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/nativeui/fonts/Inter-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/nativeui/fonts/Inter-Medium.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/nativeui/fonts/Inter-SemiBold.ttf"
    "${cycfi_elements_SOURCE_DIR}/resources/fonts/OpenSans-Regular.ttf"
    "${cycfi_elements_SOURCE_DIR}/resources/fonts/elements_basic.ttf")
foreach(_format Standalone VST3 CLAP)
    set(_target ${PROJECT_NAME}_${_format})
    if(NOT TARGET ${_target})
        continue()
    endif()
    set(_commands COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_target}>/resources")
    foreach(_font IN LISTS _nanoq_ui_fonts)
        list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_font}" "$<TARGET_FILE_DIR:${_target}>/resources")
    endforeach()
    add_custom_command(TARGET ${_target} POST_BUILD ${_commands}
        COMMENT "Copying Nano Q UI fonts into ${_target}" VERBATIM)
endforeach()
