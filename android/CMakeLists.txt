# ---------------------------------------------------------------------------
# Android native entry point.
#
# Gradle's externalNativeBuild points here. This file is a thin shim: it pins
# the options the Android build needs, pulls in the normal JUCE project, and
# then fixes up the standalone target so Android can actually load it.
#
# Two Android-specific requirements drive the fixups at the bottom:
#   1. The shared library MUST be called "juce_jni" — that is the name JUCE's
#      own Java bootstrap (com.rmsl.juce.Java) hands to System.loadLibrary().
#      Left alone, JUCE would name it after PRODUCT_NAME ("Soundshed Guitar"),
#      which is both wrong and contains a space.
#   2. It must land in the directory AGP passes as CMAKE_LIBRARY_OUTPUT_DIRECTORY,
#      not in JUCE's usual <target>_artefacts tree, or Gradle will not find it
#      to package into the APK.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.25)

project(SoundshedGuitarAndroid LANGUAGES C CXX)

# AGP tells us where the .so has to end up. JUCE overwrites this property per
# target, so remember it now and re-apply it after add_subdirectory().
set(SSG_ANDROID_LIB_OUT "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

# --- Options the Android build cannot use ----------------------------------
# Wasmtime publishes no Android C API bundle, so the generic WASM effect host
# is unavailable here. (Default is ON, so this override matters.)
set(GUITARFX_CORE_ENABLE_WASM_EFFECTS OFF CACHE BOOL "" FORCE)

# The core enables AVX2 for any x86_64 target, which includes the Android
# x86_64 ABI — and nothing in that ABI's baseline guarantees AVX2, so an
# emulator or device without it would take an illegal instruction. Also ON by
# default, so this override matters too.
set(GUITARFX_CORE_ENABLE_AVX2 OFF CACHE BOOL "" FORCE)

# `char` is unsigned by default on ARM, and WDL (vendored under NAM's
# AudioDSPTools) refuses to compile in that mode — it has a static assert
# demanding signed char, because its code assumes it.
#
# Force signed char for the whole Android build rather than defining
# WDL_ALLOW_UNSIGNED_DEFAULT_CHAR: silencing the guard would leave whatever it
# protects against in place, and this keeps `char` semantics identical to the
# x86 desktop builds, so the DSP behaves the same everywhere. Applying it
# tree-wide also avoids one translation unit disagreeing with another about the
# signedness of a `char` field in a shared header. It is a no-op on x86_64,
# where char is already signed.
add_compile_options(-fsigned-char)

# --- Guitar-friendly audio capture ------------------------------------------
# Oboe defaults its capture InputPreset to VoiceRecognition, which puts the
# device's voice processing (AGC, noise suppression, band-limiting) in front of
# the amp sim and adds latency. JUCE opens the stream with a local
# oboe::AudioStreamBuilder and never calls setInputPreset(), exposes no config
# flag for it, and lives in a submodule we cannot fork — so the only lever is
# the default member initialiser in Oboe's own header.
#
# oboe-overlay/ holds a copy of that one header with the default changed to
# Unprocessed. Putting it first on the include path here (directory scope, so it
# is inherited by every target created below, Oboe's own sources included) means
# all translation units agree on the value. Oboe includes it as
# "oboe/AudioStreamBase.h", which is resolved through the include path, so the
# overlay wins.
include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/oboe-overlay")
message(STATUS "Audio: Oboe input preset overridden to Unprocessed via oboe-overlay/")

# JUCE builds its juceaide helper for the host during configure. On Windows that
# needs an MSVC environment which Gradle does not provide; set one up.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HostToolchain)
ssg_provision_msvc_host_environment()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../juce" "${CMAKE_BINARY_DIR}/ssg")

# --- Android target fixups --------------------------------------------------
set(SSG_ANDROID_TARGET "SoundshedGuitar_Standalone")

if(NOT TARGET ${SSG_ANDROID_TARGET})
    message(FATAL_ERROR
        "Expected target ${SSG_ANDROID_TARGET} to exist. The JUCE project should "
        "build the Standalone format on Android — check the FORMATS list in "
        "juce/CMakeLists.txt.")
endif()

set_target_properties(${SSG_ANDROID_TARGET} PROPERTIES
    OUTPUT_NAME "juce_jni"
    LIBRARY_OUTPUT_DIRECTORY "${SSG_ANDROID_LIB_OUT}")

# JUCE's Android startup dlsym's "juce_CreateApplication" out of the loaded
# library rather than linking against it. That function is defined in Main.cpp,
# which ends up in the SoundshedGuitar static library — and because nothing in
# the shared library references it, the linker drops the whole archive member
# and the app comes up to a blank screen with no JUCEApplication behind it.
#
# --undefined= creates an artificial reference so the member is kept. The other
# half of this — giving the symbol default visibility so dlsym can see it — is
# in juce/source/Main.cpp.
target_link_options(${SSG_ANDROID_TARGET} PRIVATE
    "LINKER:--undefined=juce_CreateApplication")

