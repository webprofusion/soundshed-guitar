# Tests for SoundshedGuitarCore

set(TEST_SOURCES
    AtomicFileWriteTests.cpp
    AudioDeviceRequestTests.cpp
    AutomationBypassEndToEndTests.cpp
    AutomationNodeParamRangeTests.cpp
    AutomationNodeRealtimeTests.cpp
    AutomationPresetMappingTests.cpp
    AutomationSlotMidiToggleTests.cpp
    AutomationSlotThreadTests.cpp
    BiquadDesignTests.cpp
    BiquadNyquistTests.cpp
    BuiltinAmpEffectTests.cpp
    BlendDefinitionWorkflowTests.cpp
    BlendEffectTests.cpp
    BoundaryNodeGainTests.cpp
    ControlSurfaceDrainTests.cpp
    ControllerDisplayFeedTests.cpp
    CustomEffectLibraryTests.cpp
    DefaultPresetIntegrityTests.cpp
    DelayCharacterTests.cpp
    DriveLegacyMigrationTests.cpp
    DrivePedalTests.cpp
    DSPPerformanceTests.cpp
    EffectAliasParityTests.cpp
    EffectProcessorTests.cpp
    FastMathNanTests.cpp
    GaplessSwitchingTests.cpp
    GlobalChainEditorTests.cpp
    GlobalChainEngineTests.cpp
    GuitarToMidiEffectTests.cpp
    PresetRetirementTests.cpp
    DemoPreviewLoopTests.cpp
    HostedPluginCaptureLockTests.cpp
    HostedPluginStateWorkflowTests.cpp
    HostRestoreThreadTests.cpp
    HostStateThreadTests.cpp
    IRCabAutomationRealtimeTests.cpp
    IRConvolutionTests.cpp
    IRReverbAutomationRealtimeTests.cpp
    IRReverbQualityTests.cpp
    ImpulseResamplerTests.cpp
    PracticeToolCrossfadeTests.cpp
    MessageDispatchTypeErrorTests.cpp
    MessageHandlerRegistryTests.cpp
    MetronomeServiceTests.cpp
    MixerInstanceLockTests.cpp
    MixerTelemetryTests.cpp
    MultiPresetMixerTests.cpp
    NamCalibrationProbe.cpp
    NamModelCacheTests.cpp
    NamQualityPolicyTests.cpp
    NamResamplerResetTests.cpp
    NAMPerformanceBenchmark.cpp
    NoiseGateTests.cpp
    NotePlayerTests.cpp
    NoteTrackerTests.cpp
    PitchTrackerTests.cpp
    PluginPathUtilsTests.cpp
    PresetDSPLoadingTests.cpp
    PresetManagementWorkflowTests.cpp
    PresetStorageTests.cpp
    PresetVoicePoolTests.cpp
    JsonStoreConcurrencyTests.cpp
    JsonStoreTests.cpp
    ResourceLibraryDeleteWorkflowTests.cpp
    ResourcePreviewWorkflowTests.cpp
    RealtimeConvolverTests.cpp
    ResourceLoadBenchmark.cpp
    RingModEffectTests.cpp
    SampleRateConverterTests.cpp
    StorageMigrationTests.cpp
    SignalChainMutationStressTest.cpp
    SignalChainThreadingBenchmark.cpp
    SteadyStateProfiler.cpp
    TelemetryPublisherTests.cpp
    TunerEngineTests.cpp
    SignalGraphExecutorTests.cpp
    SignalPathNodeConfigLockTests.cpp
    SimpleCabEffectTests.cpp
    SpectrumTapTests.cpp
    Spatial3DEffectTests.cpp
    StereoProcessingTests.cpp
    SynthSawEffectTests.cpp
    TestWavLoad.cpp
    Tone3000ResourceWorkflowTests.cpp
    TransposeBenchmark.cpp
    WahEffectTests.cpp
)

if(GUITARFX_CORE_ENABLE_WASM_EFFECTS)
    list(APPEND TEST_SOURCES WasmEffectHostTests.cpp)
endif()

# SignalChainMutationStressTest is a concurrency fuzzer: it runs for ~15 minutes, seeds itself
# from the clock, and a failure needs manual triage from its trace file rather than a rerun.
# The binary is always built, but it is not registered with ctest unless this is ON, so a
# normal `ctest` run stays fast and deterministic. To run it: enable this option, or just
# invoke the executable directly (see GUITARFX_STRESS_* env vars in its header comment).
option(GUITARFX_TEST_STRESS "Register the signal-chain mutation stress test with ctest" OFF)

# The benchmarks print measurements and assert nothing about them, so under ctest they only
# catch a crash, yet they were over three minutes of a Debug run (TransposeBenchmark alone about
# three, and it leaves ~36 MB of WAVs under the build tree each time). They are always built;
# this registers them, labelled `benchmark`, for `ctest -L benchmark`.
set(GUITARFX_TEST_BENCHMARK_NAMES
    NAMPerformanceBenchmark
    ResourceLoadBenchmark
    SignalChainThreadingBenchmark
    TransposeBenchmark
)
option(GUITARFX_TEST_BENCHMARKS "Register the benchmarks with ctest" OFF)

option(GUITARFX_TEST_EXTENDED_DSP "Build extended DSP diagnostic tests" OFF)
if(GUITARFX_TEST_EXTENDED_DSP)
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
            GUITARFX_DEMO_AUDIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../ui/demo"
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

    # The stress test and the benchmarks are opt-in; everything else is registered normally.
    set(_register_test TRUE)
    if(test_name STREQUAL "SignalChainMutationStressTest" AND NOT GUITARFX_TEST_STRESS)
        set(_register_test FALSE)
    endif()
    if(test_name IN_LIST GUITARFX_TEST_BENCHMARK_NAMES AND NOT GUITARFX_TEST_BENCHMARKS)
        set(_register_test FALSE)
    endif()

    if(_register_test)
        if(test_name STREQUAL "SteadyStateProfiler")
            # A profiler, not a test: under ctest it is a smoke check of its stack sampler,
            # which fails if a sampling pass deadlocks or captures nothing. The baseline
            # chain needs no model files, and one short pass per mode keeps it to seconds
            # even in Debug; the timeout turns a hang back into a failure.
            add_test(NAME ${test_name} COMMAND ${test_name}
                --profile baseline --seconds 0.5 --repeats 1 --diagnostics both --top 5)
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
        else()
            add_test(NAME ${test_name} COMMAND ${test_name})
        endif()

        # Label the slow opt-in runs so `ctest -LE benchmark` can skip them once registered. Not
        # DSPPerformanceTests, whose five checks (stats, limiter ceiling) are real and take 0.1 s,
        # nor SteadyStateProfiler, whose registered form is a quick smoke check.
        if(test_name IN_LIST GUITARFX_TEST_BENCHMARK_NAMES OR test_name STREQUAL "SignalChainMutationStressTest")
            set_tests_properties(${test_name} PROPERTIES LABELS "benchmark")
        endif()
    endif()
endforeach()

# Shared harness for the riff/audio-lock regression without growing the main test past its size budget.
target_sources(MixerInstanceLockTests PRIVATE MixerInstanceLockSupport.cpp MixerInstanceLockSupport.h)

# Builds real (deflated) zips to preview, as Tone3000 serves them; the core links miniz privately.
target_link_libraries(Tone3000ResourceWorkflowTests PRIVATE guitarfx_miniz)

# The default-settings audio snapshot tools/audio-ab compares between versions. Built, not registered.
include(AudioSnapshot.cmake)
