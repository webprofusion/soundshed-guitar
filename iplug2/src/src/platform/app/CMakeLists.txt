if(NOT WIN32)
  message(FATAL_ERROR "The standalone application build is currently only supported on Windows.")
endif()

if(NOT DEFINED iPlug2_SOURCE_DIR)
  message(FATAL_ERROR "iPlug2 source directory is not available; ensure dependencies are fetched before enabling the app target.")
endif()

set(_nam_app_sources
  ../../GuitarFXPluginAdapter.cpp
  resources/main.rc
  ${iPlug2_SOURCE_DIR}/IPlug/APP/IPlugAPP.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/APP/IPlugAPP_dialog.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/APP/IPlugAPP_host.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/APP/IPlugAPP_main.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugAPIBase.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugParameter.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugPaths.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugPluginBase.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugProcessor.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugTimer.cpp
  # WebView-only editor delegate (no IGraphics)
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebViewEditorDelegate.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebView_win.cpp
  # WDL UTF8 support for Windows path/file operations
  ${iPlug2_SOURCE_DIR}/WDL/win32_utf8.c
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/RtAudio.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/asio.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/asiodrivers.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/asiolist.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/iasiothiscallresolver.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTMidi/RtMidi.cpp
)

add_executable(SoundshedGuitar_App WIN32 ${_nam_app_sources})

target_include_directories(SoundshedGuitar_App
  PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/resources
    ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio
    ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include
    ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTMidi
)

target_compile_definitions(SoundshedGuitar_App
  PRIVATE
    APP_API
    __WINDOWS_DS__
    __WINDOWS_MM__
    __WINDOWS_ASIO__
    # WebView-only mode: simpler delegate without IGraphics overhead
    WEBVIEW_EDITOR_DELEGATE
    NO_IGRAPHICS
)

target_link_libraries(SoundshedGuitar_App
  PRIVATE
    GuitarFXCore
    WebView2::Loader
    wil::headers
    dsound
    winmm
    windowsapp
    shlwapi
    comctl32
    ole32
    oleaut32
    uuid
    shell32
    user32
    gdi32
    advapi32
    windowscodecs
  wininet
)

if(MSVC)
  target_compile_options(SoundshedGuitar_App PRIVATE /wd4068 /bigobj /FS)
endif()

set_target_properties(SoundshedGuitar_App PROPERTIES
  OUTPUT_NAME "Soundshed Guitar"
  FOLDER "GuitarFX/Formats"
)

add_custom_command(TARGET SoundshedGuitar_App POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -D SOURCE_DIR="${CMAKE_SOURCE_DIR}/../../core/ui"
    -D DEST_DIR="$<TARGET_FILE_DIR:SoundshedGuitar_App>/resources"
    -P "${CMAKE_SOURCE_DIR}/cmake/CopyAppResources.cmake"
  COMMENT "Copying resource assets into the standalone bundle")
