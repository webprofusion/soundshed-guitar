#include "AndroidUsbAudioPath.h"

#include <atomic>
#include <mutex>
#include <utility>

#if defined(__ANDROID__)
    #include <android/log.h>
    #include <dlfcn.h>
    #include <jni.h>

    #include <cstdint>
    #include <string>
#endif

namespace soundshed::android_audio
{
namespace
{
std::atomic<bool> gMMapAllowed{true};
std::mutex gHandlerMutex;
std::function<void()> gHandler;
} // namespace

void SetUsbAudioPathHandler(std::function<void()> handler)
{
    const std::lock_guard lock(gHandlerMutex);
    gHandler = std::move(handler);
}

bool UsbMMapAllowed() noexcept
{
    return gMMapAllowed.load();
}

#if defined(__ANDROID__)
namespace
{
constexpr const char* kTag = "SoundshedGuitar";

// AAudio's MMAP policy, as libaaudio defines it.
constexpr std::int32_t kPolicyNever = 1;
constexpr std::int32_t kPolicyAuto = 2;

// AAudio_setMMapPolicy is exported by libaaudio but not declared by the NDK: Oboe reaches it the
// same way (OboeExtensions::setMMapEnabled). It sets the policy for this process only, for
// streams opened after it, whichever copy of Oboe opens them.
bool SetMMapPolicy(std::int32_t policy)
{
    using SetPolicy = std::int32_t (*)(std::int32_t);
    static const SetPolicy setPolicy = [] {
        void* library = dlopen("libaaudio.so", RTLD_NOW);
        return library != nullptr ? reinterpret_cast<SetPolicy>(dlsym(library, "AAudio_setMMapPolicy")) : nullptr;
    }();
    return setPolicy != nullptr && setPolicy(policy) == 0;
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
} // namespace
#endif
} // namespace soundshed::android_audio

#if defined(__ANDROID__)
// com.soundshed.audio.UsbAudioPath, on the main thread: whether the MMAP path may be used, and the
// interface that rules it out (empty when allowed).
extern "C" JNIEXPORT void JNICALL Java_com_soundshed_audio_UsbAudioPath_nativeSetMMapAllowed(JNIEnv* env, jclass,
                                                                                         jboolean allowed,
                                                                                         jstring reason)
{
    using namespace soundshed::android_audio;
    const bool allow = allowed == JNI_TRUE;
    if (gMMapAllowed.exchange(allow) == allow)
        return;

    const bool set = SetMMapPolicy(allow ? kPolicyAuto : kPolicyNever);
    if (allow)
        __android_log_print(ANDROID_LOG_INFO, kTag, "USB audio: MMAP path allowed again%s",
                            set ? "" : " (the policy could not be set)");
    else
        __android_log_print(ANDROID_LOG_INFO, kTag, "USB audio: MMAP path off for %s (no 16-bit format)%s",
                            ToString(env, reason).c_str(), set ? "" : " (the policy could not be set)");

    std::function<void()> handler;
    {
        const std::lock_guard lock(gHandlerMutex);
        handler = gHandler;
    }
    if (handler)
        handler();
}
#endif
