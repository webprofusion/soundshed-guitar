#pragma once

/**
 * PluginProcessorAdapter — JUCE thin adapter.
 *
 * Implements IPluginHost and delegates all business logic to PluginController.
 * This replaces the original monolithic PluginProcessor class.
 */

#include <juce_audio_processors/juce_audio_processors.h>

#include "IPluginHost.h"
#include "PluginController.h"

// Defined by clap_juce_extensions, which the CLAP build links into the shared code (not on Android).
#ifdef HAS_CLAP_JUCE_EXTENSIONS
 #include <clap-juce-extensions/clap-juce-extensions.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace juce
{
    class FileChooser;
}

class PluginProcessorAdapter : public juce::AudioProcessor,
                               public guitarfx::IPluginHost
                              #ifdef HAS_CLAP_JUCE_EXTENSIONS
                               , public clap_juce_extensions::clap_juce_audio_processor_capabilities
                              #endif
{
public:
    PluginProcessorAdapter();
    ~PluginProcessorAdapter() override;

    // ── juce::AudioProcessor overrides ─────────────────────────────
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

   #ifdef HAS_CLAP_JUCE_EXTENSIONS
    // ── CLAP outbound events ───────────────────────────────────────
    /// The CLAP wrapper's own MIDI output forwards only two- and three-byte messages, so the
    /// controller-display SysEx would never reach the host without taking the queue over.
    bool supportsOutboundEvents() override { return true; }
    void addOutboundEventsToQueue (const clap_output_events* outEvents,
                                   const juce::MidiBuffer& midiBuffer,
                                   int sampleOffset) override;
   #endif

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    // ── Automation parameters ──────────────────────────────────────
    /// A JUCE parameter backed by an automation slot.
    class AutomationSlotParameter;

    /// Get all automation slot parameter IDs (for DAW exposure).
    [[nodiscard]] std::vector<juce::String> getAutomationParameterIds() const;

    /// Build the full list of DAW-exposed parameter IDs (defaults + reserved custom).
    /// Called once in the constructor to register parameters with stable indices.
    void registerAutomationParameters();

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /// The host flips this around an offline bounce/freeze/export. Forwarded to the
    /// controller so NAM renders at full quality when there is no realtime deadline.
    void setNonRealtime (bool isNonRealtime) noexcept override;

    // ── IPluginHost implementation ─────────────────────────────────
    void SendMessageToUI (const std::string& jsonMessage) override;
    void BrowseFileAsync (guitarfx::BrowseFileType type,
        const std::string& title,
        std::function<void (const guitarfx::BrowseFileResult&)> callback) override;
    void SaveFileAsync (guitarfx::BrowseFileType type,
        const std::string& title,
        const std::string& defaultName,
        std::function<void (const guitarfx::BrowseFileResult&)> callback) override;
    void RunOnMainThread (std::function<void()> fn) override;
    [[nodiscard]] bool IsMessageThread() const override;
    [[nodiscard]] std::filesystem::path GetUserDataPath() const override;
    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override;
    [[nodiscard]] double GetSampleRate() const override;
    [[nodiscard]] int GetBlockSize() const override;
    [[nodiscard]] bool SupportsAudioDeviceSettings() const override;
    void HandleAudioDeviceRequest (const std::string& requestJson) override;
    void NotifyStateChanged() override;
    void NotifyDeferredStateRestored() override;
    void NotifyLatencyChanged (int latencySamples) override;
    [[nodiscard]] double GetHostTempo() const override;
    [[nodiscard]] bool IsHostPlaying() const override;
    [[nodiscard]] bool IsStandalone() const override;

    // ── WebView bridge ─────────────────────────────────────────────
    void setWebMessageCallback (std::function<void (const juce::String&)> callback);
    void handleWebMessage (const juce::String& message);
    void sendMessageToUI (const juce::String& message);

    // ── Standalone audio device settings ───────────────────────────
    /// Where "audioDevice" requests go. The standalone app's window installs this once the
    /// device manager exists and clears it before that goes away; plugin formats never do.
    /// Message thread only, like the requests.
    using AudioDeviceRequestHandler = std::function<void (const std::string& requestJson)>;
    void setAudioDeviceRequestHandler (AudioDeviceRequestHandler handler);

    // ── Accessors ──────────────────────────────────────────────────
    [[nodiscard]] guitarfx::PluginController& getController() { return mController; }

private:
    [[nodiscard]] std::filesystem::path locateAssetsRoot() const;
    void ensureStandaloneProtocolHandlerRegistration();
    /// `mayBlock` false on the audio thread: see PluginController::ApplyAutomationFromDAW.
    void applyPendingDAWParamChanges (bool mayBlock);
    /// After a restore has been applied: tells the host that the parameters it reads back
    /// may have changed.
    void notifyHostOfRestoredState();

    // ── State ──────────────────────────────────────────────────────
    guitarfx::PluginController mController;

    std::function<void (const juce::String&)> mWebMessageCallback;
    std::mutex mWebMessageMutex;

    AudioDeviceRequestHandler mAudioDeviceRequestHandler;

    std::filesystem::path mAssetRoot;
    std::unique_ptr<juce::FileChooser> mFileChooser;
    bool mStandaloneProtocolRegistrationAttempted = false;

    // Pending DAW parameter changes, drained under the DSP lock by processBlock, or by
    // releaseResources for a block that is not coming. Each entry: (parameter index,
    // normalized 0..1), an index into mSlotIdsByParameter, so queuing one allocates nothing.
    // The queue is reserved up front and cleared in place, never swapped out, so the audio
    // thread neither grows nor frees it in the usual case.
    std::mutex mPendingDAWParamMutex;
    std::vector<std::pair<int, float>> mPendingDAWParamChanges;
    std::vector<std::string> mSlotIdsByParameter; ///< fixed once the parameters are registered

    // True from prepareToPlay to releaseResources. Only then does anything drain the
    // queue above, so only then does AutomationSlotParameter::setValue use it.
    std::atomic<bool> mAudioActive { false };

   #ifdef HAS_CLAP_JUCE_EXTENSIONS
    // A CLAP SysEx event points at its bytes, and the host reads them after process()
    // returns, by which time the MidiBuffer has been reused for the next slice of the
    // block. They are copied here instead; the arena starts over with each process() call.
    // Audio thread only.
    std::array<juce::uint8, 512> mClapSysExArena {};
    std::size_t mClapSysExArenaUsed = 0;
   #endif

    // Applies the setlist steps, bank changes and scene switches that MIDI and DAW automation
    // park for the message thread. The editor's idle loop does too, but it only runs while the
    // editor is open. Last, so it is stopped before anything its callback uses is destroyed.
    static constexpr int kControlSurfaceDrainHz = 30;
    juce::TimedCallback mControlSurfaceDrain { [this] { mController.DrainControlSurfaceRequests(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessorAdapter)
};
