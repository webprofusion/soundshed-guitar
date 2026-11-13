// Soundshed Nano Q for Android: the Gradle project around nanoq/CMakeLists.txt.
// See docs/plans/nano-q-android.md. Versions follow Soundshed Guitar's Android project (android/).
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "SoundshedNanoQ"
include(":app")
