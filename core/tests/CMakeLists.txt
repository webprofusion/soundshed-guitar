# Tests for SoundshedGuitarCore

set(TEST_SOURCES
    DefaultPresetIntegrityTests.cpp
    DSPPerformanceTests.cpp
    EffectProcessorTests.cpp
    IRConvolutionTests.cpp
    MultiPresetMixerTests.cpp
    NAMPerformanceBenchmark.cpp
    PresetDSPLoadingTests.cpp
    PresetStorageTests.cpp
    SignalGraphExecutorTests.cpp
    SynthSawEffectTests.cpp
    TestWavLoad.cpp
)

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

set(GUITARFX_TEST_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui")

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

    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "GuitarFX/Tests"
    )

    # Reuse core library's precompiled header
    target_precompile_headers(${test_name} REUSE_FROM SoundshedGuitarCore)

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
