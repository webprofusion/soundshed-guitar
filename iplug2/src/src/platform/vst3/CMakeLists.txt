# VST3 wrapper target for GuitarFX plugin

if(NOT DEFINED iPlug2_SOURCE_DIR)
  message(FATAL_ERROR "iPlug2 source directory is not available; ensure dependencies are fetched before enabling the VST3 target.")
endif()

# Define VST3 SDK root for cleaner path references
set(VST3_SDK_ROOT "${iPlug2_SOURCE_DIR}/Dependencies/IPlug/VST3_SDK")

# Validate VST3 SDK presence (iPlug2 does not bundle it by default)
if(NOT EXISTS "${VST3_SDK_ROOT}/base/source/baseiids.cpp")
  message(FATAL_ERROR
    "VST3 SDK not found under ${VST3_SDK_ROOT}. "
    "Run the iPlug2 VST3 SDK download script (Dependencies/IPlug/download-vst3-sdk.sh) "
    "or point iPlug2_SOURCE_DIR to a checkout that includes the VST3 SDK."
  )
endif()

# VST3 SDK source files
set(_vst3_sdk_sources
  ${VST3_SDK_ROOT}/base/source/baseiids.cpp
  ${VST3_SDK_ROOT}/base/source/fbuffer.cpp
  ${VST3_SDK_ROOT}/base/source/fdebug.cpp
  ${VST3_SDK_ROOT}/base/source/fobject.cpp
  ${VST3_SDK_ROOT}/base/source/fstreamer.cpp
  ${VST3_SDK_ROOT}/base/source/fstring.cpp
  ${VST3_SDK_ROOT}/base/source/updatehandler.cpp
  ${VST3_SDK_ROOT}/base/thread/source/flock.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/conststringtable.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp
  ${VST3_SDK_ROOT}/pluginterfaces/base/ustring.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/common/commoniids.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/common/memorystream.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/common/pluginview.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstbus.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstcomponent.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstcomponentbase.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstparameters.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/main/pluginfactory.cpp
  ${VST3_SDK_ROOT}/public.sdk/source/main/dllmain.cpp
)

# iPlug2 core sources
set(_iplug2_core_sources
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugAPIBase.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugParameter.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugPaths.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugPluginBase.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugProcessor.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/IPlugTimer.cpp
)

# iPlug2 VST3-specific sources
set(_iplug2_vst3_sources
  ${iPlug2_SOURCE_DIR}/IPlug/VST3/IPlugVST3.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/VST3/IPlugVST3_ProcessorBase.cpp
)

# WebView-only editor delegate (no IGraphics)
# This eliminates the complexity of IGraphics + IWebViewControl and uses
# the simpler WebViewEditorDelegate which directly owns the WebView.
set(_webview_sources
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebViewEditorDelegate.cpp
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView/IPlugWebView_win.cpp
  # WDL UTF8 support for Windows path/file operations
  ${iPlug2_SOURCE_DIR}/WDL/win32_utf8.c
)

set(_nam_vst3_sources
  GuitarFX_VST3.cpp
  ../../GuitarFXPluginAdapter.cpp
  ${_vst3_sdk_sources}
  ${_iplug2_core_sources}
  ${_iplug2_vst3_sources}
  ${_webview_sources}
)

add_library(SoundshedGuitar_VST3 MODULE ${_nam_vst3_sources})

target_compile_definitions(SoundshedGuitar_VST3 PRIVATE 
  VST3_API 
  IPLUG_EDITOR=1 
  IPLUG_DSP=1
  # WebView-only mode: simpler delegate without IGraphics overhead
  WEBVIEW_EDITOR_DELEGATE
  NO_IGRAPHICS
)

# Include all paths needed for compiling GuitarFXPlugin.cpp with VST3_API
# Note: IGraphics directories are still included for header compatibility but IGraphics is not compiled
target_include_directories(SoundshedGuitar_VST3 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../config
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../resources
  ${iPlug2_SOURCE_DIR}/IPlug
  ${iPlug2_SOURCE_DIR}/IPlug/APP
  ${iPlug2_SOURCE_DIR}/IPlug/Extras
  ${iPlug2_SOURCE_DIR}/IPlug/Extras/WebView
  ${iPlug2_SOURCE_DIR}/IPlug/VST3
  ${VST3_SDK_ROOT}
  ${VST3_SDK_ROOT}/base/source
  ${VST3_SDK_ROOT}/base/thread/include
  ${VST3_SDK_ROOT}/pluginterfaces
  ${VST3_SDK_ROOT}/public.sdk/source
  ${iPlug2_SOURCE_DIR}/WDL
  ${NeuralAmpModelerCore_SOURCE_DIR}
  ${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/nlohmann
)

target_link_libraries(SoundshedGuitar_VST3
  PRIVATE
    GuitarFXCore
    WebView2::Loader
    wil::headers
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
  target_compile_options(SoundshedGuitar_VST3 PRIVATE /wd4068 /bigobj)
endif()

set_target_properties(SoundshedGuitar_VST3 PROPERTIES
  OUTPUT_NAME "Soundshed Guitar"
  FOLDER GuitarFX/Formats
)

# Create VST3 bundle structure
# VST3 on Windows expects:
# <name>.vst3/Contents/x86_64-win/<name>.vst3 (the DLL)
# <name>.vst3/Contents/Resources/ (resources)

set(VST3_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Soundshed Guitar.vst3")
set(VST3_CONTENTS_DIR "${VST3_BUNDLE_DIR}/Contents")
set(VST3_ARCH_DIR "${VST3_CONTENTS_DIR}/x86_64-win")
set(VST3_RESOURCES_DIR "${VST3_CONTENTS_DIR}/Resources")

# Post-build commands to create bundle
add_custom_command(TARGET SoundshedGuitar_VST3 POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -D SOURCE_DLL="$<TARGET_FILE:SoundshedGuitar_VST3>"
    -D DEST_BUNDLE_DIR="${VST3_BUNDLE_DIR}"
    -D RESOURCE_DIR="${CMAKE_SOURCE_DIR}/../../core/ui"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/CopyVst3Bundle.cmake"
  COMMENT "Updating VST3 bundle structure"
)

message(STATUS "VST3 bundle will be created at: ${VST3_BUNDLE_DIR}")
