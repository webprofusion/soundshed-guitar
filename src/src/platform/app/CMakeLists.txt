if(NOT WIN32)
  message(FATAL_ERROR "The standalone application build is currently only supported on Windows.")
endif()

if(NOT DEFINED iPlug2_SOURCE_DIR)
  message(FATAL_ERROR "iPlug2 source directory is not available; ensure dependencies are fetched before enabling the app target.")
endif()

set(_nam_app_sources
  ../../NAMGuitarPlugin.cpp
  ../../../resources/main.rc
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
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebViewEditorDelegate.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebView_win.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/IControl.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/IGraphics.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/IGraphicsEditorDelegate.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/Platforms/IGraphicsWin.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/Controls/IControls.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/Controls/IPopupMenuControl.cpp
  ${iPlug2_SOURCE_DIR}/IGraphics/Controls/ITextEntryControl.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/RtAudio.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/asio.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/asiodrivers.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/asiolist.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTAudio/include/iasiothiscallresolver.cpp
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/RTMidi/RtMidi.cpp
)

add_executable(NAMGuitarFX_App WIN32 ${_nam_app_sources})

target_include_directories(NAMGuitarFX_App
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../resources
)

target_compile_definitions(NAMGuitarFX_App
  PRIVATE
    APP_API
    __WINDOWS_DS__
    __WINDOWS_MM__
    __WINDOWS_ASIO__
)

target_link_libraries(NAMGuitarFX_App
  PRIVATE
    NAMGuitarFXCore
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
  wininet
)

set_target_properties(NAMGuitarFX_App PROPERTIES
  OUTPUT_NAME NAMGuitarFX
  FOLDER "NAMGuitarFX/Formats"
)

add_custom_command(TARGET NAMGuitarFX_App POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NAMGuitarFX_App>/resources"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/resources/ui"
    "$<TARGET_FILE_DIR:NAMGuitarFX_App>/resources/ui"
  COMMENT "Copying web UI assets into the standalone bundle")
