include(FetchContent)

if(GUITARFX_FETCH_DEPENDENCIES)
  message(STATUS "Fetching iPlug2 dependencies")

  # Check for local iPlug2 copy first (with VST3_SDK included)
  set(_local_iplug2 "${CMAKE_CURRENT_SOURCE_DIR}/_deps/iplug2-src")
  if(EXISTS "${_local_iplug2}/IPlug/VST3_SDK" AND NOT DEFINED iPlug2_SOURCE_DIR)
    message(STATUS "Using local iPlug2 from ${_local_iplug2}")
    set(iPlug2_SOURCE_DIR "${_local_iplug2}" CACHE PATH "iPlug2 source directory")
  endif()

  if(DEFINED iPlug2_SOURCE_DIR AND NOT EXISTS "${iPlug2_SOURCE_DIR}/IPlug/APP/IPlugAPP.cpp")
    message(WARNING "Configured iPlug2_SOURCE_DIR=${iPlug2_SOURCE_DIR}, but required sources are missing. Clearing cached iPlug2_SOURCE_DIR to refetch.")
    unset(iPlug2_SOURCE_DIR CACHE)
  endif()

  if(NOT DEFINED iPlug2_SOURCE_DIR)
    # Use FetchContent_Populate instead of FetchContent_MakeAvailable to avoid
    # building iPlug2's Examples and Tests which are added unconditionally
    FetchContent_Declare(
      iPlug2
      GIT_REPOSITORY https://github.com/iPlug2/iPlug2.git
      GIT_TAG master
    )
    FetchContent_GetProperties(iPlug2)
    if(NOT iplug2_POPULATED)
      FetchContent_Populate(iPlug2)
    endif()
    set(iPlug2_SOURCE_DIR "${iplug2_SOURCE_DIR}" CACHE PATH "iPlug2 source directory")
  endif()

  if(NOT EXISTS "${iPlug2_SOURCE_DIR}/IPlug/APP/IPlugAPP.cpp")
    message(FATAL_ERROR "iPlug2 dependency not found at ${iPlug2_SOURCE_DIR}. Ensure git access is available or set iPlug2_SOURCE_DIR to a local iPlug2 checkout with VST3_SDK included.")
  endif()

  # Ensure VST3 SDK is available for VST3 builds (clone into iPlug2 dependency tree if missing).
  set(_vst3_sdk_dir "${iPlug2_SOURCE_DIR}/Dependencies/IPlug/VST3_SDK")
  if(NOT EXISTS "${_vst3_sdk_dir}/base/source/baseiids.cpp")
    find_program(GIT_EXECUTABLE git)
    if(GIT_EXECUTABLE)
      if(EXISTS "${_vst3_sdk_dir}")
        file(REMOVE_RECURSE "${_vst3_sdk_dir}")
      endif()
      message(STATUS "Fetching VST3 SDK into ${_vst3_sdk_dir}")
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone https://github.com/steinbergmedia/vst3sdk.git "${_vst3_sdk_dir}"
        RESULT_VARIABLE _vst3_git_result
      )
      if(NOT _vst3_git_result EQUAL 0)
        message(WARNING "Failed to clone VST3 SDK. Clone https://github.com/steinbergmedia/vst3sdk.git into ${_vst3_sdk_dir} manually.")
      endif()
    else()
      message(WARNING "git not found; cannot auto-fetch VST3 SDK. Clone https://github.com/steinbergmedia/vst3sdk.git into ${_vst3_sdk_dir} manually.")
    endif()
  endif()

  # Patch iPlug2 WebView scaling to avoid DPI issues on Windows.
  set(_iplug2_webview_win "${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebView_win.cpp")
  if(EXISTS "${_iplug2_webview_win}")
    file(READ "${_iplug2_webview_win}" _iplug2_webview_win_contents)
    string(REPLACE "GetScaleForHWND(mParentWnd)" "1.0f" _iplug2_webview_win_patched "${_iplug2_webview_win_contents}")
    if(NOT _iplug2_webview_win_contents STREQUAL _iplug2_webview_win_patched)
      file(WRITE "${_iplug2_webview_win}" "${_iplug2_webview_win_patched}")
      message(STATUS "Patched iPlug2 WebView DPI scaling in ${_iplug2_webview_win}")
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

  # Signalsmith headers are provided transitively by SoundshedGuitarCore.

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
