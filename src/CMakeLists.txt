cmake_minimum_required(VERSION 3.22)

project(GuitarFX LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(GUITARFX_ENABLE_VST3 "Build the VST3 plugin" ON)
option(GUITARFX_ENABLE_AU "Build the AU plugin (macOS only)" ON)
option(GUITARFX_ENABLE_AAX "Build the AAX plugin" OFF)
option(GUITARFX_ENABLE_APP "Build the standalone application" ON)
option(GUITARFX_ENABLE_TOOLS "Build debug and diagnostic tools" ON)
option(GUITARFX_FETCH_DEPENDENCIES "Automatically fetch iPlug2 and NeuralAmpModelerCore" ON)
option(GUITARFX_ENABLE_TESTS "Build the test suite (Debug only)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchDependencies)

add_subdirectory(src)

# Debug tools
if(GUITARFX_ENABLE_TOOLS)
    add_subdirectory(tools/vst3_debug_host)
endif()

# Only build tests in Debug configuration
if(GUITARFX_ENABLE_TESTS AND (CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE))
    enable_testing()
    add_subdirectory(tests)
endif()

# Inno Setup installer target (Windows only)
if(WIN32)
    set(GUITARFX_INSTALLER_SCRIPT "${CMAKE_SOURCE_DIR}/../installer/GuitarFX.iss")
    if(EXISTS "${GUITARFX_INSTALLER_SCRIPT}")
        find_program(GUITARFX_ISCC_EXECUTABLE NAMES iscc ISCC)
        if(GUITARFX_ISCC_EXECUTABLE)
            add_custom_target(GuitarFX_Installer
                COMMAND "${GUITARFX_ISCC_EXECUTABLE}" "${GUITARFX_INSTALLER_SCRIPT}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/../installer"
                COMMENT "Building Inno Setup installer"
            )

            if(TARGET GuitarFX_VST3)
                add_dependencies(GuitarFX_Installer GuitarFX_VST3)
            endif()
            if(TARGET GuitarFX_App)
                add_dependencies(GuitarFX_Installer GuitarFX_App)
            endif()
        else()
            message(WARNING "Inno Setup compiler (ISCC) not found; GuitarFX_Installer target will be unavailable.")
        endif()
    else()
        message(WARNING "Installer script not found at ${GUITARFX_INSTALLER_SCRIPT}; GuitarFX_Installer target will be unavailable.")
    endif()
endif()
