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
    using juce::AudioProcessor::processBlock;

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
    void NotifyLatencyChanged(int latencySamples) override;
    [[nodiscard]] double GetHostTempo() const override;
    [[nodiscard]] bool IsHostPlaying() const override;
    [[nodiscard]] bool IsStandalone() const override;

    // ── WebView bridge ─────────────────────────────────────────────
    void setWebMessageCallback(std::function<void(const juce::String&)> callback);
    void handleWebMessage(const juce::String& message);
    void sendMessageToUI(const juce::String& message);

    // ── Accessors ──────────────────────────────────────────────────
    [[nodiscard]] guitarfx::PluginController& getController() { return mController; }

private:
    [[nodiscard]] std::filesystem::path locateAssetsRoot() const;

    // ── State ──────────────────────────────────────────────────────
    guitarfx::PluginController mController;

    std::function<void(const juce::String&)> mWebMessageCallback;
    std::mutex mWebMessageMutex;

    std::filesystem::path mAssetRoot;
    std::unique_ptr<juce::FileChooser> mFileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessorAdapter)
};
