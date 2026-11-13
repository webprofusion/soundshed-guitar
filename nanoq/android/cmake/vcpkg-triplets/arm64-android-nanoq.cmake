# vcpkg triplet for the Skia that Nano Q's Android app draws with (nanoq/android/build-skia.sh):
# static arm64-v8a libraries, release only, for API 29 (the app's minSdk, as Soundshed Guitar's),
# on libc++ shared (VCPKG_CRT_LINKAGE dynamic gives ANDROID_STL=c++_shared), as the app links it.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 29)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a)
