# Tests for SoundshedGuitarCore

set(TEST_SOURCES
    CustomEffectLibraryTests.cpp
    DefaultPresetIntegrityTests.cpp
    DSPPerformanceTests.cpp
    EffectProcessorTests.cpp
    IRConvolutionTests.cpp
    MultiPresetMixerTests.cpp
    NAMPerformanceBenchmark.cpp
    PresetDSPLoadingTests.cpp
    PresetManagementWorkflowTests.cpp
    PresetStorageTests.cpp
    ResourcePreviewWorkflowTests.cpp
    SignalGraphExecutorTests.cpp
    StereoProcessingTests.cpp
    SynthSawEffectTests.cpp
    TestWavLoad.cpp
)

if(GUITARFX_CORE_ENABLE_WASM_EFFECTS)
    list(APPEND TEST_SOURCES WasmEffectHostTests.cpp)
endif()

option(GUITARFX_TEST_WITH_IPLUG2 "Build tests that require iPlug2 headers" OFF)
if(GUITARFX_TEST_WITH_IPLUG2)
    list(APPEND TEST_SOURCES
        NAMModelDiagnosticTests.cpp
        OfflineProcessingTest.cpp
        PresetDSPProcessingTests.cpp
        RealAudioProcessingTests.cpp
        SampleRateBufferTests.cpp
    )
endif()

set(GUITARFX_TEST_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# Copy the shared library data files (JSON catalogue) into testdata at configure time.
# The NAM and IR asset files are copied manually into testdata/assets/ and are not
# part of the UI deployment – they live here for test purposes only.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/testdata/data")
foreach(_data_file audiofx-models.json ir-library.json default-presets.json)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../ui/data/${_data_file}"
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/data/${_data_file}"
        COPYONLY
    )
endforeach()

foreach(test_source IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE SoundshedGuitarCore)
    if(TARGET NeuralAmpModelerCore)
        target_link_libraries(${test_name} PRIVATE NeuralAmpModelerCore)
    elseif(TARGET nam)
        target_link_libraries(${test_name} PRIVATE nam)
    endif()
    target_include_directories(${test_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/helpers
    )
    target_compile_definitions(${test_name}
        PRIVATE
            GUITARFX_TEST_RESOURCES_DIR="${GUITARFX_TEST_RESOURCES_DIR}"
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test-resources")
        add_custom_command(TARGET ${test_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/test-resources"
                "$<TARGET_FILE_DIR:${test_name}>/test-resources"
            COMMENT "Copying test resources"
        )
    endif()

    if(MSVC)
        target_compile_options(${test_name} PRIVATE /bigobj)
    endif()

    target_compile_options(${test_name} PRIVATE
        $<TARGET_PROPERTY:SoundshedGuitarCore,COMPILE_OPTIONS>
    )

    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "GuitarFX/Tests"
    )

    # Reuse core library's precompiled header
    target_precompile_headers(${test_name} REUSE_FROM SoundshedGuitarCore)

    add_test(NAME ${test_name} COMMAND ${test_name})

    # Label slow benchmarks so they are excluded from default test runs
    if(test_name STREQUAL "NAMPerformanceBenchmark" OR test_name STREQUAL "DSPPerformanceTests")
        set_tests_properties(${test_name} PROPERTIES LABELS "benchmark")
    endif()
endforeach()
