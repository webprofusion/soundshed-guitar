import com.android.build.api.variant.ApplicationAndroidComponentsExtension
import org.gradle.api.DefaultTask
import org.gradle.api.file.ConfigurableFileCollection
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.FileSystemOperations
import org.gradle.api.provider.ListProperty
import org.gradle.api.provider.Property
import org.gradle.api.tasks.CacheableTask
import org.gradle.api.tasks.Input
import org.gradle.api.tasks.InputFiles
import org.gradle.api.tasks.Internal
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import java.security.MessageDigest
import javax.inject.Inject

plugins {
    id("com.android.application")
}

val juceModules = rootProject.file("../juce/JUCE/modules")
val uiSrcDir = rootProject.file("../core/ui")

/** ABIs to build, from the ssg.abis property (see gradle.properties). */
val targetAbis: List<String> =
    (project.findProperty("ssg.abis") as String? ?: "arm64-v8a")
        .split(",").map { it.trim() }.filter { it.isNotEmpty() }

/** Kept in step with juce/VERSION so the APK matches the desktop build. */
val appVersionName: String = rootProject.file("../juce/VERSION").readText().trim()

android {
    namespace = "com.soundshed.guitar"
    compileSdk = 36
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "com.soundshed.guitar"
        // 29 is a hard floor, not a preference: JUCE 8's Android font backend
        // (juce_Fonts_android.cpp) is built on the AFontMatcher API added in
        // API 29. Its own calls are guarded with __builtin_available, but the
        // AFontMatcher_destroy / AFont_close deleters get instantiated through
        // unique_ptr outside any guard, so the module does not compile below 29.
        //
        // No great loss: API 29 also has the mature AAudio path that makes
        // low-latency guitar input viable.
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = appVersionName

        ndk { abiFilters += targetAbis }

        externalNativeBuild {
            cmake {
                targets += "SoundshedGuitar_Standalone"
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildTypes {
        debug {
            isJniDebuggable = true
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false

            // Signed with the debug key so a release build can actually be
            // installed and played through. This is for measuring the real
            // thing on a device — a debug build compiles the DSP at -O0, which
            // is nowhere near fast enough to judge latency or even to run
            // cleanly — not for distribution. Shipping needs a real keystore
            // and this line replaced.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            // JUCE ships the Java half of its Android backend as plain source
            // inside each module. The Projucer copies these into generated
            // projects; compiling them in place means there is no second copy
            // to keep in sync.
            //
            // Only the modules this app links are listed. juce_gui_extra's
            // javaopt directory is deliberately excluded: it holds the Firebase
            // push-notification services, which would drag in the Firebase SDK
            // for a feature this app does not use.
            java.srcDirs(
                "src/main/java",
                "$juceModules/juce_core/native/java/app",
                "$juceModules/juce_core/native/javacore/app",
                "$juceModules/juce_core/native/javacore/init",
                "$juceModules/juce_gui_basics/native/java/app",
                // javaopt here supplies Receiver and JuceSharingContentProvider,
                // which juce_ContentSharer_android.cpp looks up by name. It also
                // contains JuceActivity — see MainActivity for why nothing may
                // extend that class in a build without push notifications.
                "$juceModules/juce_gui_basics/native/javaopt/app",
                // NOTE: juce_gui_extra/native/java is deliberately absent. Its
                // only file is JuceWebViewClasses.java, and we ship a patched
                // copy under src/main/java/com/rmsl/juce/ that enables DOM
                // storage. Adding the upstream directory back would be a
                // duplicate-class error. See checkVendoredJuceWebView below.
                "$juceModules/juce_audio_devices/native/java/app",
                "$juceModules/juce_audio_devices/native/javaopt/app",
            )
        }
    }

    packaging {
        // Store libjuce_jni.so uncompressed and load it straight out of the
        // APK rather than unpacking a second copy on install.
        jniLibs.useLegacyPackaging = false
    }


}

// ---------------------------------------------------------------------------
// Web UI packaging.
//
// The editor resolves its resources as files under "<root>/ui/...". On Android
// there is no directory next to the binary, so the same tree is packaged as APK
// assets under "ui/" and unpacked to the app's data dir on first run
// (see SoundshedApp.java).
// ---------------------------------------------------------------------------

/** Subdirectories of core/ui that ship with the app. */
val uiAssetDirs = listOf(
    "dist", "css", "images", "data", "assets", "metronome",
    "ui-components", "presets", "demo",
)

@CacheableTask
abstract class StageUiAssets @Inject constructor(
    private val fs: FileSystemOperations,
) : DefaultTask() {

    /**
     * Where core/ui lives. Not an @InputDirectory: that whole tree includes
     * node_modules, and Gradle would fingerprint every file in it on each
     * build. [sourceFiles] declares the parts that actually ship instead.
     */
    @get:Internal
    abstract val uiSource: DirectoryProperty

    /** The subset of core/ui that is packaged — the real up-to-date inputs. */
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val sourceFiles: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:Input
    abstract val subdirectories: ListProperty<String>

    @get:Input
    abstract val stamp: Property<String>

    @get:Input
    abstract val jamEnabled: Property<Boolean>

    @TaskAction
    fun stage() {
        val src = uiSource.get().asFile
        val dest = outputDir.get().asFile.resolve("ui")

        fs.sync {
            into(dest)
            from(src) { include("index.html") }
            subdirectories.get().forEach { dir ->
                from(File(src, dir)) { into(dir) }
            }
        }

        // index.html loads build-flags.js unconditionally. On desktop CMake
        // generates it from juce/cmake/ui-build-flags.js.in; nothing in core/ui
        // produces it, so without this the Android build 404s on every launch and
        // the UI runs with its build flags undefined.
        dest.resolve("build-flags.js").writeText(
            """
            window.SOUNDSHED_BUILD_FLAGS = Object.freeze({
              jamEnabled: ${jamEnabled.get()}
            });

            if (document.documentElement) {
              document.documentElement.dataset.jamEnabled = window.SOUNDSHED_BUILD_FLAGS.jamEnabled ? "true" : "false";
            }
            """.trimIndent() + "\n"
        )

        // The runtime compares this against its unpacked copy to decide whether
        // the assets need re-extracting, so it has to change whenever the staged
        // content does — a version-derived stamp is not enough, because during
        // development the version stays put while the UI changes underneath it,
        // and the device then keeps serving a stale copy that looks correct in
        // every build output.
        //
        // Not dot-prefixed: aapt's default ignoreAssetsPattern drops anything
        // matching ".*", so a ".asset-stamp" would silently never reach the APK
        // and every launch would re-extract instead.
        val digest = MessageDigest.getInstance("SHA-256")
        digest.update(stamp.get().toByteArray())

        dest.walkTopDown()
            .filter { it.isFile }
            .sortedBy { it.absolutePath }
            .forEach { file ->
                digest.update(file.toRelativeString(dest).toByteArray())
                digest.update(file.readBytes())
            }

        val contentStamp = digest.digest().joinToString("") { byte -> "%02x".format(byte) }
        dest.resolve("asset-stamp.txt").writeText("${stamp.get()}-$contentStamp")
    }
}

/**
 * Builds the TypeScript UI so dist/ is fresh, mirroring the desktop CMake step.
 *
 * `npm run build` is two steps — tsc into dist/, then scripts/assemble-html.js
 * expanding index.template.html (plus the ui-components/ partials it inlines)
 * into index.html. Both halves have to be declared, or a template-only edit
 * leaves the task up-to-date and the stale index.html gets packaged.
 */
val buildUiBundle = tasks.register<Exec>("buildUiBundle") {
    description = "Runs npm run build in core/ui."
    group = "build"
    workingDir = uiSrcDir
    val npm = if (System.getProperty("os.name").startsWith("Windows")) "npm.cmd" else "npm"
    commandLine(npm, "run", "build")

    inputs.dir(File(uiSrcDir, "ts")).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.dir(File(uiSrcDir, "ui-components")).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.dir(File(uiSrcDir, "scripts")).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.file(File(uiSrcDir, "index.template.html")).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.file(File(uiSrcDir, "tsconfig.json")).withPathSensitivity(PathSensitivity.RELATIVE)

    outputs.dir(File(uiSrcDir, "dist"))
    outputs.file(File(uiSrcDir, "index.html"))
}

val stageUiAssets = tasks.register<StageUiAssets>("stageUiAssets") {
    description = "Copies the built web UI into the APK asset staging directory."
    group = "build"
    dependsOn(buildUiBundle)
    uiSource.set(uiSrcDir)
    subdirectories.set(uiAssetDirs)
    // Mirrors GUITARFX_ENABLE_JAM, which defaults to ON in juce/CMakeLists.txt.
    jamEnabled.set((project.findProperty("ssg.jam") as String? ?: "true").toBoolean())
    sourceFiles.from(File(uiSrcDir, "index.html"))
    uiAssetDirs.forEach { sourceFiles.from(File(uiSrcDir, it)) }
    // Version-derived so an upgrade forces a re-extract on device.
    stamp.set("$appVersionName-${uiAssetDirs.size}")
}

extensions.configure<ApplicationAndroidComponentsExtension> {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(stageUiAssets, StageUiAssets::outputDir)
    }
}

// ---------------------------------------------------------------------------
// Vendored-JUCE drift check.
//
// src/main/java/com/rmsl/juce/JuceWebViewClasses.java is a patched copy of a
// JUCE file (it enables DOM storage, which upstream leaves off and the UI needs).
// JUCE resolves the class from the app's class loader first, so our copy wins.
//
// The hazard is a JUCE upgrade silently changing the original while we keep
// shipping an old fork. Pin the upstream file's hash: if it moves, the build
// fails and whoever bumped JUCE re-applies the one-line patch on top of the new
// version and updates the hash here.
// ---------------------------------------------------------------------------
val vendoredJuceWebViewUpstream =
    File(juceModules, "juce_gui_extra/native/java/app/com/rmsl/juce/JuceWebViewClasses.java")

/** SHA-256 of the upstream file this fork was taken from (JUCE 8.0.14). */
val vendoredJuceWebViewUpstreamSha =
    "bea03778134b46da7387b318e6eba43569f89c1bd430996ba45c37039c194213"

val checkVendoredJuceWebView = tasks.register("checkVendoredJuceWebView") {
    description = "Fails if JUCE's JuceWebViewClasses.java has changed since our patched copy was taken."
    group = "verification"

    val upstream = vendoredJuceWebViewUpstream
    val expected = vendoredJuceWebViewUpstreamSha
    inputs.file(upstream).withPathSensitivity(PathSensitivity.RELATIVE)
    val marker = layout.buildDirectory.file("vendored-juce-webview.ok")
    outputs.file(marker)

    doLast {
        val actual = MessageDigest.getInstance("SHA-256")
            .digest(upstream.readBytes())
            .joinToString("") { byte -> "%02x".format(byte) }

        if (actual != expected) {
            throw GradleException(
                """
                JUCE's JuceWebViewClasses.java has changed.

                  file:     ${upstream.path}
                  expected: $expected
                  actual:   $actual

                android/app/src/main/java/com/rmsl/juce/JuceWebViewClasses.java is a
                patched copy of that file (it adds settings.setDomStorageEnabled(true),
                without which the UI cannot start). Re-take the copy from the new
                upstream version, re-apply that one-line patch, and update
                vendoredJuceWebViewUpstreamSha in this build file.
                """.trimIndent()
            )
        }

        marker.get().asFile.writeText(actual)
    }
}

tasks.named("preBuild") { dependsOn(checkVendoredJuceWebView) }

// ---------------------------------------------------------------------------
// Vendored-Oboe drift check.
//
// android/oboe-overlay/oboe/AudioStreamBase.h is a patched copy of Oboe's
// header that changes one value: the default capture InputPreset, from
// VoiceRecognition to Unprocessed. It is forced onto the front of the include
// path by android/CMakeLists.txt, so it silently replaces the original for the
// entire build — which is exactly why it must not be allowed to drift.
// ---------------------------------------------------------------------------
val vendoredOboeUpstream =
    File(juceModules, "juce_audio_devices/native/oboe/include/oboe/AudioStreamBase.h")

/** SHA-256 of the upstream header this overlay was taken from (JUCE 8.0.14). */
val vendoredOboeUpstreamSha =
    "da424cf10e7ae32db12d639fd63ed82da58153fbc30e385a97c5c388b979e949"

val checkVendoredOboeHeader = tasks.register("checkVendoredOboeHeader") {
    description = "Fails if Oboe's AudioStreamBase.h has changed since our overlay was taken."
    group = "verification"

    val upstream = vendoredOboeUpstream
    val expected = vendoredOboeUpstreamSha
    inputs.file(upstream).withPathSensitivity(PathSensitivity.RELATIVE)
    val marker = layout.buildDirectory.file("vendored-oboe-header.ok")
    outputs.file(marker)

    doLast {
        val actual = MessageDigest.getInstance("SHA-256")
            .digest(upstream.readBytes())
            .joinToString("") { byte -> "%02x".format(byte) }

        if (actual != expected) {
            throw GradleException(
                """
                Oboe's AudioStreamBase.h has changed.

                  file:     ${upstream.path}
                  expected: $expected
                  actual:   $actual

                android/oboe-overlay/oboe/AudioStreamBase.h is a copy of that header
                with the default capture InputPreset changed to Unprocessed, and it is
                forced ahead of the original on the include path. Re-take the copy from
                the new upstream version, re-apply that one change, and update
                vendoredOboeUpstreamSha in this build file.
                """.trimIndent()
            )
        }

        marker.get().asFile.writeText(actual)
    }
}

tasks.named("preBuild") { dependsOn(checkVendoredOboeHeader) }
