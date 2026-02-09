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

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace juce { class FileChooser; }

class PluginProcessorAdapter : public juce::AudioProcessor,
                                public guitarfx::IPluginHost
{
public:
    PluginProcessorAdapter();
    ~PluginProcessorAdapter() override;

    // ── juce::AudioProcessor overrides ─────────────────────────────
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ── IPluginHost implementation ─────────────────────────────────
    void SendMessageToUI(const std::string& jsonMessage) override;
    void BrowseFileAsync(guitarfx::BrowseFileType type,
                         const std::string& title,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override;
    void SaveFileAsync(guitarfx::BrowseFileType type,
                       const std::string& title,
                       const std::string& defaultName,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override;
    void RunOnMainThread(std::function<void()> fn) override;
    [[nodiscard]] std::filesystem::path GetUserDataPath() const override;
    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override;
    [[nodiscard]] double GetSampleRate() const override;
    [[nodiscard]] int GetBlockSize() const override;
    void OpenAudioPreferences() override;
    void NotifyStateChanged() override;
    [[nodiscard]] double GetHostTempo() const override;
    [[nodiscard]] bool IsHostPlaying() const override;

    // ── WebView bridge ─────────────────────────────────────────────
    void setWebMessageCallback(std::function<void(const juce::String&)> callback);
    void handleWebMessage(const juce::String& message);
    void sendMessageToUI(const juce::String& message);

    // ── Accessors ──────────────────────────────────────────────────
    juce::AudioProcessorValueTreeState& getState() { return mAPVTS; }
    [[nodiscard]] guitarfx::PluginController& getController() { return mController; }

    // ── Parameter IDs (matches PluginController::ParameterId) ──────
    enum ParameterIndex
    {
        kParamInputTrim = 0,
        kParamOutputTrim,
        kParamDrive,
        kParamTone,
        kParamGateEnabled,
        kParamGateThreshold,
        kParamMix,
        kParamDoublerEnabled,
        kParamDoublerDelay,
        kParamTranspose,
        kParamIRQuality,
        kParamEQEnabled,
        kParamEQLowGain,
        kParamEQLowFreq,
        kParamEQLowMidGain,
        kParamEQLowMidFreq,
        kParamEQLowMidQ,
        kParamEQHighMidGain,
        kParamEQHighMidFreq,
        kParamEQHighMidQ,
        kParamEQHighGain,
        kParamEQHighFreq,
        kParamCount
    };

    static constexpr const char* kParamIds[] = {
        "input_trim", "output_trim", "drive", "tone",
        "gate_enabled", "gate_threshold", "mix",
        "doubler_enabled", "doubler_delay", "transpose", "ir_quality",
        "eq_enabled",
        "eq_low_gain", "eq_low_freq",
        "eq_lowmid_gain", "eq_lowmid_freq", "eq_lowmid_q",
        "eq_highmid_gain", "eq_highmid_freq", "eq_highmid_q",
        "eq_high_gain", "eq_high_freq"
    };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void applyParametersToController();
    [[nodiscard]] std::filesystem::path locateAssetsRoot() const;

    // ── State ──────────────────────────────────────────────────────
    guitarfx::PluginController mController;
    juce::AudioProcessorValueTreeState mAPVTS;

    std::function<void(const juce::String&)> mWebMessageCallback;
    std::mutex mWebMessageMutex;

    std::filesystem::path mAssetRoot;
    std::unique_ptr<juce::FileChooser> mFileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessorAdapter)
};
