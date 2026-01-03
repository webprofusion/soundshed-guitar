include(FetchContent)

if(NAMGUITAR_FETCH_DEPENDENCIES)
  message(STATUS "Fetching iPlug2 and NeuralAmpModelerCore dependencies")

  # Check for local iPlug2 copy first (with VST3_SDK included)
  set(_local_iplug2 "${CMAKE_CURRENT_SOURCE_DIR}/_deps/iplug2-src")
  if(EXISTS "${_local_iplug2}/IPlug/VST3_SDK" AND NOT DEFINED iPlug2_SOURCE_DIR)
    message(STATUS "Using local iPlug2 from ${_local_iplug2}")
    set(iPlug2_SOURCE_DIR "${_local_iplug2}" CACHE PATH "iPlug2 source directory")
  elseif(NOT TARGET iplug2)
    FetchContent_Declare(
      iPlug2
      GIT_REPOSITORY https://github.com/iPlug2/iPlug2.git
      GIT_TAG master
    )
    FetchContent_MakeAvailable(iPlug2)
    FetchContent_GetProperties(iPlug2 SOURCE_DIR IPLUG2_SOURCE_DIR)
    if(NOT DEFINED iPlug2_SOURCE_DIR)
      set(iPlug2_SOURCE_DIR "${IPLUG2_SOURCE_DIR}")
    endif()
  endif()

  if(NOT TARGET NeuralAmpModelerCore)
    FetchContent_Declare(
      NeuralAmpModelerCore
      GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
      GIT_TAG main
      CMAKE_ARGS -DNAM_BUILD_TOOLS=OFF
    )
    FetchContent_MakeAvailable(NeuralAmpModelerCore)
    FetchContent_GetProperties(NeuralAmpModelerCore SOURCE_DIR NAM_SOURCE_DIR)
    if(NOT DEFINED NeuralAmpModelerCore_SOURCE_DIR)
      set(NeuralAmpModelerCore_SOURCE_DIR "${NAM_SOURCE_DIR}")
    endif()
    if(MSVC AND EXISTS "${NeuralAmpModelerCore_SOURCE_DIR}/NAM/dsp.cpp")
      set_source_files_properties("${NeuralAmpModelerCore_SOURCE_DIR}/NAM/dsp.cpp" PROPERTIES COMPILE_FLAGS "")
      foreach(_tool benchmodel loadmodel run_tests)
        if(TARGET ${_tool})
          set_property(TARGET ${_tool} PROPERTY COMPILE_OPTIONS "")
        endif()
      endforeach()
      foreach(_target benchmodel loadmodel run_tests tools)
        if(TARGET ${_target})
          set_property(TARGET ${_target} PROPERTY EXCLUDE_FROM_ALL TRUE)
          set_property(TARGET ${_target} PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        endif()
      endforeach()
    endif()
  endif()

  if(NOT TARGET nlohmann_json)
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.12.0
    )
    FetchContent_MakeAvailable(nlohmann_json)
  endif()

  if(NOT TARGET httplib::httplib)
    FetchContent_Declare(
      cpp_httplib
      GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
      GIT_TAG v0.15.3
    )
    FetchContent_MakeAvailable(cpp_httplib)
    if(NOT TARGET httplib::httplib)
      add_library(httplib::httplib INTERFACE IMPORTED)
      target_include_directories(httplib::httplib INTERFACE ${cpp_httplib_SOURCE_DIR})
    endif()
  endif()

  if(NOT TARGET WebView2::Loader)
    set(WEBVIEW2_SDK_VERSION "1.0.2903.40" CACHE STRING "Microsoft WebView2 SDK version to fetch" FORCE)
    set(WEBVIEW2_SDK_URL "https://globalcdn.nuget.org/packages/microsoft.web.webview2.${WEBVIEW2_SDK_VERSION}.nupkg" CACHE STRING "Microsoft WebView2 SDK download URL" FORCE)

    FetchContent_Declare(
      WebView2SDK
      URL ${WEBVIEW2_SDK_URL}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    cmake_policy(PUSH)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_GetProperties(WebView2SDK)
    if(NOT WebView2SDK_POPULATED)
      FetchContent_Populate(WebView2SDK)
    endif()
    cmake_policy(POP)

    set(WEBVIEW2_SDK_ROOT "${webview2sdk_SOURCE_DIR}" CACHE PATH "Path to extracted Microsoft WebView2 SDK" FORCE)

    if(NOT EXISTS "${WEBVIEW2_SDK_ROOT}/build/native/include/WebView2.h")
      message(FATAL_ERROR "WebView2 SDK headers were not downloaded correctly. Expected WebView2.h under ${WEBVIEW2_SDK_ROOT}/build/native/include.")
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_webview2_lib_dir "${WEBVIEW2_SDK_ROOT}/build/native/x64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(_webview2_lib_dir "${WEBVIEW2_SDK_ROOT}/build/native/x86")
    else()
      set(_webview2_lib_dir "${WEBVIEW2_SDK_ROOT}/build/native/arm64")
    endif()

    if(NOT EXISTS "${_webview2_lib_dir}/WebView2LoaderStatic.lib")
      message(FATAL_ERROR "WebView2 SDK static loader library not found under ${_webview2_lib_dir}.")
    endif()

    add_library(WebView2::Loader STATIC IMPORTED)
    set_target_properties(WebView2::Loader PROPERTIES
      IMPORTED_LOCATION "${_webview2_lib_dir}/WebView2LoaderStatic.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${WEBVIEW2_SDK_ROOT}/build/native/include"
    )
  endif()

  if(NOT TARGET wil::headers)
    set(WIL_SDK_VERSION "1.0.240803.1" CACHE STRING "Microsoft Windows Implementation Library version to fetch" FORCE)
    set(WIL_SDK_URL "https://globalcdn.nuget.org/packages/microsoft.windows.implementationlibrary.${WIL_SDK_VERSION}.nupkg" CACHE STRING "Microsoft Windows Implementation Library download URL" FORCE)

    FetchContent_Declare(
      WILSDK
      URL ${WIL_SDK_URL}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    cmake_policy(PUSH)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_GetProperties(WILSDK)
    if(NOT WILSDK_POPULATED)
      FetchContent_Populate(WILSDK)
    endif()
    cmake_policy(POP)

    set(WIL_SDK_ROOT "${wilsdk_SOURCE_DIR}" CACHE PATH "Path to extracted Microsoft Windows Implementation Library" FORCE)

    if(NOT EXISTS "${WIL_SDK_ROOT}/include/wil/com.h")
      message(FATAL_ERROR "Windows Implementation Library headers not found at ${WIL_SDK_ROOT}/include. Expected wil/com.h to be present.")
    endif()

    add_library(wil::headers INTERFACE IMPORTED)
    set_target_properties(wil::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${WIL_SDK_ROOT}/include"
    )
  endif()
endif()
