# GuitarFXCore static library containing the shared plugin implementation.
# Format-specific targets in the platform subdirectory link against this library.

# NeuralAmpModelerCore sources - compiled directly since the upstream project
# doesn't export a library target
set(_nam_dsp_sources
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/activations.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/conv1d.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/convnet.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/dsp.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/get_dsp.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/lstm.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/ring_buffer.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/util.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/wavenet.cpp
)

# Fix for upstream NAM bug: activations.h uses assert() without including <cassert>
# This causes build failures in Release builds. We force include cassert for NAM sources.
if(MSVC)
  set_source_files_properties(${_nam_dsp_sources} PROPERTIES
    COMPILE_FLAGS "/FI\"cassert\""
  )
else()
  set_source_files_properties(${_nam_dsp_sources} PROPERTIES
    COMPILE_FLAGS "-include cassert"
  )
endif()

# GuitarFXCore contains API-agnostic shared code. GuitarFXPlugin.cpp 
# and GuitarFXPlugin.h are NOT included here because they require format-
# specific API definitions (APP_API, VST3_API, etc.). Each format target
# compiles GuitarFXPlugin.cpp directly with its own API definition.

set(_guitarfx_core_sources
  # Core DSP utilities
  dsp/SimdFFT.h
  dsp/RealtimeConvolver.cpp
  dsp/RealtimeConvolver.h
  dsp/IRTypes.h
  dsp/NAMFactoryForce.cpp
  
  # V2 Signal Graph DSP
  dsp/EffectProcessor.h
  dsp/EffectRegistry.h
  dsp/EffectRegistry.cpp
  dsp/SignalGraphExecutor.h
  dsp/SignalGraphExecutor.cpp
  dsp/MultiPresetMixer.h
  dsp/MultiPresetMixer.cpp
  
  # SIMD-optimized DSP utilities
  dsp/simd/SimdMath.h
  dsp/simd/OptimizedActivations.h
  dsp/simd/OptimizedWaveNet.h
  dsp/simd/OptimizedLSTM.h
  dsp/simd/OptimizedNAM.h
  dsp/simd/Benchmark.h
  
  # Effect implementations
  dsp/effects/BuiltinEffects.h
  dsp/effects/GainEffect.h
  dsp/effects/NoiseGateEffect.h
  dsp/effects/ParametricEQEffect.h
  dsp/effects/DelayEffect.h
  dsp/effects/ReverbEffect.h
  dsp/effects/CompressorEffect.h
  dsp/effects/NAMAmpEffect.h
  dsp/effects/OptimizedNAMAmpEffect.h
  dsp/effects/IRCabEffect.h
  dsp/effects/SimpleCabEffect.h
  
  # Models
  models/ModelHasher.cpp
  models/ModelHasher.h
  
  # Network
  network/PresetServiceClient.cpp
  network/PresetServiceClient.h
  
  # Presets
  presets/PresetStorage.cpp
  presets/PresetStorage.h
  presets/PresetTypes.h
  presets/PresetTypes.cpp
  
  # Resources
  resources/ResourceLibrary.h
  resources/ResourceLibrary.cpp
  
  # Utilities
  util/FileSystem.cpp
  util/FileSystem.h
  
  # Platform-specific helpers
  $<$<PLATFORM_ID:Windows>:platform/WebViewDPIHelper_win.cpp>
  $<$<PLATFORM_ID:Windows>:platform/WebViewDPIHelper_win.h>
  
  ${_nam_dsp_sources}
)

add_library(GuitarFXCore STATIC ${_guitarfx_core_sources})

# Locate Eigen bundled with NeuralAmpModelerCore
if(DEFINED NeuralAmpModelerCore_SOURCE_DIR)
  set(_nam_eigen_dir "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen")
  if(NOT EXISTS "${_nam_eigen_dir}")
    set(_nam_eigen_dir "${NeuralAmpModelerCore_SOURCE_DIR}/../neuralampmodelercore-build/_deps/eigen-src")
  endif()
else()
  set(_nam_eigen_dir "${CMAKE_BINARY_DIR}/_deps/neuralampmodelercore-src/Dependencies/eigen")
endif()

target_include_directories(GuitarFXCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../config
    ${iPlug2_SOURCE_DIR}/IPlug
    ${iPlug2_SOURCE_DIR}/IPlug/APP
    ${iPlug2_SOURCE_DIR}/IPlug/Extras
    ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView
    ${iPlug2_SOURCE_DIR}/IPlug/VST3_SDK
    ${iPlug2_SOURCE_DIR}/WDL
    ${NeuralAmpModelerCore_SOURCE_DIR}
    ${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/nlohmann
    ${_nam_eigen_dir}
)

target_link_libraries(GuitarFXCore
  PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
)

if(TARGET nam)
  target_link_libraries(GuitarFXCore PUBLIC nam)
endif()

target_compile_definitions(GuitarFXCore
  PUBLIC
    IPLUG_EDITOR=1
    IPLUG_DSP=1
    SAMPLE_TYPE_FLOAT
    # Prevent Windows min/max macros from conflicting with std::numeric_limits
    NOMINMAX
    # Note: APP_API is NOT defined here. Format-specific targets (standalone,
    # VST3, etc.) must define their own API macro. The core library compiles
    # GuitarFXPlugin.cpp as a static library without API-specific code.
)

if(WIN32)
  target_compile_definitions(GuitarFXCore PUBLIC _WIN32 WIN32 _WINDOWS)
endif()

set_target_properties(GuitarFXCore PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  FOLDER "GuitarFX"
)

add_subdirectory(platform)
