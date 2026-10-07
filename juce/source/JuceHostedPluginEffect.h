#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/EffectProcessor.h"
#include "dsp/NoteEvents.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace guitarfx
{
    class JuceHostedPluginEffect final : public EffectProcessor,
                                         private juce::AudioProcessorListener,
                                         private juce::AudioProcessorParameter::Listener,
                                         private juce::AsyncUpdater
    {
    public:
        JuceHostedPluginEffect();
        ~JuceHostedPluginEffect() override;

        void Prepare (double sampleRate, int maxBlockSize) override;
        void Reset() override;
        void Process (float** inputs, float** outputs, int numSamples) override;

        void SetParam (const std::string& key, double value) override;
        [[nodiscard]] double GetParam (const std::string& key) const override;

        void SetConfig (const std::string& key, const std::string& value) override;
        [[nodiscard]] std::string GetConfig (const std::string& key) const override;
        void SetRuntimeConfigChangedCallback (RuntimeConfigChangedCallback callback) override;

        bool LoadResource (const std::filesystem::path& path) override;
        bool LoadResources (const std::vector<ResourceRef>& refs,
            const std::vector<std::filesystem::path>& paths) override;
        [[nodiscard]] bool RequiresResource() const override { return true; }
        [[nodiscard]] bool HasResource() const override { return mPlugin != nullptr; }
        // Plugin scanning and instantiation must run on the JUCE message thread
        // (AU and VST3 enforce this). Prevents deadlock when called from std::async.
        [[nodiscard]] bool RequiresMainThreadLoad() const noexcept override { return true; }
        [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mPluginPath; }
        [[nodiscard]] int GetLatencySamples() const override;

        [[nodiscard]] std::string GetType() const override { return "plugin_host"; }
        [[nodiscard]] std::string GetCategory() const override { return "utility"; }

        // A hosted instrument plays the notes of any Guitar to MIDI node upstream of it; an
        // effect that ignores MIDI ignores them. With no note source upstream nothing is sent.
        [[nodiscard]] bool AcceptsNoteInput() const override { return true; }
        void SetNoteInput (std::span<const NoteBlock* const> sources) override { mNoteSources = sources; }

#if defined(GUITARFX_ENABLE_PLUGIN_HOST_TEST_API)
        [[nodiscard]] juce::AudioPluginInstance* GetHostedPluginForTesting() const { return mPlugin.get(); }
        [[nodiscard]] bool IsPluginEditorOpenForTesting() const { return mEditorWindow != nullptr; }
        bool ClosePluginEditorForTesting() { return ClosePluginEditor(); }
        // Installs a plugin instance directly, bypassing scanning, and wires up the same
        // listeners a real load would. Lets the state-plumbing tests (capture guards,
        // gesture coalescing) run against a stub instead of a real plugin binary.
        void InstallHostedPluginForTesting (std::unique_ptr<juce::AudioPluginInstance> plugin);
        [[nodiscard]] std::string GetPendingPluginStateForTesting() const { return mPluginStateBase64; }
        [[nodiscard]] const juce::AudioPluginFormatManager& GetPluginFormatsForTesting() const { return mSharedFormats->manager; }
#endif

    private:
        void EnsureFormatsAdded();
        bool LoadPluginFromPath (const std::filesystem::path& path);
        bool ConfigurePluginBuses (juce::AudioPluginInstance& plugin) const;
        void PrepareLoadedPlugin();
        void UpdateWorkBufferForPlugin();
        void CopyInputToWorkBuffer (float** inputs, int numSamples);
        void FillMidiFromNotes (std::span<const NoteBlock* const> sources, int numSamples);
        void CopyWorkBufferToOutputs (float** inputs, float** outputs, int numSamples);
        void Passthrough (float** inputs, float** outputs, int numSamples) const;
        void ApplyPluginStateBase64 (const std::string& value);
        void ApplyPendingPluginState();
        [[nodiscard]] std::string CapturePluginStateBase64() const;
        void AttachHostedPluginListeners();
        void AttachHostedPluginParameterListeners();
        void DetachHostedPluginParameterListeners();
        void ReleaseHostedPlugin();
        void ClearLoadedPluginMetadata();
        void ScheduleAutoCapture (bool forceNotify = false);
        void CaptureAndPublishPluginState (bool forceNotify = false);
        void PublishCapturedPluginState (const std::string& capturedState, bool forceNotify = false);
        void EnsurePluginStateBaseline();
        void OpenPluginEditor();
        bool ClosePluginEditor();
        void SetError (const std::string& message, const std::string& code = "unknown");
        void parameterValueChanged (int parameterIndex, float newValue) override;
        void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;
        void audioProcessorParameterChanged (juce::AudioProcessor* processor, int parameterIndex, float newValue) override;
        void audioProcessorChanged (juce::AudioProcessor* processor,
            const juce::AudioProcessorListener::ChangeDetails& details) override;
        void handleAsyncUpdate() override;

        // The plugin formats every Plugin Host in the process scans and instantiates with.
        // Setting them up is not cheap: JUCE's LV2 format loads every installed LV2 bundle and
        // writes its own spec bundles to a temp folder, 100-250 ms here, and while each node
        // owned a set that was paid again for every Plugin Host on every preset load. Shared,
        // they live while any Plugin Host does, and are set up on the first load rather than
        // at construction. In the app every load runs on the message thread; the lock covers
        // a load made where no message loop exists, and is recursive, so a plugin whose setup
        // runs a nested message loop cannot deadlock a second load on the same thread.
        struct SharedPluginFormats
        {
            juce::CriticalSection lock;
            juce::AudioPluginFormatManager manager;
            bool added = false;
        };

        // Declared ahead of mPlugin so a plugin instance is always destroyed before the
        // formats that made it, should this be the last Plugin Host.
        juce::SharedResourcePointer<SharedPluginFormats> mSharedFormats;
        juce::AudioBuffer<float> mWorkBuffer;
        juce::MidiBuffer mMidiBuffer;
        // Notes for the hosted plugin (dsp/NoteEvents.h). The sources are this block's, set by the
        // executor just before Process() and dropped by it. The player is the audio thread's; the
        // message thread only touches it under mPluginProcessLock, while the plugin is swapped or
        // prepared, which is when the plugin's own voices are reset too.
        std::span<const NoteBlock* const> mNoteSources;
        NotePlayer mNotePlayer;
        // Guards the hosted plugin against concurrent access: the audio thread
        // try-locks (falling back to passthrough), while the message thread holds
        // the lock during editor create/destroy, prepareToPlay, state restore and
        // instance swap. JUCE's LV2 host in particular rebuilds plugin internals
        // during editor/view lifecycle, which crashes if run() executes concurrently.
        juce::SpinLock mPluginProcessLock;
        std::unique_ptr<juce::AudioPluginInstance> mPlugin;
        std::unique_ptr<juce::DocumentWindow> mEditorWindow;
        juce::PluginDescription mPluginDescription;

        std::filesystem::path mPluginPath;
        std::string mPluginFormat;
        std::string mPluginIdentifier;
        std::string mPluginStateBase64;
        // The state last restored into the current mPlugin, so preparing it straight after a
        // load does not restore the same state a second time. Cleared with the instance.
        std::string mAppliedPluginStateBase64;
        std::string mLastError;
        std::string mLastErrorCode;
        RuntimeConfigChangedCallback mRuntimeConfigChangedCallback;
        std::vector<juce::AudioProcessorParameter*> mHostedParametersWithListeners;

        double mMix = 1.0;
        double mInputGainDb = 0.0;
        double mOutputGainDb = 0.0;
        std::atomic<int> mAutoCaptureSuppressionDepth { 0 };
        // Depth of in-progress parameter gestures (knob drags). While non-zero, per-value
        // change notifications are ignored: a drag fires one for every tick, and each
        // capture is a full getStateInformation() on the message thread. The matching
        // gesture-end notification performs the single capture that covers the whole drag.
        std::atomic<int> mActiveGestureDepth { 0 };
        std::atomic<bool> mForceAutoCaptureNotification { false };
        bool mPrepared = false;
        bool mHostedPluginListenerAttached = false;
    };

    void RegisterJuceHostedPluginEffect();

} // namespace guitarfx
