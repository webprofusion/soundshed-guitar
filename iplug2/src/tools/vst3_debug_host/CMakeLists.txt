# VST3 Debug Host
# A minimal VST3 host for diagnosing plugin initialization issues

cmake_minimum_required(VERSION 3.20)

project(VST3DebugHost)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get iPlug2 path from parent (iPlug2_SOURCE_DIR is set by FetchDependencies.cmake)
if(DEFINED iPlug2_SOURCE_DIR)
    set(IPLUG2_DIR "${iPlug2_SOURCE_DIR}")
elseif(NOT DEFINED IPLUG2_DIR)
    set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../_deps/iplug2-src")
endif()

set(VST3_SDK_DIR "${IPLUG2_DIR}/Dependencies/IPlug/VST3_SDK")

# Skip building if VST3 SDK not available
if(NOT EXISTS "${VST3_SDK_DIR}/pluginterfaces")
    message(WARNING "VST3 SDK not found at ${VST3_SDK_DIR} - skipping VST3DebugHost")
    return()
endif()

add_executable(VST3DebugHost
    main.cpp
    # VST3 SDK source files for interface definitions
    "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
    "${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
    "${VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp"
)

target_include_directories(VST3DebugHost PRIVATE
    "${VST3_SDK_DIR}"
)

# Define required macros for VST3 SDK
target_compile_definitions(VST3DebugHost PRIVATE
    DEVELOPMENT=1
    RELEASE=0
)

# Link Windows libraries
target_link_libraries(VST3DebugHost PRIVATE
    ole32
    uuid
)

# Set output directory
set_target_properties(VST3DebugHost PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/tools"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tools"
)
