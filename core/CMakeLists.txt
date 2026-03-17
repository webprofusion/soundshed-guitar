# SoundshedGuitarCore — Shared framework-agnostic library
# Consumed by both the iPlug2 and JUCE plugin projects via add_subdirectory().

cmake_minimum_required(VERSION 3.22)

project(SoundshedGuitarCore LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Options — defaults assume the consumer provides these dependencies.
# When building standalone (e.g. for tests) set GUITARFX_CORE_FETCH_DEPS=ON.
# ---------------------------------------------------------------------------
option(GUITARFX_CORE_FETCH_DEPS "Fetch third-party dependencies automatically" OFF)
option(GUITARFX_CORE_BUILD_TESTS "Build core unit tests" OFF)

if(MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("/arch:AVX2" GUITARFX_MSVC_HAS_ARCH_AVX2)
    check_cxx_compiler_flag("/arch:armv8.2" GUITARFX_MSVC_HAS_ARCH_ARMV82)
    check_cxx_compiler_flag("/arch:armv8.1" GUITARFX_MSVC_HAS_ARCH_ARMV81)
endif()

function(guitarfx_apply_release_optimizations target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:/Ox>
            $<$<CONFIG:RelWithDebInfo>:/Ox>
            $<$<CONFIG:Release>:/fp:fast>
            $<$<CONFIG:RelWithDebInfo>:/fp:fast>)
        set(_guitarfx_windows_arch "${CMAKE_GENERATOR_PLATFORM}")
        if(NOT _guitarfx_windows_arch)
            set(_guitarfx_windows_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        string(TOLOWER "${_guitarfx_windows_arch}" _guitarfx_windows_arch)

        if(_guitarfx_windows_arch MATCHES "^(x64|amd64|x86_64)$" AND GUITARFX_MSVC_HAS_ARCH_AVX2)
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:/arch:AVX2>
                $<$<CONFIG:RelWithDebInfo>:/arch:AVX2>)
        elseif(_guitarfx_windows_arch MATCHES "^(arm64|arm64ec|aarch64)$")
            if(GUITARFX_MSVC_HAS_ARCH_ARMV82)
                target_compile_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:/arch:armv8.2>
                    $<$<CONFIG:RelWithDebInfo>:/arch:armv8.2>)
            elseif(GUITARFX_MSVC_HAS_ARCH_ARMV81)
                target_compile_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:/arch:armv8.1>
                    $<$<CONFIG:RelWithDebInfo>:/arch:armv8.1>)
            endif()
        endif()
    elseif(APPLE)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-O3>
            $<$<CONFIG:Release>:-ffast-math>
            $<$<CONFIG:RelWithDebInfo>:-ffast-math>)

        set(_guitarfx_osx_arches "${CMAKE_OSX_ARCHITECTURES}")
        if(NOT _guitarfx_osx_arches)
            set(_guitarfx_osx_arches "${CMAKE_SYSTEM_PROCESSOR}")
        endif()

        if(_guitarfx_osx_arches MATCHES "x86_64")
            if(_guitarfx_osx_arches MATCHES "arm64")
                target_compile_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:SHELL:-Xarch_x86_64 -mavx2>
                    $<$<CONFIG:RelWithDebInfo>:SHELL:-Xarch_x86_64 -mavx2>)
            else()
                target_compile_options(${target_name} PRIVATE
                    $<$<CONFIG:Release>:-mavx2>
                    $<$<CONFIG:RelWithDebInfo>:-mavx2>)
            endif()
        endif()
    else()
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-O3>
            $<$<CONFIG:Release>:-ffast-math>
            $<$<CONFIG:RelWithDebInfo>:-ffast-math>)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:-mavx2>
                $<$<CONFIG:RelWithDebInfo>:-mavx2>)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# nlohmann/json — consumer usually provides this
if(NOT TARGET nlohmann_json::nlohmann_json AND GUITARFX_CORE_FETCH_DEPS)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Signalsmith headers — consumer may set these variables before add_subdirectory()
if(NOT DEFINED signalsmith_linear_SOURCE_DIR AND GUITARFX_CORE_FETCH_DEPS)
    include(FetchContent)
    FetchContent_Declare(signalsmith_linear
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
        GIT_TAG main)
    FetchContent_MakeAvailable(signalsmith_linear)
endif()

if(NOT DEFINED signalsmith_stretch_SOURCE_DIR AND GUITARFX_CORE_FETCH_DEPS)
    include(FetchContent)
    FetchContent_Declare(signalsmith_stretch
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
        GIT_TAG main)
    FetchContent_MakeAvailable(signalsmith_stretch)
endif()

if(NOT DEFINED stftpitchshift_SOURCE_DIR AND GUITARFX_CORE_FETCH_DEPS)
    include(FetchContent)
    FetchContent_Declare(stftpitchshift
        GIT_REPOSITORY https://github.com/jurihock/stftPitchShift.git
        GIT_TAG main)
    FetchContent_MakeAvailable(stftpitchshift)
endif()

# miniz — required for startup factory preset archive support (.preset/.presets)
if(NOT TARGET guitarfx_miniz)
    include(FetchContent)
    FetchContent_Declare(guitarfx_miniz_src
        GIT_REPOSITORY https://github.com/richgel999/miniz.git
        GIT_TAG 3.0.2)
    FetchContent_GetProperties(guitarfx_miniz_src)
    if(NOT guitarfx_miniz_src_POPULATED)
        FetchContent_Populate(guitarfx_miniz_src)
    endif()

    file(WRITE "${guitarfx_miniz_src_SOURCE_DIR}/miniz_export.h" "#ifndef MINIZ_EXPORT\n#define MINIZ_EXPORT\n#endif\n")

    add_library(guitarfx_miniz STATIC
        "${guitarfx_miniz_src_SOURCE_DIR}/miniz.c"
        "${guitarfx_miniz_src_SOURCE_DIR}/miniz_tdef.c"
        "${guitarfx_miniz_src_SOURCE_DIR}/miniz_tinfl.c"
        "${guitarfx_miniz_src_SOURCE_DIR}/miniz_zip.c")
    target_include_directories(guitarfx_miniz PUBLIC
        "${guitarfx_miniz_src_SOURCE_DIR}")
endif()

# NeuralAmpModelerCore + Eigen (used by NAM effects)
if(GUITARFX_CORE_FETCH_DEPS AND NOT TARGET NeuralAmpModelerCore AND NOT TARGET nam)
    include(FetchContent)
    set(NAM_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NAM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(NeuralAmpModelerCore
        GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
        GIT_TAG v0.4.0.rc3
        CMAKE_ARGS -DNAM_BUILD_TOOLS=OFF -DNAM_BUILD_TESTS=OFF)
    FetchContent_MakeAvailable(NeuralAmpModelerCore)
    FetchContent_GetProperties(NeuralAmpModelerCore SOURCE_DIR NAM_SOURCE_DIR BINARY_DIR NAM_BINARY_DIR)
    if(NOT DEFINED NeuralAmpModelerCore_SOURCE_DIR)
        set(NeuralAmpModelerCore_SOURCE_DIR "${NAM_SOURCE_DIR}" CACHE PATH "" FORCE)
    endif()

    if(MSVC)
        # neuralampmodelercore/tools/CMakeLists.txt sets -Wno-error on NAM sources as a
        # GCC/Clang workaround for an Eigen warning. That flag is invalid on MSVC (D8021).
        # Clear it from the tools directory scope (requires CMake >= 3.18).
        if(DEFINED NeuralAmpModelerCore_SOURCE_DIR)
            foreach(_src
                    "${NeuralAmpModelerCore_SOURCE_DIR}/NAM/dsp.cpp"
                    "${NeuralAmpModelerCore_SOURCE_DIR}/NAM/conv1d.cpp")
                if(EXISTS "${_src}")
                    set_source_files_properties("${_src}"
                        DIRECTORY "${NeuralAmpModelerCore_SOURCE_DIR}/tools"
                        PROPERTIES COMPILE_FLAGS "")
                endif()
            endforeach()
        endif()
        # Exclude all NAM tool executables from the default build on MSVC.
        # 'render' was added upstream after the original list was written.
        foreach(_nam_tool benchmodel loadmodel render run_tests tools)
            if(TARGET ${_nam_tool})
                set_property(TARGET ${_nam_tool} PROPERTY EXCLUDE_FROM_ALL TRUE)
                set_property(TARGET ${_nam_tool} PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
            endif()
        endforeach()
    endif()

    if(DEFINED NeuralAmpModelerCore_SOURCE_DIR)
        set(_nam_eigen_dest "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen")
        if(NOT EXISTS "${_nam_eigen_dest}/Eigen")
            FetchContent_Declare(eigen
                GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
                GIT_TAG 3.4.0)
            FetchContent_MakeAvailable(eigen)
            if(EXISTS "${eigen_SOURCE_DIR}/Eigen")
                file(MAKE_DIRECTORY "${_nam_eigen_dest}")
                file(COPY "${eigen_SOURCE_DIR}/Eigen" "${eigen_SOURCE_DIR}/unsupported" DESTINATION "${_nam_eigen_dest}")
            endif()
        endif()
    endif()

    if(NOT TARGET NeuralAmpModelerCore AND DEFINED NeuralAmpModelerCore_SOURCE_DIR)
        file(GLOB NAM_SOURCES CONFIGURE_DEPENDS "${NeuralAmpModelerCore_SOURCE_DIR}/NAM/*.cpp")
        add_library(NeuralAmpModelerCore STATIC ${NAM_SOURCES})
        target_compile_features(NeuralAmpModelerCore PUBLIC cxx_std_17)
        guitarfx_apply_release_optimizations(NeuralAmpModelerCore)
        target_include_directories(NeuralAmpModelerCore PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src/compat"
            "${NeuralAmpModelerCore_SOURCE_DIR}"
            "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen")
        if(TARGET nlohmann_json::nlohmann_json)
            target_link_libraries(NeuralAmpModelerCore PUBLIC nlohmann_json::nlohmann_json)
        else()
            target_include_directories(NeuralAmpModelerCore PUBLIC
                "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/nlohmann")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(GUITARFX_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

set(GUITARFX_CORE_SOURCES
    # Host interface & controller
    src/PluginController.h
    src/PluginController.cpp
    src/MessageDispatcher.h
    src/MessageDispatcher.cpp
    src/IPluginHost.h
    src/UiBridge.h
    src/UiBridge.cpp
    src/dispatcher/MessageDispatchState.cpp
    src/dispatcher/MessageDispatchSettings.cpp
    src/dispatcher/MessageDispatchParameters.cpp
    src/dispatcher/MessageDispatchPresetsResources.cpp
    src/dispatcher/MessageDispatchSignalPath.cpp
    src/dispatcher/MessageDispatchMixer.cpp
    src/dispatcher/MessageDispatchLibraryComposite.cpp
    src/controller/DemoPreviewService.h
    src/controller/DemoPreviewService.cpp

    # DSP core
    src/dsp/EffectProcessor.h
    src/dsp/EffectRegistry.h
    src/dsp/EffectRegistry.cpp
    src/dsp/SignalGraphExecutor.h
    src/dsp/SignalGraphExecutor.cpp
    src/dsp/MultiPresetMixer.h
    src/dsp/MultiPresetMixer.cpp
    src/dsp/RealtimeConvolver.h
    src/dsp/RealtimeConvolver.cpp
    src/dsp/SimdFFT.h
    src/dsp/IRTypes.h
    src/dsp/IRWavLoader.h
    src/dsp/NAMFactoryForce.cpp

    # SIMD optimizations
    src/dsp/simd/SimdMath.h
    src/dsp/simd/OptimizedActivations.h
    src/dsp/simd/OptimizedWaveNet.h
    src/dsp/simd/OptimizedLSTM.h
    src/dsp/simd/OptimizedNAM.h
    src/dsp/simd/Benchmark.h

    # Effect processors
    src/dsp/effects/BuiltinEffects.h
    src/dsp/effects/GainEffect.h
    src/dsp/effects/NoiseGateEffect.h
    src/dsp/effects/ParametricEQEffect.h
    src/dsp/effects/DelayEffect.h
    src/dsp/effects/ReverbEffect.h
    src/dsp/effects/SpringReverbEffect.h
    src/dsp/effects/CompressorEffect.h
    src/dsp/effects/LimiterEffect.h
    src/dsp/effects/NAMAmpEffect.h
    src/dsp/effects/OptimizedNAMAmpEffect.h
    src/dsp/effects/MultiModelNAMAmpEffect.h
    src/dsp/effects/IRCabEffect.h
    src/dsp/effects/IRReverbEffect.h
    src/dsp/effects/SimpleCabEffect.h
    src/dsp/effects/OverdriveEffect.h
    src/dsp/effects/DistortionEffect.h
    src/dsp/effects/FuzzEffect.h
    src/dsp/effects/BuiltinAmpEffect.h
    src/dsp/effects/ChorusEffect.h
    src/dsp/effects/FlangerEffect.h
    src/dsp/effects/PhaserEffect.h
    src/dsp/effects/TremoloEffect.h
    src/dsp/effects/AutoWahEffect.h
    src/dsp/effects/OctaveEffect.h
    src/dsp/effects/PitchShiftEffect.h
    src/dsp/effects/StftTransposeEffect.h
    src/dsp/effects/TransposeEffect.h
    src/dsp/effects/DoublerEffect.h
    src/dsp/effects/SynthSawEffect.h
    src/dsp/effects/MixerEffect.h
    src/dsp/effects/CompositeEffectProcessor.h
    src/dsp/effects/CompositeEffectProcessor.cpp

    # Presets
    src/presets/PresetTypes.h
    src/presets/PresetTypes.cpp
    src/presets/PresetTypesJson.h
    src/presets/PresetStorage.h
    src/presets/PresetStorage.cpp
    src/presets/CompositePresetTypes.h
    src/presets/CompositePresetStorage.h
    src/presets/CompositePresetStorage.cpp

    # Resources
    src/resources/ResourceLibrary.h
    src/resources/ResourceLibrary.cpp

    # Models
    src/models/ModelHasher.h
    src/models/ModelHasher.cpp

    # Utilities
    src/util/Base64.h
    src/util/Base64.cpp
    src/util/FileIO.h
    src/util/FileIO.cpp
    src/util/FileSystem.h
    src/util/FileSystem.cpp
    src/util/PathSanitizer.h
    src/util/PathSanitizer.cpp
    src/util/Wav.h
    src/util/Wav.cpp
)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(SoundshedGuitarCore STATIC ${GUITARFX_CORE_SOURCES})

# Build UI bundle from core/ui with npm when available.
# Uses a stamp file so the TS build only reruns when sources change.
find_program(NPM_EXECUTABLE npm)
if(NPM_EXECUTABLE)
    file(GLOB_RECURSE _UI_TS_SOURCES CONFIGURE_DEPENDS
        "${GUITARFX_CORE_ROOT}/ui/ts/*.ts"
        "${GUITARFX_CORE_ROOT}/ui/tsconfig.json"
        "${GUITARFX_CORE_ROOT}/ui/package.json")
    set(_UI_STAMP "${CMAKE_CURRENT_BINARY_DIR}/ui_build.stamp")

    # On Windows, npm is typically a .cmd shim. When invoked from another batch
    # context (MSBuild CustomBuild), it must be called via `call` or the
    # remaining commands (stamp touch) will not execute.
    if(WIN32)
        set(_UI_BUILD_CMD cmd /c call "${NPM_EXECUTABLE}" run build)
    else()
        set(_UI_BUILD_CMD "${NPM_EXECUTABLE}" run build)
    endif()

    add_custom_command(
        OUTPUT "${_UI_STAMP}"
        COMMAND ${_UI_BUILD_CMD}
        COMMAND ${CMAKE_COMMAND} -E touch "${_UI_STAMP}"
        WORKING_DIRECTORY "${GUITARFX_CORE_ROOT}/ui"
        DEPENDS ${_UI_TS_SOURCES}
        COMMENT "Building core UI TypeScript bundle")
    add_custom_target(SoundshedGuitarCoreUiBuild DEPENDS "${_UI_STAMP}")
    add_dependencies(SoundshedGuitarCore SoundshedGuitarCoreUiBuild)
else()
    message(WARNING "npm not found; core UI TypeScript build will be skipped.")
endif()

target_include_directories(SoundshedGuitarCore
    PUBLIC
        "${GUITARFX_CORE_ROOT}/src"
)

# Framework-agnostic — no iPlug2 or JUCE includes here
target_compile_definitions(SoundshedGuitarCore
    PUBLIC
        NOMINMAX
)

# Link dependencies
target_link_libraries(SoundshedGuitarCore
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        guitarfx_miniz
)

# NeuralAmpModelerCore — consumer must provide this target or set include dirs
if(TARGET NeuralAmpModelerCore)
    target_link_libraries(SoundshedGuitarCore PUBLIC NeuralAmpModelerCore)
elseif(TARGET nam)
    target_link_libraries(SoundshedGuitarCore PUBLIC nam)
endif()

if(DEFINED NeuralAmpModelerCore_SOURCE_DIR)
    target_include_directories(SoundshedGuitarCore PUBLIC
        "${NeuralAmpModelerCore_SOURCE_DIR}"
        "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen"
    )
    if(NOT TARGET nlohmann_json::nlohmann_json)
        target_include_directories(SoundshedGuitarCore PUBLIC
            "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/nlohmann"
        )
    endif()
endif()

# Signalsmith headers
if(TARGET signalsmith_linear::headers)
    target_link_libraries(SoundshedGuitarCore PUBLIC signalsmith_linear::headers)
elseif(DEFINED signalsmith_linear_SOURCE_DIR)
    target_include_directories(SoundshedGuitarCore PUBLIC "${signalsmith_linear_SOURCE_DIR}/include")
endif()

if(TARGET signalsmith_stretch::headers)
    target_link_libraries(SoundshedGuitarCore PUBLIC signalsmith_stretch::headers)
elseif(DEFINED signalsmith_stretch_SOURCE_DIR)
    target_include_directories(SoundshedGuitarCore PUBLIC "${signalsmith_stretch_SOURCE_DIR}")
endif()

if(DEFINED stftpitchshift_SOURCE_DIR)
    target_include_directories(SoundshedGuitarCore PUBLIC "${stftpitchshift_SOURCE_DIR}/cpp")
else()
    message(FATAL_ERROR "stftPitchShift sources are required for transpose_stft. Set stftpitchshift_SOURCE_DIR or enable GUITARFX_CORE_FETCH_DEPS.")
endif()

# Platform flags
if(WIN32)
    target_compile_definitions(SoundshedGuitarCore PUBLIC _WIN32 WIN32 _WINDOWS)
    target_compile_options(SoundshedGuitarCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
endif()

# Release optimization flags — architecture-aware and safe for universal macOS builds
guitarfx_apply_release_optimizations(SoundshedGuitarCore)

set_target_properties(SoundshedGuitarCore PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "GuitarFX"
)

# Precompiled header — speeds up parsing of heavy template libraries
target_precompile_headers(SoundshedGuitarCore PRIVATE
    <string>
    <vector>
    <memory>
    <algorithm>
    <unordered_map>
    <functional>
    <cmath>
    <nlohmann/json.hpp>
)

# ---------------------------------------------------------------------------
# Tests (optional)
# ---------------------------------------------------------------------------
if(GUITARFX_CORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
