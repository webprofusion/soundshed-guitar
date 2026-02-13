# GuitarFXCore static library containing the shared plugin implementation.
# Format-specific targets in the platform subdirectory link against this library.

# GuitarFXCore contains API-agnostic shared code. GuitarFXPlugin.cpp 
# and GuitarFXPlugin.h are NOT included here because they require format-
# specific API definitions (APP_API, VST3_API, etc.). Each format target
# compiles GuitarFXPlugin.cpp directly with its own API definition.
#
# Core DSP, presets, resources, models, network, and utilities are now
# provided by the SoundshedGuitarCore shared library (added via
# add_subdirectory in the top-level CMakeLists.txt).  GuitarFXCore only
# compiles platform helpers.

set(_guitarfx_core_sources
  # Platform-specific helpers
  $<$<PLATFORM_ID:Windows>:platform/WebViewDPIHelper_win.cpp>
  $<$<PLATFORM_ID:Windows>:platform/WebViewDPIHelper_win.h>
)

add_library(GuitarFXCore STATIC ${_guitarfx_core_sources})

target_include_directories(GuitarFXCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../config
    ${iPlug2_SOURCE_DIR}/IPlug
    ${iPlug2_SOURCE_DIR}/IPlug/APP
    ${iPlug2_SOURCE_DIR}/IPlug/Extras
    ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView
    ${iPlug2_SOURCE_DIR}/IPlug/VST3_SDK
    ${iPlug2_SOURCE_DIR}/WDL
)

target_link_libraries(GuitarFXCore
  PUBLIC
    SoundshedGuitarCore
    nlohmann_json::nlohmann_json
)

target_compile_definitions(GuitarFXCore
  PUBLIC
    IPLUG_EDITOR=1
    IPLUG_DSP=1
    SAMPLE_TYPE_FLOAT
    # Prevent Windows min/max macros from conflicting with std::numeric_limits
    NOMINMAX
    # Note: APP_API is NOT defined here. Format-specific targets (standalone,
    # VST3, etc.) must define their own API macro. The core library compiles
    # GuitarFXPlugin.cpp as a static library without API-specific code.
)

if(WIN32)
  target_compile_definitions(GuitarFXCore PUBLIC _WIN32 WIN32 _WINDOWS)
  # GuitarFXPlugin.cpp heavily uses nlohmann/json templates which can exceed
  # the default COFF section limit on MSVC.
  target_compile_options(GuitarFXCore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
endif()

set_target_properties(GuitarFXCore PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  FOLDER "GuitarFX"
)

add_subdirectory(platform)
