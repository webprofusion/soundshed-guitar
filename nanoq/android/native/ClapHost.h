#pragma once

// The Android app's CLAP host: the smallest one Nano Q's plugin needs, around the plugin that is
// linked into the app's library (the clap_entry nanoq/src/clap_entry.cpp defines). It does what
// clap-wrapper's standalone does on the desktop, with the audio left to AudioIO. A port of the
// iOS app's (nanoq/ios/app/ClapHost.h):
//
//   main thread   Load, Activate, Deactivate, AttachView, ResizeView, Unload, and
//                 RunMainThreadCallbacks once a display frame, which runs on_main_thread when
//                 the plugin has asked for it
//   audio thread  Process, from Oboe's data callback
//
// See docs/plans/nano-q-android.md.

#include <clap/clap.h>

#include <atomic>
#include <cstdint>

namespace nanoq::android
{
class ClapHost
{
public:
    ClapHost();
    ~ClapHost();

    ClapHost(const ClapHost&) = delete;
    ClapHost& operator=(const ClapHost&) = delete;

    /// Initialises the entry, creates the plugin and inits it. The thread this is called on is
    /// the main thread from then on.
    bool Load(const char* libraryPath);
    void Unload();

    /// The plugin's processing state follows the audio streams': activated before they start,
    /// deactivated once they have stopped, so Process never runs on an inactive plugin.
    bool Activate(double sampleRate, uint32_t maxFrames);
    void Deactivate();
    bool IsActive() const
    {
        return mActive.load();
    }

    /// Audio thread. `in` and `out` are two channels of `frames` samples each; `in` may be
    /// null (no input), and is then silence. `frames` is at most the activation's maxFrames.
    void Process(const float* const* in, float* const* out, uint32_t frames);

    /// The plugin's state (clap.state), kept in a file between runs as clap-wrapper's desktop
    /// standalone keeps it: the engine's own data is in its profile, and this is the rest of
    /// what a host would save in a project (the active preset, the host parameters).
    bool SaveState(const char* path);
    bool LoadState(const char* path);

    /// Creates the editor and puts its view in `parent`, a cycfi::elements::android::window,
    /// at a size in its logical units.
    bool AttachView(void* parent, uint32_t width, uint32_t height);
    void ResizeView(uint32_t width, uint32_t height);
    void DetachView();
    bool HasView() const
    {
        return mGuiCreated;
    }

    /// Main thread: on_main_thread, if the plugin asked for it since the last call.
    void RunMainThreadCallbacks();

private:
    static const void* GetExtension(const clap_host_t* host, const char* id);
    static void RequestRestart(const clap_host_t* host);
    static void RequestProcess(const clap_host_t* host);
    static void RequestCallback(const clap_host_t* host);

    clap_host_t mHost{};
    const clap_plugin_entry_t* mEntry = nullptr;
    const clap_plugin_t* mPlugin = nullptr;
    const clap_plugin_gui_t* mGui = nullptr;
    const clap_plugin_state_t* mState = nullptr;
    bool mGuiCreated = false;
    std::atomic<bool> mActive{false};

    // Whether start_processing has been called on the audio thread since activation.
    bool mProcessing = false;
    uint64_t mSteadyTime = 0;

    // Set when a main-thread callback is asked for (from any thread), cleared when it runs.
    std::atomic<bool> mCallbackQueued{false};
};
} // namespace nanoq::android
