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
#include <cmath>
#include <filesystem>
#include <iostream>

// ════════════════════════════════════════════════════════════════════════
// Parameter string IDs and labels — keep in sync with enum
// ════════════════════════════════════════════════════════════════════════

static constexpr const char* kParamLabels[] = {
    "Input Trim", "Output Trim", "Drive", "Tone",
    "Noise Gate", "Gate Threshold", "Mix",
    "Doubler", "Doubler Delay", "Transpose", "IR Quality",
    "EQ",
    "EQ Low Gain", "EQ Low Freq",
    "EQ Low-Mid Gain", "EQ Low-Mid Freq", "EQ Low-Mid Q",
    "EQ High-Mid Gain", "EQ High-Mid Freq", "EQ High-Mid Q",
    "EQ High Gain", "EQ High Freq"
};

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
      mController(*this),
      mAPVTS(*this, nullptr, "PARAMS", createParameterLayout())
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

    // Push APVTS parameter values into the controller every block
    applyParametersToController();

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

    auto json = nlohmann::json::parse(controllerState, nullptr, false);
    if (json.is_object() && json.contains("parameters") && json["parameters"].is_array())
    {
        int idx = 0;
        for (const auto& value : json["parameters"])
        {
            if (idx >= kParamCount) break;
            if (value.is_number())
            {
                if (auto* param = mAPVTS.getParameter(kParamIds[idx]))
                {
                    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                    {
                        const float normalized = ranged->convertTo0to1(static_cast<float>(value.get<double>()));
                        ranged->setValueNotifyingHost(normalized);
                    }
                }
            }
            idx++;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════
// IPluginHost implementation
// ════════════════════════════════════════════════════════════════════════

void PluginProcessorAdapter::SendMessageToUI(const std::string& jsonMessage)
{
    sendMessageToUI(juce::String(jsonMessage));
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
    juce::String filters;
    switch (type)
    {
        case guitarfx::BrowseFileType::PresetFile:  filters = "*.json"; break;
        case guitarfx::BrowseFileType::ArchiveFile: filters = "*.soundshed.preset;*.soundshed.presets;*.zip"; break;
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
            .getChildFile("SoundshedGuitar")
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
    // In standalone builds, the JUCE standalone wrapper provides audio
    // preferences in its own menu bar. No additional action needed here.
    // For plugin formats (VST3, AU), this is a no-op.
}

void PluginProcessorAdapter::NotifyStateChanged()
{
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

double PluginProcessorAdapter::GetHostTempo() const
{
    if (auto* playHead = const_cast<PluginProcessorAdapter*>(this)->getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            if (auto bpm = pos->getBpm())
                return *bpm;
        }
    }
    return 120.0;
}

bool PluginProcessorAdapter::IsHostPlaying() const
{
    if (auto* playHead = const_cast<PluginProcessorAdapter*>(this)->getPlayHead())
    {
        if (auto pos = playHead->getPosition())
            return pos->getIsPlaying();
    }
    return false;
}

bool PluginProcessorAdapter::IsStandalone() const
{
    return wrapperType == wrapperType_Standalone;
}

// ════════════════════════════════════════════════════════════════════════
// WebView bridge
// ════════════════════════════════════════════════════════════════════════

void PluginProcessorAdapter::setWebMessageCallback(
    std::function<void(const juce::String&)> callback)
{
    std::scoped_lock lock(mWebMessageMutex);
    mWebMessageCallback = std::move(callback);
}

void PluginProcessorAdapter::handleWebMessage(const juce::String& message)
{
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

// ════════════════════════════════════════════════════════════════════════
// Private helpers
// ════════════════════════════════════════════════════════════════════════

juce::AudioProcessorValueTreeState::ParameterLayout
PluginProcessorAdapter::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamInputTrim], 1 }, kParamLabels[kParamInputTrim],
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamOutputTrim], 1 }, kParamLabels[kParamOutputTrim],
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamDrive], 1 }, kParamLabels[kParamDrive],
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamTone], 1 }, kParamLabels[kParamTone],
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ kParamIds[kParamGateEnabled], 1 }, kParamLabels[kParamGateEnabled], false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamGateThreshold], 1 }, kParamLabels[kParamGateThreshold],
        juce::NormalisableRange<float>(-80.0f, -20.0f, 0.1f), -60.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamMix], 1 }, kParamLabels[kParamMix],
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ kParamIds[kParamDoublerEnabled], 1 }, kParamLabels[kParamDoublerEnabled], false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamDoublerDelay], 1 }, kParamLabels[kParamDoublerDelay],
        juce::NormalisableRange<float>(0.5f, 50.0f, 0.1f), 6.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{ kParamIds[kParamTranspose], 1 }, kParamLabels[kParamTranspose],
        -12, 12, 0,
        juce::AudioParameterIntAttributes{}.withLabel("st")));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ kParamIds[kParamIRQuality], 1 }, kParamLabels[kParamIRQuality],
        juce::StringArray{ "Economy", "Standard", "High", "Full" }, 1));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ kParamIds[kParamEQEnabled], 1 }, kParamLabels[kParamEQEnabled], false));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQLowGain], 1 }, kParamLabels[kParamEQLowGain],
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQLowFreq], 1 }, kParamLabels[kParamEQLowFreq],
        juce::NormalisableRange<float>(20.0f, 500.0f, 1.0f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQLowMidGain], 1 }, kParamLabels[kParamEQLowMidGain],
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQLowMidFreq], 1 }, kParamLabels[kParamEQLowMidFreq],
        juce::NormalisableRange<float>(100.0f, 2000.0f, 1.0f), 500.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQLowMidQ], 1 }, kParamLabels[kParamEQLowMidQ],
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.1f), 1.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQHighMidGain], 1 }, kParamLabels[kParamEQHighMidGain],
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQHighMidFreq], 1 }, kParamLabels[kParamEQHighMidFreq],
        juce::NormalisableRange<float>(500.0f, 8000.0f, 1.0f), 2000.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQHighMidQ], 1 }, kParamLabels[kParamEQHighMidQ],
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.1f), 1.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQHighGain], 1 }, kParamLabels[kParamEQHighGain],
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ kParamIds[kParamEQHighFreq], 1 }, kParamLabels[kParamEQHighFreq],
        juce::NormalisableRange<float>(2000.0f, 16000.0f, 1.0f), 8000.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));

    return layout;
}

void PluginProcessorAdapter::applyParametersToController()
{
    auto getValue = [this](const char* id, double fallback) -> double
    {
        if (auto* param = mAPVTS.getParameter(id))
            return static_cast<double>(param->convertFrom0to1(param->getValue()));
        return fallback;
    };

    // Push only core amp/preset parameters each block.
    // Global signal-chain controls (gate/transpose/EQ/doubler) are driven by
    // explicit UI messages (`setGlobalChainParam`) and must not be overwritten
    // here with APVTS defaults every audio block.
    mController.OnParamChange(kParamInputTrim,      getValue(kParamIds[kParamInputTrim], 0.0));
    mController.OnParamChange(kParamOutputTrim,     getValue(kParamIds[kParamOutputTrim], 0.0));
    mController.OnParamChange(kParamDrive,          getValue(kParamIds[kParamDrive], 0.5));
    mController.OnParamChange(kParamTone,           getValue(kParamIds[kParamTone], 0.5));
    mController.OnParamChange(kParamMix,            getValue(kParamIds[kParamMix], 1.0));
    mController.OnParamChange(kParamIRQuality,      getValue(kParamIds[kParamIRQuality], 1.0));
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
