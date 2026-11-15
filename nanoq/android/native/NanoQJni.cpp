// Soundshed Nano Q for Android: the native side of NanoQActivity.
//
// One App for the activity's life: the plugin (through ClapHost), the audio (AudioIO) and the
// window the plugin's editor draws in (Elements' Android host, android_host.hpp). Every call
// comes on the activity's main thread; only the audio callback runs elsewhere.
//
// See docs/plans/nano-q-android.md.

#include "AppLog.h"
#include "AudioIO.h"
#include "ClapHost.h"

#include "../elements_host/android_host.hpp"

#include "AndroidUsbAudioPath.h"

#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace an = cycfi::elements::android;
using nanoq::android::AppLog;

namespace
{
struct App
{
    std::unique_ptr<nanoq::android::ClapHost> host;
    std::unique_ptr<nanoq::android::AudioIO> audio;
    an::window* window = nullptr;
    std::string statePath;
    bool attached = false;
};

App* FromHandle(jlong handle)
{
    return reinterpret_cast<App*>(static_cast<intptr_t>(handle));
}

std::string ToString(JNIEnv* env, jstring s)
{
    if (s == nullptr)
        return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr)
        env->ReleaseStringUTFChars(s, chars);
    return out;
}

// The editor goes into the window once the window has a size, and follows it after.
void AttachOrResize(App& app)
{
    if (!app.host)
        return;
    uint32_t width = 0, height = 0;
    an::logical_size(app.window, width, height);
    if (width == 0 || height == 0)
        return;
    if (!app.attached)
    {
        app.attached = app.host->AttachView(app.window, width, height);
        AppLog("Nano Q: the editor %s at %ux%u", app.attached ? "opened" : "did not open", width, height);
    }
    else
    {
        app.host->ResizeView(width, height);
    }
}
} // namespace

extern "C"
{
JNIEXPORT jlong JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeCreate(JNIEnv* env, jclass, jstring libraryPath,
                                                                            jstring resourceRoot, jstring profileDir,
                                                                            jstring homeDir, jstring statePath,
                                                                            jfloat density)
{
    // Before the plugin is made: where its resources are (Paths.cpp, the Elements host), where
    // its data goes (SOUNDSHED_NANOQ_PROFILE, read as the engine is built), and a HOME for what
    // derives a folder from one (QPlug's user presets).
    setenv("SOUNDSHED_NANOQ_RESOURCES", ToString(env, resourceRoot).c_str(), 1);
    setenv("SOUNDSHED_NANOQ_PROFILE", ToString(env, profileDir).c_str(), 1);
    const auto home = ToString(env, homeDir);
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);

    auto app = std::make_unique<App>();
    app->statePath = ToString(env, statePath);
    app->window = an::make_window(density);

    app->host = std::make_unique<nanoq::android::ClapHost>();
    if (!app->host->Load(ToString(env, libraryPath).c_str()))
    {
        AppLog("Nano Q: the plugin did not load");
        app->host.reset();
    }
    else
    {
        if (app->host->LoadState(app->statePath.c_str()))
            AppLog("Nano Q: the plugin's state was restored");
        app->audio = std::make_unique<nanoq::android::AudioIO>(*app->host);
        // A USB interface the MMAP path mangles came or went: reopen on the path now allowed.
        auto* audio = app->audio.get();
        soundshed::android_audio::SetUsbAudioPathHandler([audio] { audio->Restart(); });
    }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(app.release()));
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeDestroy(JNIEnv*, jclass, jlong handle)
{
    auto* app = FromHandle(handle);
    if (app == nullptr)
        return;
    soundshed::android_audio::SetUsbAudioPathHandler({});
    if (app->audio)
        app->audio->Stop();
    app->audio.reset();
    if (app->host)
    {
        app->host->SaveState(app->statePath.c_str());
        app->host->Unload();
    }
    app->host.reset();
    an::destroy_window(app->window);
    delete app;
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeSurfaceCreated(JNIEnv* env, jclass, jlong handle,
                                                                                   jobject surface)
{
    auto* app = FromHandle(handle);
    if (app == nullptr || surface == nullptr)
        return;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    an::surface_created(app->window, window);
    // surface_created took its own reference.
    if (window != nullptr)
        ANativeWindow_release(window);
    AttachOrResize(*app);
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeSurfaceChanged(JNIEnv*, jclass, jlong handle,
                                                                                   jint width, jint height)
{
    auto* app = FromHandle(handle);
    if (app == nullptr)
        return;
    an::surface_changed(app->window, width, height);
    AttachOrResize(*app);
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeSurfaceDestroyed(JNIEnv*, jclass, jlong handle)
{
    auto* app = FromHandle(handle);
    if (app != nullptr)
        an::surface_destroyed(app->window);
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeFrame(JNIEnv*, jclass, jlong handle,
                                                                          jlong frameTimeNanos)
{
    auto* app = FromHandle(handle);
    if (app == nullptr)
        return;
    const double now = double(frameTimeNanos) * 1e-9;
    if (app->host)
        app->host->RunMainThreadCallbacks();
    if (app->audio)
        app->audio->Poll(now);
    an::frame(app->window, now);
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeTouch(JNIEnv* env, jclass, jlong handle,
                                                                          jint action, jint pointerId, jintArray ids,
                                                                          jfloatArray xs, jfloatArray ys,
                                                                          jlong eventTimeMillis)
{
    auto* app = FromHandle(handle);
    if (app == nullptr || ids == nullptr || xs == nullptr || ys == nullptr)
        return;
    const jsize count = env->GetArrayLength(ids);
    if (count <= 0 || env->GetArrayLength(xs) < count || env->GetArrayLength(ys) < count)
        return;
    const auto n = static_cast<size_t>(count);
    std::vector<int> idv(n);
    std::vector<float> xv(n), yv(n);
    env->GetIntArrayRegion(ids, 0, count, idv.data());
    env->GetFloatArrayRegion(xs, 0, count, xv.data());
    env->GetFloatArrayRegion(ys, 0, count, yv.data());
    an::touch(app->window, action, pointerId, int(count), idv.data(), xv.data(), yv.data(),
              double(eventTimeMillis) * 1e-3);
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeWheel(JNIEnv*, jclass, jlong handle, jfloat dx,
                                                                          jfloat dy, jfloat x, jfloat y)
{
    auto* app = FromHandle(handle);
    if (app != nullptr)
        an::wheel(app->window, dx, dy, x, y);
}

JNIEXPORT jboolean JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeKey(JNIEnv*, jclass, jlong handle,
                                                                            jint keyCode, jboolean down, jint meta,
                                                                            jint repeat, jint codepoint)
{
    auto* app = FromHandle(handle);
    if (app == nullptr)
        return JNI_FALSE;
    return an::key(app->window, keyCode, down == JNI_TRUE, meta, repeat, uint32_t(codepoint)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeFocus(JNIEnv*, jclass, jlong handle,
                                                                          jboolean focused)
{
    auto* app = FromHandle(handle);
    if (app != nullptr)
        an::focus(app->window, focused == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeStartAudio(JNIEnv*, jclass, jlong handle,
                                                                                   jboolean withInput)
{
    auto* app = FromHandle(handle);
    if (app == nullptr || !app->audio)
        return JNI_FALSE;
    return app->audio->Start(withInput == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeStopAudio(JNIEnv*, jclass, jlong handle)
{
    auto* app = FromHandle(handle);
    if (app != nullptr && app->audio)
        app->audio->Stop();
}

JNIEXPORT void JNICALL Java_com_soundshed_nanoq_NanoQActivity_nativeSaveState(JNIEnv*, jclass, jlong handle)
{
    auto* app = FromHandle(handle);
    if (app == nullptr || !app->host)
        return;
    if (app->host->SaveState(app->statePath.c_str()))
        AppLog("Nano Q: the plugin's state was saved");
    else
        AppLog("Nano Q: the plugin's state was not saved");
}
} // extern "C"
