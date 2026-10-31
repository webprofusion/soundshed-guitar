# vcpkg triplet for the Skia that Nano Q's iOS build draws with (nanoq/ios/build-skia.sh):
# static arm64 libraries, release only, for iOS 12, the oldest the app supports (the
# legacy build for the iPad mini 2). One Skia serves both iOS builds: a library may be
# built for an older iOS than the app that links it.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)

# Skia's gn takes these as extra_cflags, for its C and C++ only; build-skia.sh gives it
# ios_min_target as well, which covers its Objective-C++ (the Metal code).
set(VCPKG_C_FLAGS "-mios-version-min=12.0")
set(VCPKG_CXX_FLAGS "-mios-version-min=12.0")
