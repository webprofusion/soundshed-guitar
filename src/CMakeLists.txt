cmake_minimum_required(VERSION 3.22)

project(NAMGuitarFX LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(NAMGUITAR_ENABLE_VST3 "Build the VST3 plugin" ON)
option(NAMGUITAR_ENABLE_AU "Build the AU plugin (macOS only)" ON)
option(NAMGUITAR_ENABLE_AAX "Build the AAX plugin" OFF)
option(NAMGUITAR_ENABLE_APP "Build the standalone application" ON)
option(NAMGUITAR_ENABLE_TOOLS "Build debug and diagnostic tools" ON)
option(NAMGUITAR_FETCH_DEPENDENCIES "Automatically fetch iPlug2 and NeuralAmpModelerCore" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchDependencies)

add_subdirectory(src)

# Debug tools
if(NAMGUITAR_ENABLE_TOOLS)
    add_subdirectory(tools/vst3_debug_host)
endif()
