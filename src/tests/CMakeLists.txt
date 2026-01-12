set(GUITARFX_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
get_filename_component(GUITARFX_PROJECT_ROOT "${GUITARFX_PROJECT_ROOT}" ABSOLUTE)
set(GUITARFX_RESOURCES_DIR "${GUITARFX_PROJECT_ROOT}/resources")
file(TO_CMAKE_PATH "${GUITARFX_RESOURCES_DIR}" GUITARFX_RESOURCES_DIR_NORMALIZED)

add_executable(GuitarFX_PresetTests
  DefaultPresetIntegrityTests.cpp
)

target_link_libraries(GuitarFX_PresetTests
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_PresetTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_PresetTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME DefaultPresetIntegrityTests COMMAND GuitarFX_PresetTests)

# Effect Processor Test - verifies that each effect processor can process audio
add_executable(GuitarFX_EffectProcessorTests
  EffectProcessorTests.cpp
)

target_include_directories(GuitarFX_EffectProcessorTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
)

target_link_libraries(GuitarFX_EffectProcessorTests
  PRIVATE
    GuitarFXCore
)

target_compile_features(GuitarFX_EffectProcessorTests PRIVATE cxx_std_20)

add_test(NAME EffectProcessorTests COMMAND GuitarFX_EffectProcessorTests)

# DSP Loading Test - verifies that each preset can load its model and IR into the DSP manager
add_executable(GuitarFX_DSPLoadingTests
  PresetDSPLoadingTests.cpp
)

target_include_directories(GuitarFX_DSPLoadingTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_DSPLoadingTests
  PRIVATE
    GuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_DSPLoadingTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_DSPLoadingTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME PresetDSPLoadingTests COMMAND GuitarFX_DSPLoadingTests)

# DSP Processing Test - verifies that each preset can process audio without errors
add_executable(GuitarFX_DSPProcessingTests
  PresetDSPProcessingTests.cpp
)

target_include_directories(GuitarFX_DSPProcessingTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_DSPProcessingTests
  PRIVATE
    GuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_DSPProcessingTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_DSPProcessingTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME PresetDSPProcessingTests COMMAND GuitarFX_DSPProcessingTests)

# IR Convolution Unit Tests - verifies the convolution algorithm correctness
add_executable(GuitarFX_IRConvolutionTests
  IRConvolutionTests.cpp
)

target_include_directories(GuitarFX_IRConvolutionTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_IRConvolutionTests
  PRIVATE
    GuitarFXCore
)

target_compile_features(GuitarFX_IRConvolutionTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_IRConvolutionTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME IRConvolutionTests COMMAND GuitarFX_IRConvolutionTests)

# NAM Model Diagnostic Tests - detailed diagnostics for model processing issues
add_executable(GuitarFX_ModelDiagnosticTests
  NAMModelDiagnosticTests.cpp
)

target_include_directories(GuitarFX_ModelDiagnosticTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_ModelDiagnosticTests
  PRIVATE
    GuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_ModelDiagnosticTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_ModelDiagnosticTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME NAMModelDiagnosticTests COMMAND GuitarFX_ModelDiagnosticTests)

# Sample Rate and Buffer Size Tests - diagnoses sample rate and buffer issues
add_executable(GuitarFX_SampleRateBufferTests
  SampleRateBufferTests.cpp
)

target_include_directories(GuitarFX_SampleRateBufferTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_SampleRateBufferTests
  PRIVATE
    GuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_SampleRateBufferTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_SampleRateBufferTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME SampleRateBufferTests COMMAND GuitarFX_SampleRateBufferTests)
# SignalGraph execution tests - simple, complex, and parallel routing
add_executable(GuitarFX_SignalGraphExecutorTests
  SignalGraphExecutorTests.cpp
)

target_include_directories(GuitarFX_SignalGraphExecutorTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
)

target_link_libraries(GuitarFX_SignalGraphExecutorTests
  PRIVATE
    GuitarFXCore
)

target_compile_features(GuitarFX_SignalGraphExecutorTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_SignalGraphExecutorTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME SignalGraphExecutorTests COMMAND GuitarFX_SignalGraphExecutorTests)

# DSP Performance Tests - verifies DSP performance statistics are correctly populated
add_executable(GuitarFX_DSPPerformanceTests
  DSPPerformanceTests.cpp
)

target_include_directories(GuitarFX_DSPPerformanceTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
)

target_link_libraries(GuitarFX_DSPPerformanceTests
  PRIVATE
    GuitarFXCore
)

target_compile_features(GuitarFX_DSPPerformanceTests PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_DSPPerformanceTests
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME DSPPerformanceTests COMMAND GuitarFX_DSPPerformanceTests)

# Real Audio Processing Tests - tests the full DSP pipeline with actual audio processing
add_executable(GuitarFX_RealAudioProcessingTests
  RealAudioProcessingTests.cpp
)

target_include_directories(GuitarFX_RealAudioProcessingTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_RealAudioProcessingTests
  PRIVATE
    GuitarFXCore
)

target_compile_features(GuitarFX_RealAudioProcessingTests PRIVATE cxx_std_20)

add_test(NAME RealAudioProcessingTests COMMAND GuitarFX_RealAudioProcessingTests)

# Multi-Preset Mixer Pan/Mix Tests
add_executable(GuitarFX_MultiPresetMixerTests
  MultiPresetMixerTests.cpp
)

target_include_directories(GuitarFX_MultiPresetMixerTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
)

target_link_libraries(GuitarFX_MultiPresetMixerTests
  PRIVATE
    GuitarFXCore
)

target_compile_features(GuitarFX_MultiPresetMixerTests PRIVATE cxx_std_20)

add_test(NAME MultiPresetMixerTests COMMAND GuitarFX_MultiPresetMixerTests)

add_executable(GuitarFX_OfflineProcessingTest
  OfflineProcessingTest.cpp
)

target_include_directories(GuitarFX_OfflineProcessingTest
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
    ${NeuralAmpModelerCore_SOURCE_DIR}
)

target_link_libraries(GuitarFX_OfflineProcessingTest
  PRIVATE
    GuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_OfflineProcessingTest PRIVATE cxx_std_20)

target_compile_definitions(GuitarFX_OfflineProcessingTest
  PRIVATE
    GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_RESOURCES_DIR_NORMALIZED}"
)

add_test(NAME OfflineProcessingTest COMMAND GuitarFX_OfflineProcessingTest)

# Preset Storage Serialization Tests - verifies preset serialization/deserialization with attachments
add_executable(GuitarFX_PresetStorageTests
  PresetStorageTests.cpp
)

target_include_directories(GuitarFX_PresetStorageTests
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
)

target_link_libraries(GuitarFX_PresetStorageTests
  PRIVATE
    GuitarFXCore
    nlohmann_json::nlohmann_json
)

target_compile_features(GuitarFX_PresetStorageTests PRIVATE cxx_std_20)

add_test(NAME PresetStorageTests COMMAND GuitarFX_PresetStorageTests)

# WAV Loading Debug Test
add_executable(TestWavLoad
  TestWavLoad.cpp
)

target_include_directories(TestWavLoad
  PRIVATE
    ${GUITARFX_PROJECT_ROOT}/src
)

target_link_libraries(TestWavLoad
  PRIVATE
    GuitarFXCore
)

target_compile_features(TestWavLoad PRIVATE cxx_std_20)
