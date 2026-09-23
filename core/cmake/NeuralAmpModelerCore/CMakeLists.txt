# The local NeuralAmpModelerCore target, built in a directory scope of its own so that its
# Debug flags can differ from the rest of the core's.
#
# Unoptimised Eigen is 10-70x slower than /O2: loading an A2 model, prewarming it and running
# 8 blocks took 2.6 s in Debug and 72 ms optimised, with identical output. Every Debug test and
# Debug app session that touches NAM paid that. MSVC refuses /O2 alongside /RTC1, and CMake has
# no per-target way to drop a flag from CMAKE_CXX_FLAGS_DEBUG (MSVC_RUNTIME_CHECKS needs 4.0
# and a fresh cache), so the flags are edited here, where only this target sees them.
#
# Turn GUITARFX_OPTIMISE_NAM_IN_DEBUG off to step through NAM itself. The runtime library and
# _ITERATOR_DEBUG_LEVEL stay Debug either way, so the target still links with the rest.

file(GLOB_RECURSE NAM_SOURCES CONFIGURE_DEPENDS "${NeuralAmpModelerCore_SOURCE_DIR}/NAM/*.cpp")
add_library(NeuralAmpModelerCore STATIC ${NAM_SOURCES})
set_target_properties(NeuralAmpModelerCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
guitarfx_enforce_nam_a2_fast(NeuralAmpModelerCore)
guitarfx_disable_linux_gnu_unique(NeuralAmpModelerCore)
target_compile_features(NeuralAmpModelerCore PUBLIC cxx_std_17)
guitarfx_apply_release_optimizations(NeuralAmpModelerCore)

if(GUITARFX_OPTIMISE_NAM_IN_DEBUG)
    if(MSVC)
        string(REGEX REPLACE "[/-](RTC[1csu]+|Od|Ob0)( |$)" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(NeuralAmpModelerCore PRIVATE $<$<CONFIG:Debug>:/O2> $<$<CONFIG:Debug>:/Ob2>)
    else()
        target_compile_options(NeuralAmpModelerCore PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endif()

# Spelled as the core's own path, as it was before this file existed: a different spelling of the
# same directory recompiles everything that inherits it.
get_filename_component(_guitarfx_nam_compat_dir "${CMAKE_CURRENT_LIST_DIR}/../../src/compat" ABSOLUTE)
target_include_directories(NeuralAmpModelerCore PUBLIC
    "${_guitarfx_nam_compat_dir}"
    "${NeuralAmpModelerCore_SOURCE_DIR}"
    "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen")
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(NeuralAmpModelerCore PUBLIC nlohmann_json::nlohmann_json)
else()
    target_include_directories(NeuralAmpModelerCore PUBLIC
        "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/nlohmann")
endif()
