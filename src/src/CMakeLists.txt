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

set(_namguitar_core_sources
  NAMGuitarPlugin.cpp
  NAMGuitarPlugin.h
  dsp/IRManager.cpp
  dsp/IRManager.h
  dsp/NAMDSPManager.cpp
  dsp/NAMDSPManager.h
  models/ModelHasher.cpp
  models/ModelHasher.h
  network/PresetServiceClient.cpp
  network/PresetServiceClient.h
  presets/PresetManager.cpp
  presets/PresetManager.h
  presets/PresetStorage.cpp
  presets/PresetStorage.h
  presets/PresetTypes.h
  ui/WebUIBridge.cpp
  ui/WebUIBridge.h
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
    ${iPlug2_SOURCE_DIR}/IGraphics
    ${iPlug2_SOURCE_DIR}/IGraphics/Controls
    ${iPlug2_SOURCE_DIR}/IGraphics/Drawing
    ${iPlug2_SOURCE_DIR}/IGraphics/Platforms
    ${iPlug2_SOURCE_DIR}/WDL
    ${iPlug2_SOURCE_DIR}/Dependencies/IGraphics/STB
    ${iPlug2_SOURCE_DIR}/Dependencies/IGraphics/NanoSVG/src
    ${iPlug2_SOURCE_DIR}/Dependencies/IGraphics/NanoVG/src
    ${iPlug2_SOURCE_DIR}/Dependencies/IGraphics/glad/include
    ${iPlug2_SOURCE_DIR}/Dependencies/IGraphics/glad_GL2/include
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
    # Use IGraphics with NanoVG backend
    IGRAPHICS_NANOVG
    IGRAPHICS_GL2
    # Prevent Windows min/max macros from conflicting with std::numeric_limits
    NOMINMAX
    # Default to standalone APP_API for building the core library;
    # format-specific targets override this when linking.
    APP_API
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
