# Applied to each product's shared-code interface library: set SOUNDSHED_SHARED_CODE_TARGET
# (SharedCode for Soundshed Guitar, NanoSharedCode for Soundshed Guitar Nano) and include this.
if(NOT SOUNDSHED_SHARED_CODE_TARGET)
    set(SOUNDSHED_SHARED_CODE_TARGET SharedCode)
endif()

if (MSVC)
    # fast math and better simd support in RELEASE
    # https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior?view=msvc-170#fast
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE $<$<CONFIG:RELEASE>:/fp:fast>)
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE $<$<CONFIG:RelWithDebInfo>:/fp:fast>)
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE $<$<CONFIG:RELEASE>:/Ox>)
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE $<$<CONFIG:RelWithDebInfo>:/Ox>)

else ()
    # See the implications here:
    # https://stackoverflow.com/q/45685487
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE $<$<CONFIG:RELEASE>:-Ofast>)
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE $<$<CONFIG:RelWithDebInfo>:-Ofast>)
endif ()

# Same x86-64 (avx2/avx/sse2) and arm64 baseline the core library uses.
guitarfx_apply_simd_arch_flags(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE)

# Tell MSVC to properly report what c++ version is being used
if (MSVC)
    target_compile_options(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE /Zc:__cplusplus)
endif ()

# C++23, please
# Use cxx_std_23 for C++23 (as of CMake v 3.20)
target_compile_features(${SOUNDSHED_SHARED_CODE_TARGET} INTERFACE cxx_std_23)
