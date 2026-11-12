# ── Soundshed Nano Q for iOS and iPadOS ─────────────────────────────────────────────
# Included by nanoq/CMakeLists.txt in an iOS build, in place of the plugin formats. One app,
# universal for iPhone and iPad, that hosts Nano Q's CLAP plugin linked into it (ClapHost) and
# plays it through RemoteIO (AudioIO). See docs/plans/nano-q-ios.md; build with nanoq/ios/build.sh.

set(_nanoq_ios "${CMAKE_CURRENT_SOURCE_DIR}/ios")

set(_nanoq_ios_sources
    "${_nanoq_ios}/app/NanoQApp.mm"
    "${_nanoq_ios}/app/ClapHost.mm"
    "${_nanoq_ios}/app/AudioIO.mm"
    "${_nanoq_ios}/app/AppLog.mm"
    "${_nanoq_ios}/app/AppLog.h"
    "${_nanoq_ios}/app/ClapHost.h"
    "${_nanoq_ios}/app/AudioIO.h"
    # The CLAP entry the app calls, as clap-wrapper compiles it into each format on the desktop.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/clap_entry.cpp")
set(_nanoq_ios_launch "${_nanoq_ios}/app/LaunchScreen.storyboard")
# The app icon (made by tools/gen-nanoq-icons.mjs). Xcode compiles the catalog with actool, which
# adds CFBundleIcons to the Info.plist itself.
set(_nanoq_ios_icons "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/AppIcon.xcassets")

add_executable(nanoq_ios MACOSX_BUNDLE ${_nanoq_ios_sources} ${_nanoq_ios_launch} ${_nanoq_ios_icons})
set_source_files_properties("${_nanoq_ios_icons}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties(
    "${_nanoq_ios}/app/NanoQApp.mm" "${_nanoq_ios}/app/ClapHost.mm" "${_nanoq_ios}/app/AudioIO.mm"
    "${_nanoq_ios}/app/AppLog.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# Log.h, for nanoq::Log (AppLog.mm).
target_include_directories(nanoq_ios PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(nanoq_ios PRIVATE NANOQ_VERSION="${PROJECT_VERSION}")

# A legacy (iOS 12) build asks for 256 frames, as Soundshed Guitar's does (docs/ios-build.md,
# "Buffer size"): an A7 glitched at less.
if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 13.0)
    target_compile_definitions(nanoq_ios PRIVATE NANOQ_IOS_BUFFER_FRAMES=256)
endif()

target_link_libraries(nanoq_ios PRIVATE nanoq_impl
    "-framework UIKit" "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio"
    "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics" "-framework CoreText"
    "-framework ImageIO")

set_target_properties(nanoq_ios PROPERTIES
    OUTPUT_NAME "${NANOQ_NAME}"
    MACOSX_BUNDLE_INFO_PLIST "${_nanoq_ios}/app/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${NANOQ_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "${NANOQ_NAME}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    # Xcode compiles the storyboard into the bundle's Copy Bundle Resources phase.
    RESOURCE "${_nanoq_ios_launch}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${NANOQ_ID}"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS "YES")

# ── Fonts and runtime data, in the bundle ───────────────────────────────────────────
# An iOS bundle is flat: the executable at its root and its resources beside it. The fonts go
# there, where the UIKit host registers them ([NSBundle resourcePath]); the engine's data goes to
# ui/ there, which Paths.cpp looks for beside the executable. Not in a folder called resources,
# which makes codesign take the bundle for a macOS one (docs/ios-build.md).
set(_nanoq_ios_fonts ${ELEMENTS_FONTS} "${_nanoq_elements}/resources/fonts/elements_basic.ttf")
set(_dest "$<TARGET_BUNDLE_DIR:nanoq_ios>")
set(_commands)
foreach(_font IN LISTS _nanoq_ios_fonts)
    list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_font}" "${_dest}")
endforeach()
foreach(_dir IN LISTS _NANOQ_UI_DATA_DIRS)
    list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/${_dir}" "${_dest}/ui/${_dir}")
endforeach()
# Not VERBATIM: the bundle's path holds Xcode's ${EFFECTIVE_PLATFORM_NAME} (-iphonesimulator or
# -iphoneos), which VERBATIM would escape and Xcode would then never expand, as in
# juce/CMakeLists.txt's soundshed_copy_ui_resources.
add_custom_command(TARGET nanoq_ios POST_BUILD ${_commands}
    COMMENT "Copying fonts and engine data into the Nano Q app")
