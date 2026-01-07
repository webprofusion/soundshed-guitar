# NAMGuitarFXCore static library containing the shared plugin implementation.
# Format-specific targets in the platform subdirectory link against this library.

# NeuralAmpModelerCore sources - compiled directly since the upstream project
# doesn't export a library target
set(_nam_dsp_sources
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/activations.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/convnet.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/dsp.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/get_dsp.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/lstm.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/util.cpp
  ${NeuralAmpModelerCore_SOURCE_DIR}/NAM/wavenet.cpp
)

# NAMGuitarFXCore contains API-agnostic shared code. NAMGuitarPlugin.cpp 
# and NAMGuitarPlugin.h are NOT included here because they require format-
# specific API definitions (APP_API, VST3_API, etc.). Each format target
# compiles NAMGuitarPlugin.cpp directly with its own API definition.

set(_namguitar_core_sources
  # Existing DSP
  dsp/SimdFFT.h
  dsp/RealtimeConvolver.cpp
  dsp/RealtimeConvolver.h
  dsp/SimpleCabSim.h
  dsp/IRTypes.h
  dsp/IRManager.cpp
  dsp/IRManager.h
  dsp/NAMDSPManager.cpp
  dsp/NAMDSPManager.h
  dsp/NAMFactoryForce.cpp
  dsp/ParametricEQ.h
  dsp/DelayEffect.h
  dsp/ReverbEffect.h
  
  # V2 Signal Graph DSP
  dsp/EffectProcessor.h
  dsp/EffectRegistry.h
  dsp/EffectRegistry.cpp
  dsp/SignalGraphExecutor.h
  dsp/SignalGraphExecutor.cpp
  dsp/GraphDSPManager.h
  
  # Effect implementations
  dsp/effects/BuiltinEffects.h
  dsp/effects/GainEffect.h
  dsp/effects/NoiseGateEffect.h
  dsp/effects/ParametricEQEffect.h
  dsp/effects/DelayEffect.h
  dsp/effects/ReverbEffect.h
  dsp/effects/CompressorEffect.h
  dsp/effects/NAMAmpEffect.h
  dsp/effects/IRCabEffect.h
  dsp/effects/SimpleCabEffect.h
  
  # Models
  models/ModelHasher.cpp
  models/ModelHasher.h
  
  # Network
  network/PresetServiceClient.cpp
  network/PresetServiceClient.h
  
  # Presets V1
  presets/PresetManager.cpp
  presets/PresetManager.h
  presets/PresetStorage.cpp
  presets/PresetStorage.h
  presets/PresetTypes.h
  
  # Presets V2
  presets/PresetTypesV2.h
  presets/PresetStorageV2.h
  presets/PresetStorageV2.cpp
  presets/PresetMigration.h
  
  # Resources
  resources/ResourceLibrary.h
  resources/ResourceLibrary.cpp
  
  # Utilities
  util/FileSystem.cpp
  util/FileSystem.h
  
  ${_nam_dsp_sources}
)

add_library(NAMGuitarFXCore STATIC ${_namguitar_core_sources})

# Locate Eigen bundled with NeuralAmpModelerCore
if(DEFINED NeuralAmpModelerCore_SOURCE_DIR)
  set(_nam_eigen_dir "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen")
  if(NOT EXISTS "${_nam_eigen_dir}")
    set(_nam_eigen_dir "${NeuralAmpModelerCore_SOURCE_DIR}/../neuralampmodelercore-build/_deps/eigen-src")
  endif()
else()
  set(_nam_eigen_dir "${CMAKE_BINARY_DIR}/_deps/neuralampmodelercore-src/Dependencies/eigen")
endif()

target_include_directories(NAMGuitarFXCore
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

target_link_libraries(NAMGuitarFXCore
  PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
)

if(TARGET nam)
  target_link_libraries(NAMGuitarFXCore PUBLIC nam)
endif()

target_compile_definitions(NAMGuitarFXCore
  PUBLIC
    IPLUG_EDITOR=1
    IPLUG_DSP=1
    SAMPLE_TYPE_FLOAT
    # Prevent Windows min/max macros from conflicting with std::numeric_limits
    NOMINMAX
    # Note: APP_API is NOT defined here. Format-specific targets (standalone,
    # VST3, etc.) must define their own API macro. The core library compiles
    # NAMGuitarPlugin.cpp as a static library without API-specific code.
)

if(WIN32)
  target_compile_definitions(NAMGuitarFXCore PUBLIC _WIN32 WIN32 _WINDOWS)
endif()

set_target_properties(NAMGuitarFXCore PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  FOLDER "NAMGuitarFX"
)

add_subdirectory(platform)
