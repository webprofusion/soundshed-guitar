if (MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("/arch:AVX2" GUITARFX_MSVC_HAS_ARCH_AVX2)
    check_cxx_compiler_flag("/arch:armv8.2" GUITARFX_MSVC_HAS_ARCH_ARMV82)
    check_cxx_compiler_flag("/arch:armv8.1" GUITARFX_MSVC_HAS_ARCH_ARMV81)

    # fast math and better simd support in RELEASE
    # https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior?view=msvc-170#fast
    target_compile_options(SharedCode INTERFACE $<$<CONFIG:RELEASE>:/fp:fast>)
    target_compile_options(SharedCode INTERFACE $<$<CONFIG:RelWithDebInfo>:/fp:fast>)
    target_compile_options(SharedCode INTERFACE $<$<CONFIG:RELEASE>:/Ox>)
    target_compile_options(SharedCode INTERFACE $<$<CONFIG:RelWithDebInfo>:/Ox>)

    set(_guitarfx_windows_arch "${CMAKE_GENERATOR_PLATFORM}")
    if(NOT _guitarfx_windows_arch)
        set(_guitarfx_windows_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${_guitarfx_windows_arch}" _guitarfx_windows_arch)

    if(_guitarfx_windows_arch MATCHES "^(x64|amd64|x86_64)$" AND GUITARFX_MSVC_HAS_ARCH_AVX2)
        target_compile_options(SharedCode INTERFACE $<$<CONFIG:RELEASE>:/arch:AVX2>)
        target_compile_options(SharedCode INTERFACE $<$<CONFIG:RelWithDebInfo>:/arch:AVX2>)
    elseif(_guitarfx_windows_arch MATCHES "^(arm64|arm64ec|aarch64)$")
        if(GUITARFX_MSVC_HAS_ARCH_ARMV82)
            target_compile_options(SharedCode INTERFACE $<$<CONFIG:RELEASE>:/arch:armv8.2>)
            target_compile_options(SharedCode INTERFACE $<$<CONFIG:RelWithDebInfo>:/arch:armv8.2>)
        elseif(GUITARFX_MSVC_HAS_ARCH_ARMV81)
            target_compile_options(SharedCode INTERFACE $<$<CONFIG:RELEASE>:/arch:armv8.1>)
            target_compile_options(SharedCode INTERFACE $<$<CONFIG:RelWithDebInfo>:/arch:armv8.1>)
        endif()
    endif()
else ()
    # See the implications here:
    # https://stackoverflow.com/q/45685487
    target_compile_options(SharedCode INTERFACE $<$<CONFIG:RELEASE>:-Ofast>)
    target_compile_options(SharedCode INTERFACE $<$<CONFIG:RelWithDebInfo>:-Ofast>)
endif ()

# Tell MSVC to properly report what c++ version is being used
if (MSVC)
    target_compile_options(SharedCode INTERFACE /Zc:__cplusplus)
endif ()

# C++23, please
# Use cxx_std_23 for C++23 (as of CMake v 3.20)
target_compile_features(SharedCode INTERFACE cxx_std_23)
