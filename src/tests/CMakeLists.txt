set(NAMGUITAR_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
get_filename_component(NAMGUITAR_PROJECT_ROOT "${NAMGUITAR_PROJECT_ROOT}" ABSOLUTE)
set(NAMGUITAR_RESOURCES_DIR "${NAMGUITAR_PROJECT_ROOT}/resources")
file(TO_CMAKE_PATH "${NAMGUITAR_RESOURCES_DIR}" NAMGUITAR_RESOURCES_DIR_NORMALIZED)

add_executable(NAMGuitarFX_PresetTests
  DefaultPresetIntegrityTests.cpp
)

target_link_libraries(NAMGuitarFX_PresetTests
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_PresetTests PRIVATE cxx_std_20)

target_compile_definitions(NAMGuitarFX_PresetTests
  PRIVATE
    NAMGUITAR_TEST_RESOURCES_DIR="${NAMGUITAR_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME DefaultPresetIntegrityTests COMMAND NAMGuitarFX_PresetTests)

# DSP Loading Test - verifies that each preset can load its model and IR into the DSP manager
add_executable(NAMGuitarFX_DSPLoadingTests
  PresetDSPLoadingTests.cpp
)

target_include_directories(NAMGuitarFX_DSPLoadingTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_DSPLoadingTests
  PRIVATE
    NAMGuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_DSPLoadingTests PRIVATE cxx_std_20)

target_compile_definitions(NAMGuitarFX_DSPLoadingTests
  PRIVATE
    NAMGUITAR_TEST_RESOURCES_DIR="${NAMGUITAR_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME PresetDSPLoadingTests COMMAND NAMGuitarFX_DSPLoadingTests)

# DSP Processing Test - verifies that each preset can process audio without errors
add_executable(NAMGuitarFX_DSPProcessingTests
  PresetDSPProcessingTests.cpp
)

target_include_directories(NAMGuitarFX_DSPProcessingTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_DSPProcessingTests
  PRIVATE
    NAMGuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_DSPProcessingTests PRIVATE cxx_std_20)

target_compile_definitions(NAMGuitarFX_DSPProcessingTests
  PRIVATE
    NAMGUITAR_TEST_RESOURCES_DIR="${NAMGUITAR_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME PresetDSPProcessingTests COMMAND NAMGuitarFX_DSPProcessingTests)

# IR Convolution Unit Tests - verifies the convolution algorithm correctness
add_executable(NAMGuitarFX_IRConvolutionTests
  IRConvolutionTests.cpp
)

target_include_directories(NAMGuitarFX_IRConvolutionTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_IRConvolutionTests
  PRIVATE
    NAMGuitarFXCore
)

target_compile_features(NAMGuitarFX_IRConvolutionTests PRIVATE cxx_std_20)

add_test(NAME IRConvolutionTests COMMAND NAMGuitarFX_IRConvolutionTests)

# NAM Model Diagnostic Tests - detailed diagnostics for model processing issues
add_executable(NAMGuitarFX_ModelDiagnosticTests
  NAMModelDiagnosticTests.cpp
)

target_include_directories(NAMGuitarFX_ModelDiagnosticTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_ModelDiagnosticTests
  PRIVATE
    NAMGuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_ModelDiagnosticTests PRIVATE cxx_std_20)

target_compile_definitions(NAMGuitarFX_ModelDiagnosticTests
  PRIVATE
    NAMGUITAR_TEST_RESOURCES_DIR="${NAMGUITAR_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME NAMModelDiagnosticTests COMMAND NAMGuitarFX_ModelDiagnosticTests)

# Sample Rate and Buffer Size Tests - diagnoses sample rate and buffer issues
add_executable(NAMGuitarFX_SampleRateBufferTests
  SampleRateBufferTests.cpp
)

target_include_directories(NAMGuitarFX_SampleRateBufferTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_SampleRateBufferTests
  PRIVATE
    NAMGuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_SampleRateBufferTests PRIVATE cxx_std_20)

target_compile_definitions(NAMGuitarFX_SampleRateBufferTests
  PRIVATE
    NAMGUITAR_TEST_RESOURCES_DIR="${NAMGUITAR_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME SampleRateBufferTests COMMAND NAMGuitarFX_SampleRateBufferTests)
# Real Audio Processing Tests - tests the full DSP pipeline with actual audio processing
add_executable(NAMGuitarFX_RealAudioProcessingTests
  RealAudioProcessingTests.cpp
)

target_include_directories(NAMGuitarFX_RealAudioProcessingTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_RealAudioProcessingTests
  PRIVATE
    NAMGuitarFXCore
)

target_compile_features(NAMGuitarFX_RealAudioProcessingTests PRIVATE cxx_std_20)

add_test(NAME RealAudioProcessingTests COMMAND NAMGuitarFX_RealAudioProcessingTests)

# Offline Processing Test App - processes WAV files through complete DSP pipeline
add_executable(NAMGuitarFX_OfflineProcessingTest
  OfflineProcessingTest.cpp
)

target_include_directories(NAMGuitarFX_OfflineProcessingTest
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(NAMGuitarFX_OfflineProcessingTest
  PRIVATE
    NAMGuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_OfflineProcessingTest PRIVATE cxx_std_20)

target_compile_definitions(NAMGuitarFX_OfflineProcessingTest
  PRIVATE
    NAMGUITAR_TEST_RESOURCES_DIR="${NAMGUITAR_RESOURCES_DIR_NORMALIZED}"
)

# Note: This is a utility app, not added as a CTest

# Preset Storage Serialization Tests - verifies preset serialization/deserialization with attachments
add_executable(NAMGuitarFX_PresetStorageTests
  PresetStorageTests.cpp
)

target_include_directories(NAMGuitarFX_PresetStorageTests
  PRIVATE
    ${NAMGUITAR_PROJECT_ROOT}/src
)

target_link_libraries(NAMGuitarFX_PresetStorageTests
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(NAMGuitarFX_PresetStorageTests PRIVATE cxx_std_20)

add_test(NAME PresetStorageTests COMMAND NAMGuitarFX_PresetStorageTests)
