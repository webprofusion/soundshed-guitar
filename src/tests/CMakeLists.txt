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
