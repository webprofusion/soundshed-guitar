#include "JuceHostedPluginEffect.h"

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "util/FileSystem.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace guitarfx
{
namespace
{
constexpr const char* kPluginStateBase64ConfigKey = "pluginStateBase64";
constexpr const char* kHostedPluginTraceLogFileName = "logs/session-log.txt";
constexpr std::uint64_t kFNVOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFNVPrime = 1099511628211ull;
constexpr char kHostedPluginStateEnvelopeMagic[] = {'G', 'F', 'X', 'H', 'P', 'S', 'T', '1'};
constexpr int kHostedPluginStateEnvelopeVersion = 1;

struct HostedParameterState
{
    int index = -1;
    std::string parameterId;
    float value = 0.0f;
};

struct HostedPluginStateSnapshot
{
    juce::MemoryBlock rawPluginState;
    int currentProgram = -1;
    std::vector<HostedParameterState> parameters;
};

enum class HostedPluginStateEnvelopeDecodeResult
{
    notEnvelope,
    success,
    invalid,
};

std::string SummarizePluginSnapshot(juce::AudioPluginInstance& plugin);

double Clamp(double value, double minimum, double maximum)
{
    return std::min(maximum, std::max(minimum, value));
}

float DbToLinear(double db)
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

juce::String ToJucePath(const std::filesystem::path& path)
{
#if JUCE_WINDOWS
    const auto widePath = path.wstring();
    return juce::String(widePath.c_str());
#else
    return juce::String(path.string());
#endif
}

std::string ToDisplayPath(const std::filesystem::path& path)
{
    return path.string();
}

std::string FromJuceString(const juce::String& value)
{
    return value.toStdString();
}

void AppendHostedPluginTrace(const std::string& message)
{
    FileSystem fileSystem;
    const auto logPath = fileSystem.ResolveSettingsDirectory() / kHostedPluginTraceLogFileName;
    [[maybe_unused]] const auto ensuredLogDir = fileSystem.EnsureDirectory(logPath.parent_path());

    std::ofstream output(logPath, std::ios::app);
    if (output)
        output << "[HostedPluginEffect] " << message << "\n";

    std::cerr << "[JuceHostedPluginEffect] " << message << std::endl;
}

void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kFNVPrime;
    }
}

std::string HashStringForLog(std::string_view value)
{
    std::uint64_t hash = kFNVOffsetBasis;
    HashBytes(hash, value.data(), value.size());

    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string GetHostedParameterId(const juce::AudioProcessorParameter& parameter)
{
    if (const auto* hostedParameter = dynamic_cast<const juce::HostedAudioProcessorParameter*>(&parameter))
        return FromJuceString(hostedParameter->getParameterID());

    return {};
}

bool DecodeBase64State(const std::string& value, juce::MemoryBlock& state)
{
    juce::MemoryOutputStream standardDecoded;
    if (juce::Base64::convertFromBase64(standardDecoded, juce::String(value)))
    {
        state = standardDecoded.getMemoryBlock();
        return true;
    }

    return state.fromBase64Encoding(juce::String(value));
}

bool HasHostedPluginStateData(const HostedPluginStateSnapshot& snapshot)
{
    return !snapshot.rawPluginState.isEmpty()
        || snapshot.currentProgram >= 0
        || !snapshot.parameters.empty();
}

HostedPluginStateSnapshot CaptureHostedPluginStateSnapshot(juce::AudioPluginInstance& plugin)
{
    HostedPluginStateSnapshot snapshot;
    plugin.getStateInformation(snapshot.rawPluginState);

    if (plugin.getNumPrograms() > 1)
        snapshot.currentProgram = plugin.getCurrentProgram();

    const auto& parameters = plugin.getParameters();
    snapshot.parameters.reserve(parameters.size());
    for (auto* parameter : parameters)
    {
        if (parameter == nullptr || !parameter->isAutomatable())
            continue;

        snapshot.parameters.push_back(HostedParameterState{
            parameter->getParameterIndex(),
            GetHostedParameterId(*parameter),
            parameter->getValue(),
        });
    }

    return snapshot;
}

std::string EncodeHostedPluginStateBase64(const HostedPluginStateSnapshot& snapshot)
{
    if (!HasHostedPluginStateData(snapshot))
        return {};

    juce::MemoryOutputStream encoded;
    encoded.write(kHostedPluginStateEnvelopeMagic, sizeof(kHostedPluginStateEnvelopeMagic));
    encoded.writeInt(kHostedPluginStateEnvelopeVersion);
    encoded.writeInt(snapshot.currentProgram);
    encoded.writeInt(static_cast<int>(snapshot.parameters.size()));
    encoded.writeInt(static_cast<int>(snapshot.rawPluginState.getSize()));

    if (!snapshot.rawPluginState.isEmpty())
        encoded.write(snapshot.rawPluginState.getData(), snapshot.rawPluginState.getSize());

    for (const auto& parameter : snapshot.parameters)
    {
        encoded.writeInt(parameter.index);
        encoded.writeString(juce::String(parameter.parameterId));
        encoded.write(&parameter.value, sizeof(parameter.value));
    }

    return FromJuceString(juce::Base64::toBase64(encoded.getData(), encoded.getDataSize()));
}

HostedPluginStateEnvelopeDecodeResult DecodeHostedPluginStateEnvelope(const juce::MemoryBlock& state,
                                                                     HostedPluginStateSnapshot& snapshot)
{
    snapshot = {};

    if (state.getSize() < sizeof(kHostedPluginStateEnvelopeMagic))
        return HostedPluginStateEnvelopeDecodeResult::notEnvelope;

    if (std::memcmp(state.getData(), kHostedPluginStateEnvelopeMagic, sizeof(kHostedPluginStateEnvelopeMagic)) != 0)
        return HostedPluginStateEnvelopeDecodeResult::notEnvelope;

    juce::MemoryInputStream stream(state, false);
    char magic[sizeof(kHostedPluginStateEnvelopeMagic)]{};
    if (stream.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic))
        || std::memcmp(magic, kHostedPluginStateEnvelopeMagic, sizeof(magic)) != 0)
    {
        return HostedPluginStateEnvelopeDecodeResult::invalid;
    }

    if (stream.readInt() != kHostedPluginStateEnvelopeVersion)
        return HostedPluginStateEnvelopeDecodeResult::invalid;

    snapshot.currentProgram = stream.readInt();
    const int parameterCount = stream.readInt();
    const int rawStateSize = stream.readInt();
    if (parameterCount < 0 || parameterCount > 32768 || rawStateSize < 0)
        return HostedPluginStateEnvelopeDecodeResult::invalid;

    if (stream.getNumBytesRemaining() < rawStateSize)
        return HostedPluginStateEnvelopeDecodeResult::invalid;

    snapshot.rawPluginState.setSize(static_cast<size_t>(rawStateSize));
    if (rawStateSize > 0
        && stream.read(snapshot.rawPluginState.getData(), rawStateSize) != rawStateSize)
    {
        return HostedPluginStateEnvelopeDecodeResult::invalid;
    }

    snapshot.parameters.reserve(static_cast<size_t>(parameterCount));
    for (int index = 0; index < parameterCount; ++index)
    {
        HostedParameterState parameter;
        parameter.index = stream.readInt();
        parameter.parameterId = FromJuceString(stream.readString());

        float value = 0.0f;
        if (stream.read(&value, sizeof(value)) != static_cast<int>(sizeof(value)))
            return HostedPluginStateEnvelopeDecodeResult::invalid;

        parameter.value = value;
        snapshot.parameters.push_back(std::move(parameter));
    }

    return HostedPluginStateEnvelopeDecodeResult::success;
}

bool DecodeHostedPluginStateBase64(const std::string& value, HostedPluginStateSnapshot& snapshot)
{
    juce::MemoryBlock decoded;
    if (!DecodeBase64State(value, decoded))
        return false;

    const auto result = DecodeHostedPluginStateEnvelope(decoded, snapshot);
    if (result == HostedPluginStateEnvelopeDecodeResult::invalid)
        return false;

    if (result == HostedPluginStateEnvelopeDecodeResult::notEnvelope)
    {
        snapshot = {};
        snapshot.rawPluginState = decoded;
    }

    return true;
}

juce::AudioProcessorParameter* FindHostedPluginParameter(juce::AudioPluginInstance& plugin,
                                                         const HostedParameterState& savedParameter)
{
    const auto& parameters = plugin.getParameters();

    if (!savedParameter.parameterId.empty())
    {
        for (auto* parameter : parameters)
        {
            if (parameter == nullptr || !parameter->isAutomatable())
                continue;

            if (GetHostedParameterId(*parameter) == savedParameter.parameterId)
                return parameter;
        }
    }

    if (savedParameter.index >= 0 && savedParameter.index < static_cast<int>(parameters.size()))
    {
        auto* parameter = parameters[static_cast<size_t>(savedParameter.index)];
        if (parameter != nullptr && parameter->isAutomatable())
            return parameter;
    }

    return nullptr;
}

std::string ApplyHostedPluginStateSnapshot(juce::AudioPluginInstance& plugin,
                                           const HostedPluginStateSnapshot& snapshot)
{
    if (!snapshot.rawPluginState.isEmpty())
    {
        plugin.setStateInformation(snapshot.rawPluginState.getData(),
                                   static_cast<int>(snapshot.rawPluginState.getSize()));
    }

    if (snapshot.currentProgram >= 0 && plugin.getNumPrograms() > 1)
    {
        const int programCount = plugin.getNumPrograms();
        if (snapshot.currentProgram < programCount)
            plugin.setCurrentProgram(snapshot.currentProgram);
    }

    for (const auto& savedParameter : snapshot.parameters)
    {
        auto* parameter = FindHostedPluginParameter(plugin, savedParameter);
        if (parameter == nullptr)
            continue;

        if (std::abs(parameter->getValue() - savedParameter.value) <= 1.0e-6f)
            continue;

        parameter->setValueNotifyingHost(savedParameter.value);
    }

    return SummarizePluginSnapshot(plugin);
}

std::string SummarizePluginSnapshot(juce::AudioPluginInstance& plugin)
{
    std::uint64_t hash = kFNVOffsetBasis;
    std::size_t parameterCount = 0;
    std::ostringstream preview;
    preview << std::fixed << std::setprecision(6);

    const auto& parameters = plugin.getParameters();
    for (auto* parameter : parameters)
    {
        if (parameter == nullptr)
            continue;

        const float value = parameter->getValue();
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        HashBytes(hash, &bits, sizeof(bits));

        if (parameterCount < 6)
        {
            if (parameterCount > 0)
                preview << ',';
            preview << value;
        }

        ++parameterCount;
    }

    int currentProgram = -1;
    if (plugin.getNumPrograms() > 1)
        currentProgram = plugin.getCurrentProgram();
    HashBytes(hash, &currentProgram, sizeof(currentProgram));

    std::ostringstream summary;
    summary << "program=";
    if (currentProgram >= 0)
        summary << currentProgram;
    else
        summary << "<none>";
    summary << ", paramCount=" << parameterCount
            << ", paramHash=0x" << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec
            << ", preview=[" << preview.str() << ']';
    return summary.str();
}

class HostedPluginEditorWindow final : public juce::DocumentWindow
{
public:
    HostedPluginEditorWindow(const juce::String& title,
                             juce::AudioProcessorEditor* editor,
                             std::function<void()> onClose)
        : DocumentWindow(title,
                         juce::Desktop::getInstance().getDefaultLookAndFeel()
                             .findColour(juce::ResizableWindow::backgroundColourId),
                         juce::DocumentWindow::closeButton)
        , mOnClose(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        centreWithSize(std::max(360, getWidth()), std::max(220, getHeight()));
        setResizable(editor != nullptr && editor->isResizable(), true);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        if (mOnClose)
            mOnClose();
    }

private:
    std::function<void()> mOnClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginEditorWindow)
};
} // namespace

JuceHostedPluginEffect::JuceHostedPluginEffect() = default;

JuceHostedPluginEffect::~JuceHostedPluginEffect()
{
    ClosePluginEditor();
    ReleaseHostedPlugin();
}

void JuceHostedPluginEffect::EnsureFormatsAdded()
{
    if (mFormatsAdded)
        return;

    juce::addDefaultFormatsToManager(mFormatManager);
    mFormatsAdded = true;
}

void JuceHostedPluginEffect::Prepare(double sampleRate, int maxBlockSize)
{
    if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;

    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;
    mWorkBuffer.setSize(2, maxBlockSize, false, false, true);
    AppendHostedPluginTrace("Prepare sampleRate=" + std::to_string(sampleRate)
        + ", blockSize=" + std::to_string(maxBlockSize)
        + ", pluginLoaded=" + std::string{mPlugin ? "true" : "false"}
        + ", pendingStateLength=" + std::to_string(mPluginStateBase64.size()));
    PrepareLoadedPlugin();
}

void JuceHostedPluginEffect::Reset()
{
    mMidiBuffer.clear();
    if (mPlugin)
    {
        AppendHostedPluginTrace("Reset plugin=" + FromJuceString(mPlugin->getName()));
        mPlugin->reset();
    }
}

void JuceHostedPluginEffect::Process(float** inputs, float** outputs, int numSamples)
{
    if (!inputs || !outputs || numSamples <= 0)
        return;

    if (!mPlugin || !mPrepared || numSamples > mWorkBuffer.getNumSamples())
    {
        Passthrough(inputs, outputs, numSamples);
        return;
    }

    CopyInputToWorkBuffer(inputs, numSamples);
    mMidiBuffer.clear();
    mPlugin->processBlock(mWorkBuffer, mMidiBuffer);
    CopyWorkBufferToOutputs(inputs, outputs, numSamples);
}

void JuceHostedPluginEffect::SetParam(const std::string& key, double value)
{
    if (key == "mix")
        mMix = Clamp(value, 0.0, 1.0);
    else if (key == "inputGain")
        mInputGainDb = Clamp(value, -24.0, 24.0);
    else if (key == "outputGain")
        mOutputGainDb = Clamp(value, -24.0, 24.0);
}

double JuceHostedPluginEffect::GetParam(const std::string& key) const
{
    if (key == "mix")
        return mMix;
    if (key == "inputGain")
        return mInputGainDb;
    if (key == "outputGain")
        return mOutputGainDb;
    return 0.0;
}

void JuceHostedPluginEffect::SetConfig(const std::string& key, const std::string& value)
{
    if (key == "pluginPath")
    {
        AppendHostedPluginTrace("SetConfig pluginPath=" + value);
        if (!value.empty())
            LoadPluginFromPath(std::filesystem::path(value));
        return;
    }

    if (key == "pluginFormat")
    {
        mPluginFormat = value;
        return;
    }

    if (key == "pluginIdentifier")
    {
        mPluginIdentifier = value;
        return;
    }

    if (key == kPluginStateBase64ConfigKey)
    {
        mPluginStateBase64 = value;
        AppendHostedPluginTrace("SetConfig pluginStateBase64 length=" + std::to_string(value.size())
            + ", prepared=" + std::string{mPrepared ? "true" : "false"}
            + ", pluginLoaded=" + std::string{mPlugin ? "true" : "false"});
        ApplyPendingPluginState();
        return;
    }

    if (key == "showPluginEditor" || key == "openPluginEditor")
    {
        if (value != "0" && value != "false")
            OpenPluginEditor();
        return;
    }
}

void JuceHostedPluginEffect::SetRuntimeConfigChangedCallback(RuntimeConfigChangedCallback callback)
{
    mRuntimeConfigChangedCallback = std::move(callback);
}

std::string JuceHostedPluginEffect::GetConfig(const std::string& key) const
{
    if (key == "pluginPath")
        return ToDisplayPath(mPluginPath);
    if (key == "pluginFormat")
        return mPluginFormat;
    if (key == "pluginIdentifier")
        return mPluginIdentifier;
    if (key == "pluginName")
        return FromJuceString(mPluginDescription.name);
    if (key == kPluginStateBase64ConfigKey)
        return CapturePluginStateBase64();
    if (key == "lastError")
        return mLastError;
    return {};
}

bool JuceHostedPluginEffect::LoadResource(const std::filesystem::path& path)
{
    AppendHostedPluginTrace("LoadResource path=" + ToDisplayPath(path));
    return LoadPluginFromPath(path);
}

bool JuceHostedPluginEffect::LoadResources(const std::vector<ResourceRef>& refs,
                                           const std::vector<std::filesystem::path>& paths)
{
    for (std::size_t i = 0; i < refs.size(); ++i)
    {
        const auto& ref = refs[i];
        if (ref.resourceType != "plugin" && ref.resourceType != "audio-plugin" && ref.resourceType != "midi-plugin")
            continue;

        if (i < paths.size())
            return LoadPluginFromPath(paths[i]);
    }

    if (!paths.empty())
        return LoadPluginFromPath(paths.front());

    return false;
}

int JuceHostedPluginEffect::GetLatencySamples() const
{
    return mPlugin ? mPlugin->getLatencySamples() : 0;
}

bool JuceHostedPluginEffect::LoadPluginFromPath(const std::filesystem::path& path)
{
    EnsureFormatsAdded();
    AppendHostedPluginTrace("LoadPluginFromPath begin path=" + ToDisplayPath(path)
        + ", pendingStateLength=" + std::to_string(mPluginStateBase64.size())
        + ", sampleRate=" + std::to_string(mSampleRate)
        + ", blockSize=" + std::to_string(mMaxBlockSize));

    const juce::File pluginFile(ToJucePath(path));
    if (!pluginFile.exists())
    {
        SetError("Plugin file does not exist: " + ToDisplayPath(path));
        ReleaseHostedPlugin();
        return false;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    const auto fileOrIdentifier = pluginFile.getFullPathName();

    for (int i = 0; i < mFormatManager.getNumFormats(); ++i)
    {
        auto* format = mFormatManager.getFormat(i);
        if (!format)
            continue;

        if (!mPluginFormat.empty() && !format->getName().equalsIgnoreCase(juce::String(mPluginFormat)))
            continue;

        if (format->fileMightContainThisPluginType(fileOrIdentifier) || !mPluginFormat.empty())
            format->findAllTypesForFile(descriptions, fileOrIdentifier);
    }

    if (descriptions.isEmpty())
    {
        SetError("No JUCE-supported plugin types were found in: " + ToDisplayPath(path));
        ReleaseHostedPlugin();
        return false;
    }

    juce::PluginDescription* selected = descriptions.getFirst();
    if (!mPluginIdentifier.empty())
    {
        for (auto* description : descriptions)
        {
            if (description && (description->createIdentifierString() == juce::String(mPluginIdentifier)
                || description->fileOrIdentifier == juce::String(mPluginIdentifier)))
            {
                selected = description;
                break;
            }
        }
    }

    if (!selected)
    {
        SetError("Plugin scan returned no selectable plugin descriptions");
        ReleaseHostedPlugin();
        return false;
    }

    juce::String error;
    auto instance = mFormatManager.createPluginInstance(*selected, mSampleRate, mMaxBlockSize, error);
    if (!instance)
    {
        SetError(error.isNotEmpty() ? FromJuceString(error) : "JUCE failed to instantiate plugin");
        ReleaseHostedPlugin();
        return false;
    }

    if (!ConfigurePluginBuses(*instance))
    {
        SetError("Plugin does not support a mono or stereo main bus layout");
        instance->releaseResources();
        ReleaseHostedPlugin();
        return false;
    }

    mPluginDescription = *selected;
    mPluginPath = path;
    mPluginFormat = FromJuceString(selected->pluginFormatName);
    mPluginIdentifier = FromJuceString(selected->createIdentifierString());
    ClosePluginEditor();
    ReleaseHostedPlugin();
    mPlugin = std::move(instance);
    AttachHostedPluginListeners();
    mLastError.clear();
    AppendHostedPluginTrace("LoadPluginFromPath instantiated name=" + FromJuceString(mPluginDescription.name)
        + ", format=" + mPluginFormat + ", identifier=" + mPluginIdentifier);
    if (!mPluginStateBase64.empty())
    {
        AppendHostedPluginTrace("LoadPluginFromPath applying pending state length=" + std::to_string(mPluginStateBase64.size()));
        ApplyPluginStateBase64(mPluginStateBase64);
    }
    PrepareLoadedPlugin();
    return true;
}

bool JuceHostedPluginEffect::ConfigurePluginBuses(juce::AudioPluginInstance& plugin) const
{
    const juce::AudioChannelSet stereo = juce::AudioChannelSet::stereo();
    const juce::AudioChannelSet mono = juce::AudioChannelSet::mono();
    const juce::AudioChannelSet disabled = juce::AudioChannelSet::disabled();

    const bool hasMainInput = plugin.getBusCount(true) > 0;
    const bool hasMainOutput = plugin.getBusCount(false) > 0;
    if (!hasMainOutput)
        return false;

    const juce::AudioProcessor::BusesLayout stereoLayout{
        hasMainInput ? juce::Array<juce::AudioChannelSet>{stereo} : juce::Array<juce::AudioChannelSet>{},
        juce::Array<juce::AudioChannelSet>{stereo}
    };
    if (plugin.checkBusesLayoutSupported(stereoLayout) && plugin.setBusesLayout(stereoLayout))
        return true;

    const juce::AudioProcessor::BusesLayout monoLayout{
        hasMainInput ? juce::Array<juce::AudioChannelSet>{mono} : juce::Array<juce::AudioChannelSet>{},
        juce::Array<juce::AudioChannelSet>{mono}
    };
    if (plugin.checkBusesLayoutSupported(monoLayout) && plugin.setBusesLayout(monoLayout))
        return true;

    auto current = plugin.getBusesLayout();
    if (current.outputBuses.size() > 0 && current.outputBuses.getReference(0).size() > 0)
    {
        for (int i = 1; i < current.inputBuses.size(); ++i)
            current.inputBuses.getReference(i) = disabled;
        for (int i = 1; i < current.outputBuses.size(); ++i)
            current.outputBuses.getReference(i) = disabled;
        return plugin.checkBusesLayoutSupported(current) && plugin.setBusesLayout(current);
    }

    return false;
}

void JuceHostedPluginEffect::PrepareLoadedPlugin()
{
    if (!mPlugin || !mPrepared)
        return;

    AppendHostedPluginTrace("PrepareLoadedPlugin plugin=" + FromJuceString(mPlugin->getName())
        + ", sampleRate=" + std::to_string(mSampleRate)
        + ", blockSize=" + std::to_string(mMaxBlockSize)
        + ", pendingStateLength=" + std::to_string(mPluginStateBase64.size()));
    mPlugin->setRateAndBufferSizeDetails(mSampleRate, mMaxBlockSize);
    mPlugin->prepareToPlay(mSampleRate, mMaxBlockSize);
    ApplyPendingPluginState();
}

void JuceHostedPluginEffect::CopyInputToWorkBuffer(float** inputs, int numSamples)
{
    const float inputGain = DbToLinear(mInputGainDb);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* dest = mWorkBuffer.getWritePointer(ch);
        const float* source = inputs[ch];
        if (source)
        {
            for (int i = 0; i < numSamples; ++i)
                dest[i] = source[i] * inputGain;
        }
        else
        {
            std::fill(dest, dest + numSamples, 0.0f);
        }
    }
}

void JuceHostedPluginEffect::CopyWorkBufferToOutputs(float** inputs, float** outputs, int numSamples)
{
    const float outputGain = DbToLinear(mOutputGainDb);
    for (int ch = 0; ch < 2; ++ch)
    {
        if (!outputs[ch])
            continue;

        const float* dry = inputs[ch];
        const float* wet = mWorkBuffer.getReadPointer(std::min(ch, mWorkBuffer.getNumChannels() - 1));
        for (int i = 0; i < numSamples; ++i)
        {
            const float drySample = dry ? dry[i] : 0.0f;
            outputs[ch][i] = static_cast<float>((drySample * (1.0 - mMix)) + (wet[i] * outputGain * mMix));
        }
    }
}

void JuceHostedPluginEffect::Passthrough(float** inputs, float** outputs, int numSamples) const
{
    for (int ch = 0; ch < 2; ++ch)
    {
        if (!outputs[ch])
            continue;

        const float* source = inputs[ch];
        if (source)
            std::copy(source, source + numSamples, outputs[ch]);
        else
            std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
    }
}

void JuceHostedPluginEffect::ApplyPluginStateBase64(const std::string& value)
{
    if (!mPlugin || value.empty())
        return;

    AppendHostedPluginTrace("ApplyPluginStateBase64 begin plugin=" + FromJuceString(mPlugin->getName())
        + ", encodedLength=" + std::to_string(value.size())
        + ", encodedHash=" + HashStringForLog(value)
        + ", preApply=" + SummarizePluginSnapshot(*mPlugin));

    HostedPluginStateSnapshot snapshot;
    if (!DecodeHostedPluginStateBase64(value, snapshot))
    {
        SetError("Invalid hosted plugin state encoding");
        return;
    }

    AppendHostedPluginTrace("ApplyPluginStateBase64 decodedBytes=" + std::to_string(snapshot.rawPluginState.getSize())
        + ", currentProgram=" + std::to_string(snapshot.currentProgram)
        + ", parameterCount=" + std::to_string(snapshot.parameters.size()));

    const auto applyState = [this, snapshot]() -> std::string
    {
        if (!mPlugin)
            return "plugin missing";

        ++mAutoCaptureSuppressionDepth;
        return ApplyHostedPluginStateSnapshot(*mPlugin, snapshot);
    };

    std::string applySummary;

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        applySummary = applyState();
        --mAutoCaptureSuppressionDepth;
    }
    else if (auto result = juce::MessageManager::callSync(applyState))
    {
        applySummary = *result;
        --mAutoCaptureSuppressionDepth;
    }
    else
    {
        mAutoCaptureSuppressionDepth.store(0, std::memory_order_release);
        SetError("Failed to restore hosted plugin state on the message thread");
        return;
    }

    AppendHostedPluginTrace("ApplyPluginStateBase64 complete plugin=" + FromJuceString(mPlugin->getName())
        + ", postApply=" + applySummary);
    mLastError.clear();
}

void JuceHostedPluginEffect::ApplyPendingPluginState()
{
    if (mPlugin && mPrepared && !mPluginStateBase64.empty())
    {
        AppendHostedPluginTrace("ApplyPendingPluginState applying plugin=" + FromJuceString(mPlugin->getName())
            + ", stateLength=" + std::to_string(mPluginStateBase64.size()));
        ApplyPluginStateBase64(mPluginStateBase64);
    }
    else
    {
        AppendHostedPluginTrace("ApplyPendingPluginState skipped pluginLoaded=" + std::string{mPlugin ? "true" : "false"}
            + ", prepared=" + std::string{mPrepared ? "true" : "false"}
            + ", stateLength=" + std::to_string(mPluginStateBase64.size()));
    }
}

std::string JuceHostedPluginEffect::CapturePluginStateBase64() const
{
    if (!mPlugin)
        return mPluginStateBase64;

    const auto captureState = [this]() -> std::string
    {
        if (!mPlugin)
            return mPluginStateBase64;

        const auto snapshot = CaptureHostedPluginStateSnapshot(*mPlugin);
        return EncodeHostedPluginStateBase64(snapshot);
    };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return captureState();
    }

    if (auto captured = juce::MessageManager::callSync(captureState))
    {
        return *captured;
    }

    return {};
}

void JuceHostedPluginEffect::AttachHostedPluginListeners()
{
    if (!mPlugin || mHostedPluginListenerAttached)
        return;

    mPlugin->addListener(this);
    AttachHostedPluginParameterListeners();
    mHostedPluginListenerAttached = true;
}

void JuceHostedPluginEffect::AttachHostedPluginParameterListeners()
{
    DetachHostedPluginParameterListeners();

    if (!mPlugin)
        return;

    const auto& parameters = mPlugin->getParameters();
    mHostedParametersWithListeners.reserve(parameters.size());
    for (auto* parameter : parameters)
    {
        if (parameter == nullptr)
            continue;

        parameter->addListener(this);
        mHostedParametersWithListeners.push_back(parameter);
    }
}

void JuceHostedPluginEffect::DetachHostedPluginParameterListeners()
{
    for (auto* parameter : mHostedParametersWithListeners)
    {
        if (parameter != nullptr)
            parameter->removeListener(this);
    }

    mHostedParametersWithListeners.clear();
}

void JuceHostedPluginEffect::ReleaseHostedPlugin()
{
    cancelPendingUpdate();
    mForceAutoCaptureNotification.store(false, std::memory_order_release);
    mAutoCaptureSuppressionDepth.store(0, std::memory_order_release);

    DetachHostedPluginParameterListeners();

    if (mHostedPluginListenerAttached && mPlugin)
        mPlugin->removeListener(this);

    mHostedPluginListenerAttached = false;

    if (mPlugin)
    {
        mPlugin->releaseResources();
        mPlugin.reset();
    }
}

void JuceHostedPluginEffect::ScheduleAutoCapture(bool forceNotify)
{
    if (!mPlugin)
        return;

    if (forceNotify)
        mForceAutoCaptureNotification.store(true, std::memory_order_release);

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        CaptureAndPublishPluginState(mForceAutoCaptureNotification.exchange(false, std::memory_order_acq_rel));
        return;
    }

    triggerAsyncUpdate();
}

void JuceHostedPluginEffect::CaptureAndPublishPluginState(bool forceNotify)
{
    if (!mPlugin)
        return;

    PublishCapturedPluginState(CapturePluginStateBase64(), forceNotify);
}

void JuceHostedPluginEffect::PublishCapturedPluginState(const std::string& capturedState, bool forceNotify)
{
    const bool changed = capturedState != mPluginStateBase64;
    if (!changed && !forceNotify)
        return;

    mPluginStateBase64 = capturedState;

    if (mRuntimeConfigChangedCallback)
        mRuntimeConfigChangedCallback(kPluginStateBase64ConfigKey, mPluginStateBase64);
}

void JuceHostedPluginEffect::EnsurePluginStateBaseline()
{
    if (!mPlugin || !mPluginStateBase64.empty())
        return;

    mPluginStateBase64 = CapturePluginStateBase64();
}

void JuceHostedPluginEffect::OpenPluginEditor()
{
    if (!mPlugin)
    {
        SetError("Cannot open hosted plugin UI before a plugin is loaded");
        return;
    }

    auto open = [this]()
    {
        if (!mPlugin)
            return;

        if (mEditorWindow)
        {
            EnsurePluginStateBaseline();
            mEditorWindow->setVisible(true);
            mEditorWindow->toFront(true);
            return;
        }

        auto* editor = mPlugin->hasEditor()
            ? mPlugin->createEditorIfNeeded()
            : static_cast<juce::AudioProcessorEditor*>(new juce::GenericAudioProcessorEditor(*mPlugin));
        if (!editor)
        {
            SetError("Hosted plugin did not provide an editor");
            return;
        }

        const auto title = mPluginDescription.name.isNotEmpty()
            ? mPluginDescription.name
            : mPlugin->getName();
        EnsurePluginStateBaseline();
        mEditorWindow = std::make_unique<HostedPluginEditorWindow>(title, editor, [this]()
        {
            ScheduleAutoCapture(false);
        });
    };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        open();
    else
        juce::MessageManager::callAsync(std::move(open));
}

void JuceHostedPluginEffect::ClosePluginEditor()
{
    if (!mEditorWindow)
        return;

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        mEditorWindow.reset();
        return;
    }

    auto* window = mEditorWindow.release();
    juce::MessageManager::callAsync([window]() { delete window; });
}

void JuceHostedPluginEffect::SetError(const std::string& message)
{
    mLastError = message;
    std::cerr << "[JuceHostedPluginEffect] " << message << std::endl;
}

void JuceHostedPluginEffect::parameterValueChanged(int,
                                                   float)
{
    if (!mPlugin || mAutoCaptureSuppressionDepth.load(std::memory_order_acquire) > 0)
        return;

    ScheduleAutoCapture(false);
}

void JuceHostedPluginEffect::parameterGestureChanged(int,
                                                     bool gestureIsStarting)
{
    if (gestureIsStarting || !mPlugin || mAutoCaptureSuppressionDepth.load(std::memory_order_acquire) > 0)
        return;

    ScheduleAutoCapture(false);
}

void JuceHostedPluginEffect::audioProcessorParameterChanged(juce::AudioProcessor* processor,
                                                            int,
                                                            float)
{
    if (processor != mPlugin.get() || mAutoCaptureSuppressionDepth.load(std::memory_order_acquire) > 0)
        return;

    ScheduleAutoCapture(false);
}

void JuceHostedPluginEffect::audioProcessorChanged(juce::AudioProcessor* processor,
                                                   const juce::AudioProcessorListener::ChangeDetails& details)
{
    if (processor != mPlugin.get() || mAutoCaptureSuppressionDepth.load(std::memory_order_acquire) > 0)
        return;

    if (details.parameterInfoChanged)
        AttachHostedPluginParameterListeners();

    if (details.programChanged || details.nonParameterStateChanged || details.parameterInfoChanged)
        ScheduleAutoCapture(details.parameterInfoChanged);
}

void JuceHostedPluginEffect::handleAsyncUpdate()
{
    CaptureAndPublishPluginState(mForceAutoCaptureNotification.exchange(false, std::memory_order_acq_rel));
}

void RegisterJuceHostedPluginEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kPluginHost;
    info.aliases = {"plugin_host", "juce_plugin_host"};
    info.displayName = "Plugin Host";
    info.category = "utility";
    info.description = "Host an external JUCE-supported audio plugin inside the signal path";
    info.requiresResource = true;
    info.resourceType = "plugin";
    info.parameters = {
        {"mix", "Mix", 1.0, 0.0, 1.0, "", "", false, 0.01},
        {"inputGain", "Input", 0.0, -24.0, 24.0, "dB"},
        {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"}
    };
    info.exposedResources = {
        {"plugin", "Plugin", "", "plugin", 0, true}
    };

    EffectRegistry::Instance().Register(info.type, info, []()
    {
        return std::make_unique<JuceHostedPluginEffect>();
    });
}

} // namespace guitarfx
