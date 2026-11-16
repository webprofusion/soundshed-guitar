# The polyphonic note detector's per-frame work (src/dsp/PolyNoteDetector.h): its spectrum, the
# network that judges each note, and the trained weights. Built in a directory scope of its own for
# the reason the NAM core and the fused WaveNet are (cmake/FusedWaveNet/CMakeLists.txt):
# unoptimised it takes about as long as the audio lasts, so a Debug app could not run Guitar to
# MIDI's Polyphonic mode live, and the tests that play chords through it would set the pace of the
# whole test run.

add_library(SoundshedGuitarPolyNotes STATIC
    "${GUITARFX_CORE_ROOT}/src/dsp/PolySpectrum.h"
    "${GUITARFX_CORE_ROOT}/src/dsp/PolySpectrum.cpp"
    "${GUITARFX_CORE_ROOT}/src/dsp/PolyNoteDetector.h"
    "${GUITARFX_CORE_ROOT}/src/dsp/PolyNoteDetector.cpp"
    "${GUITARFX_CORE_ROOT}/src/dsp/PolyNoteModel.h"
    "${GUITARFX_CORE_ROOT}/src/dsp/PolyNoteModel.cpp"
    "${GUITARFX_CORE_ROOT}/src/dsp/PolyNoteModelData.inc")
set_target_properties(SoundshedGuitarPolyNotes PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
guitarfx_disable_linux_gnu_unique(SoundshedGuitarPolyNotes)
guitarfx_apply_release_optimizations(SoundshedGuitarPolyNotes)
target_include_directories(SoundshedGuitarPolyNotes PUBLIC "${GUITARFX_CORE_ROOT}/src")

# The spectrum's transform (SimdFFT) uses SSE3 intrinsics in every configuration: as for the core.
if(APPLE)
    set(_guitarfx_polynotes_arches "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT _guitarfx_polynotes_arches)
        set(_guitarfx_polynotes_arches "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(_guitarfx_polynotes_arches MATCHES "x86_64")
        target_compile_options(SoundshedGuitarPolyNotes PRIVATE
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:SHELL:-Xarch_x86_64 -msse3>)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    target_compile_options(SoundshedGuitarPolyNotes PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-msse3>)
endif()

if(GUITARFX_OPTIMISE_NAM_IN_DEBUG)
    if(MSVC)
        string(REGEX REPLACE "[/-](RTC[1csu]+|Od|Ob0)( |$)" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(SoundshedGuitarPolyNotes PRIVATE $<$<CONFIG:Debug>:/O2> $<$<CONFIG:Debug>:/Ob2>)
    else()
        target_compile_options(SoundshedGuitarPolyNotes PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endif()
