/**
 * PluginProcessorAdapter.cpp — JUCE thin adapter implementation.
 *
 * All business logic (DSP, presets, message handling) is delegated to
 * PluginController from soundshed-guitar core. This file only contains
 * JUCE-specific glue code.
 */

#include "PluginProcessorAdapter.h"
#include "PluginEditor.h"   // existing editor, unchanged
#include "UiBridge.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace juce
{
void JUCE_CALLTYPE juce_showStandaloneAudioSettingsDialog();
}

namespace
{
#if JUCE_LINUX
class HeadlessLv2ManifestEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HeadlessLv2ManifestEditor(juce::AudioProcessor& processor)
        : juce::AudioProcessorEditor(&processor)
    {
        setResizable(true, true);
        setResizeLimits(800, 600, 8192, 8192);
        setSize(1200, 900);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);
    }

    void resized() override {}
};

bool shouldUseHeadlessLv2ManifestEditor(const PluginProcessorAdapter& processor)
{
    return processor.wrapperType == juce::AudioProcessor::wrapperType_LV2
        && std::getenv("DISPLAY") == nullptr
        && std::getenv("WAYLAND_DISPLAY") == nullptr;
}
#endif
}

// ════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ════════════════════════════════════════════════════════════════════════

PluginProcessorAdapter::PluginProcessorAdapter()
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
      ),
    mController(*this)
{
    mAssetRoot = locateAssetsRoot();
    mController.Initialize();
}

PluginProcessorAdapter::~PluginProcessorAdapter() = default;

// ════════════════════════════════════════════════════════════════════════
// juce::AudioProcessor overrides
// ════════════════════════════════════════════════════════════════════════

void PluginProcessorAdapter::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    mController.Prepare(sampleRate, samplesPerBlock);
}

void PluginProcessorAdapter::releaseResources()
{
    mController.Reset();
}

bool PluginProcessorAdapter::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
}

void PluginProcessorAdapter::processBlock(juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const auto totalInputCh = getTotalNumInputChannels();
    const auto totalOutputCh = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    // Clear any output channels that don't have corresponding inputs
    for (auto i = totalInputCh; i < totalOutputCh; ++i)
        buffer.clear(i, 0, numSamples);

    // Set up float** for the core ProcessAudio
    float* inputs[2] = {
        const_cast<float*>(buffer.getReadPointer(0)),
        (totalInputCh > 1) ? const_cast<float*>(buffer.getReadPointer(1)) : nullptr
    };
    float* outputs[2] = {
        buffer.getWritePointer(0),
        (totalOutputCh > 1) ? buffer.getWritePointer(1) : nullptr
    };

    const bool processed = mController.ProcessAudio(inputs, outputs, numSamples);
    if (!processed)
    {
        // Controller couldn't acquire DSP lock — silence
        buffer.clear();
    }
}

juce::AudioProcessorEditor* PluginProcessorAdapter::createEditor()
{
#if JUCE_LINUX
    // JUCE's LV2 manifest helper instantiates the editor in headless CI just to query
    // resize metadata. Avoid constructing the real WebView-based editor in that path.
    if (shouldUseHeadlessLv2ManifestEditor(*this))
        return new HeadlessLv2ManifestEditor(*this);
#endif

    return new PluginEditor(*this);
}

bool PluginProcessorAdapter::hasEditor() const { return true; }
const juce::String PluginProcessorAdapter::getName() const { return JucePlugin_Name; }
bool PluginProcessorAdapter::acceptsMidi() const { return false; }
bool PluginProcessorAdapter::producesMidi() const { return false; }
bool PluginProcessorAdapter::isMidiEffect() const { return false; }
double PluginProcessorAdapter::getTailLengthSeconds() const { return 0.0; }
int PluginProcessorAdapter::getNumPrograms() { return 1; }
int PluginProcessorAdapter::getCurrentProgram() { return 0; }
void PluginProcessorAdapter::setCurrentProgram(int) {}
const juce::String PluginProcessorAdapter::getProgramName(int) { return {}; }
void PluginProcessorAdapter::changeProgramName(int, const juce::String&) {}

void PluginProcessorAdapter::getStateInformation(juce::MemoryBlock& destData)
{
    const auto controllerState = mController.SerializeState();
    juce::MemoryOutputStream stream(destData, false);
    stream.write(controllerState.data(), controllerState.size());
}

void PluginProcessorAdapter::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0) return;

    std::string controllerState(reinterpret_cast<const char*>(data), static_cast<size_t>(sizeInBytes));
    if (controllerState.empty()) return;

    mController.DeserializeState(controllerState);
}

// ════════════════════════════════════════════════════════════════════════
// IPluginHost implementation
// ════════════════════════════════════════════════════════════════════════

void PluginProcessorAdapter::SendMessageToUI(const std::string& jsonMessage)
{
    // evaluateJavascript must be called on the message thread.
    // When called from the audio thread (e.g. riffCaptureStarted/Progress/Stopped),
    // dispatch asynchronously; when already on the message thread call directly.
    auto msg = juce::String(jsonMessage);
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        sendMessageToUI(msg);
    }
    else
    {
        juce::MessageManager::callAsync([this, msg]() { sendMessageToUI(msg); });
    }
}

void PluginProcessorAdapter::BrowseFileAsync(
    guitarfx::BrowseFileType type,
    const std::string& title,
    std::function<void(const guitarfx::BrowseFileResult&)> callback)
{
    juce::String filters;
    switch (type)
    {
        case guitarfx::BrowseFileType::NAMModel:   filters = "*.nam"; break;
        case guitarfx::BrowseFileType::IRFile:      filters = "*.wav;*.aiff;*.aif;*.flac"; break;
        case guitarfx::BrowseFileType::PresetFile:  filters = "*.json"; break;
        case guitarfx::BrowseFileType::ImageFile:   filters = "*.png;*.jpg;*.jpeg;*.svg"; break;
        case guitarfx::BrowseFileType::AudioFile:   filters = "*.wav;*.mp3;*.flac;*.ogg"; break;
        case guitarfx::BrowseFileType::ArchiveFile: filters = "*.soundshed.preset;*.soundshed.presets;*.zip"; break;
        case guitarfx::BrowseFileType::Any:         filters = "*.*"; break;
        default:                                    filters = "*.*"; break;
    }

    mFileChooser = std::make_unique<juce::FileChooser>(
        juce::String(title), juce::File(), filters);

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;

    mFileChooser->launchAsync(flags, [this, callback](const juce::FileChooser& chooser)
    {
        guitarfx::BrowseFileResult result;
        const auto file = chooser.getResult();
        mFileChooser.reset();

        if (file.existsAsFile())
        {
            result.success = true;
            result.path = std::filesystem::path(file.getFullPathName().toStdString());
        }

        if (callback) callback(result);
    });
}

void PluginProcessorAdapter::SaveFileAsync(
    guitarfx::BrowseFileType type,
    const std::string& title,
    const std::string& defaultName,
    std::function<void(const guitarfx::BrowseFileResult&)> callback)
{
    auto normalizedDefaultName = defaultName;
    std::transform(normalizedDefaultName.begin(), normalizedDefaultName.end(), normalizedDefaultName.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const auto hasSuffix = [&normalizedDefaultName](std::string_view suffix)
    {
        return normalizedDefaultName.size() >= suffix.size()
            && normalizedDefaultName.compare(normalizedDefaultName.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    juce::String filters;
    switch (type)
    {
        case guitarfx::BrowseFileType::PresetFile:  filters = "*.json"; break;
        case guitarfx::BrowseFileType::ArchiveFile:
            if (hasSuffix(".soundshed.preset"))      filters = "*.soundshed.preset";
            else if (hasSuffix(".soundshed.presets")) filters = "*.soundshed.presets";
            else if (hasSuffix(".zip"))               filters = "*.zip";
            else                                        filters = "*.soundshed.preset";
            break;
        case guitarfx::BrowseFileType::NAMModel:
        case guitarfx::BrowseFileType::IRFile:
        case guitarfx::BrowseFileType::ImageFile:
        case guitarfx::BrowseFileType::AudioFile:
        case guitarfx::BrowseFileType::Any:
        default:                                    filters = "*.*"; break;
    }

    mFileChooser = std::make_unique<juce::FileChooser>(
        juce::String(title), juce::File(juce::String(defaultName)), filters);

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles;

    mFileChooser->launchAsync(flags, [this, callback](const juce::FileChooser& chooser)
    {
        guitarfx::BrowseFileResult result;
        const auto file = chooser.getResult();
        mFileChooser.reset();

        if (file != juce::File())
        {
            result.success = true;
            result.path = std::filesystem::path(file.getFullPathName().toStdString());
        }

        if (callback) callback(result);
    });
}

void PluginProcessorAdapter::RunOnMainThread(std::function<void()> fn)
{
    juce::MessageManager::callAsync(std::move(fn));
}

std::filesystem::path PluginProcessorAdapter::GetUserDataPath() const
{
    return std::filesystem::path(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Soundshed Guitar")
            .getFullPathName()
            .toStdString());
}

std::filesystem::path PluginProcessorAdapter::GetBundledAssetsPath() const
{
    return mAssetRoot;
}

double PluginProcessorAdapter::GetSampleRate() const
{
    return juce::AudioProcessor::getSampleRate();
}

int PluginProcessorAdapter::GetBlockSize() const
{
    return juce::AudioProcessor::getBlockSize();
}

void PluginProcessorAdapter::OpenAudioPreferences()
{
    if (wrapperType != wrapperType_Standalone)
    {
        return;
    }

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::juce_showStandaloneAudioSettingsDialog();
        return;
    }

    juce::MessageManager::callAsync ([]()
    {
        juce::juce_showStandaloneAudioSettingsDialog();
    });
}

void PluginProcessorAdapter::NotifyStateChanged()
{
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

void PluginProcessorAdapter::NotifyLatencyChanged(int newLatencySamples)
{
    setLatencySamples(newLatencySamples);
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withLatencyChanged(true));
}

double PluginProcessorAdapter::GetHostTempo() const
{
    if (auto* ph = const_cast<PluginProcessorAdapter*>(this)->getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                return *bpm;
        }
    }
    return 120.0;
}

bool PluginProcessorAdapter::IsHostPlaying() const
{
    if (auto* ph = const_cast<PluginProcessorAdapter*>(this)->getPlayHead())
    {
        if (auto pos = ph->getPosition())
            return pos->getIsPlaying();
    }
    return false;
}

bool PluginProcessorAdapter::IsStandalone() const
{
    return wrapperType == wrapperType_Standalone;
}

void PluginProcessorAdapter::setWebMessageCallback(
    std::function<void(const juce::String&)> callback)
{
    std::scoped_lock lock(mWebMessageMutex);
    mWebMessageCallback = std::move(callback);
}

void PluginProcessorAdapter::handleWebMessage(const juce::String& message)
{
    // Handle openUrl locally — open in the system default browser.
    const auto parsed = juce::JSON::parse (message);
    if (auto* obj = parsed.getDynamicObject(); obj != nullptr)
    {
        const auto typeId = juce::Identifier { "type" };
        const auto urlId  = juce::Identifier { "url" };
        if (obj->getProperty (typeId).toString() == "openUrl")
        {
            const auto url = obj->getProperty (urlId).toString();
            if (url.startsWith ("https://") || url.startsWith ("http://"))
                juce::URL (url).launchInDefaultBrowser();
            return;
        }
    }

    mController.HandleUIMessage(message.toStdString());
}

void PluginProcessorAdapter::sendMessageToUI(const juce::String& message)
{
    std::function<void(const juce::String&)> callback;
    {
        std::scoped_lock lock(mWebMessageMutex);
        callback = mWebMessageCallback;
    }
    if (callback) callback(message);
}

std::filesystem::path PluginProcessorAdapter::locateAssetsRoot() const
{
    std::vector<std::filesystem::path> candidates;

    const auto cwd = std::filesystem::path(
        juce::File::getCurrentWorkingDirectory().getFullPathName().toStdString());
    if (!cwd.empty())
    {
        candidates.push_back(cwd / "resources");
        candidates.push_back(cwd / "Resources");
    }

    const auto exeDir = std::filesystem::path(
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory().getFullPathName().toStdString());
    if (!exeDir.empty())
    {
        candidates.push_back(exeDir / "resources");
        candidates.push_back(exeDir / "Resources");
    }

    return guitarfx::ui::ResolveResourceRoot(candidates);
}

// ════════════════════════════════════════════════════════════════════════
// JUCE plugin instance creator
// ════════════════════════════════════════════════════════════════════════

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessorAdapter();
}
