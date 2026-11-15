// Soundshed Nano Q for Android. The app is one activity (NanoQActivity) around libnanoq.so,
// which nanoq/CMakeLists.txt builds with nanoq/android/cmake/NanoQAndroid.cmake: Nano Q's CLAP
// plugin, hosted in-process, drawn with Elements' Android host and played through Oboe.
// See docs/plans/nano-q-android.md.

plugins {
    id("com.android.application")
}

/** Nano Q's version, from project(SoundshedNanoQ VERSION x.y.z) in nanoq/CMakeLists.txt. */
val nanoqVersion: String = Regex("""project\(SoundshedNanoQ VERSION ([0-9.]+)""")
    .find(rootProject.file("../CMakeLists.txt").readText())?.groupValues?.get(1) ?: "0.0.0"

/** Skia for Android, from nanoq/android/build-skia.ps1 (or build-skia.sh). */
val skiaDir = rootProject.file("skia/arm64-android-nanoq")

/** Where CMake stages the fonts and engine data that become assets/nanoq (NanoQAndroid.cmake). */
val stagedAssets = layout.buildDirectory.dir("nanoq-assets")

android {
    namespace = "com.soundshed.nanoq"
    compileSdk = 36
    // The NDK Soundshed Guitar builds with, and the one nanoq/android/build-skia.sh builds Skia
    // with: one clang and one libc++ for both.
    ndkVersion = "27.3.13750724"

    defaultConfig {
        // NANOQ_ID in nanoq/CMakeLists.txt.
        applicationId = "com.soundshed.guitar.nanoq"
        // As Soundshed Guitar: API 29 has the mature AAudio path that low-latency input needs.
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = nanoqVersion

        // Skia is built for arm64-v8a only.
        ndk { abiFilters += "arm64-v8a" }

        externalNativeBuild {
            cmake {
                targets += "nanoq"
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DNANOQ_ANDROID_SKIA=${skiaDir.absolutePath.replace('\\', '/')}",
                    "-DNANOQ_ANDROID_ASSET_DIR=${stagedAssets.get().asFile.absolutePath.replace('\\', '/')}",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildFeatures {
        // Oboe arrives as a prefab package.
        prefab = true
    }

    buildTypes {
        debug {
            isJniDebuggable = true
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
            // AGP configures CMake as RelWithDebInfo (-O2) for a release variant; Release (-O3)
            // as Soundshed Guitar's Android build does (android/app/build.gradle.kts).
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                }
            }
            // Signed with the debug key so a release build can be installed and played through:
            // for measuring on a device, not for distribution.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            assets.directories.add(stagedAssets.get().asFile.absolutePath)
            // Java shared with Soundshed Guitar's Android app (com.soundshed.audio).
            java.directories.add(rootDir.resolve("../../android/shared/java").canonicalPath)
        }
    }

    packaging {
        // Load libnanoq.so straight out of the APK rather than unpacking a second copy.
        jniLibs.useLegacyPackaging = false
    }
}

dependencies {
    implementation("com.google.oboe:oboe:1.9.3")
}

// The staged assets are written by the native build's post-build step, so assets are merged
// after it.
tasks.configureEach {
    val merge = Regex("""merge(\w+)Assets""").matchEntire(name)
    if (merge != null) {
        dependsOn("externalNativeBuild${merge.groupValues[1]}")
    }
}

// A clear failure before CMake runs, rather than a find_package error deep in its log.
tasks.configureEach {
    if (name.startsWith("configureCMake") || name.startsWith("buildCMake")) {
        doFirst {
            if (!skiaDir.resolve("share/unofficial-skia/unofficial-skia-config.cmake").isFile) {
                throw GradleException(
                    "Skia for Android is missing at $skiaDir. Build it first: nanoq\\android\\build-skia.ps1"
                )
            }
        }
    }
}
