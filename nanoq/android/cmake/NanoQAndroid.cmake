# ── Soundshed Nano Q for Android ────────────────────────────────────────────────────
# Included by nanoq/CMakeLists.txt in an Android build, in place of the plugin formats. One shared
# library, libnanoq.so, that NanoQActivity loads: it hosts Nano Q's CLAP plugin linked into it
# (ClapHost), plays it through Oboe (AudioIO) and draws its editor with Elements' Android host
# (nanoq/android/elements_host). See docs/plans/nano-q-android.md; Gradle (nanoq/android) builds it.

set(_nanoq_android "${CMAKE_CURRENT_SOURCE_DIR}/android")

# Oboe comes from Gradle, as a prefab package (com.google.oboe:oboe in app/build.gradle.kts).
find_package(oboe REQUIRED CONFIG)

add_library(nanoq SHARED
    "${_nanoq_android}/native/NanoQJni.cpp"
    "${_nanoq_android}/native/ClapHost.cpp"
    "${_nanoq_android}/native/AudioIO.cpp"
    "${_nanoq_android}/native/AppLog.cpp"
    # Moves the audio callback off the little cores (ADPF, else affinity): shared with Soundshed
    # Guitar's Android app, plain C++ with no JUCE in it.
    "${CMAKE_CURRENT_SOURCE_DIR}/../juce/source/AndroidAudioThread.cpp"
    # The CLAP entry the app calls, as clap-wrapper compiles it into each format on the desktop.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/clap_entry.cpp")
# Log.h, for nanoq::Log (AppLog.cpp).
target_include_directories(nanoq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../juce/source")
target_compile_definitions(nanoq PRIVATE NANOQ_VERSION="${PROJECT_VERSION}")
target_link_libraries(nanoq PRIVATE nanoq_impl oboe::oboe android log)
# A 16 KB page size, which Android 15 devices may use and Play requires of new apps.
target_link_options(nanoq PRIVATE "-Wl,-z,max-page-size=16384")

# Where Gradle packages the library from (AGP passes it as CMAKE_LIBRARY_OUTPUT_DIRECTORY).
if(CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set_target_properties(nanoq PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
endif()

# ── Fonts and runtime data, into the APK's assets ────────────────────────────────────
# An APK's assets are not files the engine can open, so NanoQActivity unpacks assets/nanoq into
# its data folder and names that folder SOUNDSHED_NANOQ_RESOURCES. The fonts sit at its top,
# where the Android host has Artist find them; the engine's data in ui/ there, which Paths.cpp
# takes as the resource root. Gradle passes the folder it packages (NANOQ_ANDROID_ASSET_DIR) and
# merges assets only after this library is built.
set(NANOQ_ANDROID_ASSET_DIR "" CACHE PATH "Where the APK's assets are staged (set by nanoq/android/app/build.gradle.kts)")
if(NANOQ_ANDROID_ASSET_DIR)
    set(_dest "${NANOQ_ANDROID_ASSET_DIR}/nanoq")
    set(_commands COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}")
    foreach(_font IN LISTS ELEMENTS_FONTS)
        list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_font}" "${_dest}")
    endforeach()
    list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_nanoq_elements}/resources/fonts/elements_basic.ttf" "${_dest}")
    foreach(_dir IN LISTS _NANOQ_UI_DATA_DIRS)
        list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/${_dir}" "${_dest}/ui/${_dir}")
    endforeach()
    add_custom_command(TARGET nanoq POST_BUILD ${_commands}
        COMMENT "Staging fonts and engine data for the Nano Q APK" VERBATIM)
endif()
