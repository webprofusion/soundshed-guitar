#include "JuceHostedPluginEffect.h"

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "resources/PluginPathUtils.h"
#include "util/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace guitarfx
{
    namespace
    {
        JuceHostedPluginEffect* sActiveHostedPluginEditorOwner = nullptr;

        constexpr const char* kPluginStateBase64ConfigKey = "pluginStateBase64";
        constexpr const char* kPluginStableIdConfigKey = "pluginStableId";
        constexpr const char* kPluginIdentifierConfigKey = "pluginIdentifier";
        constexpr const char* kPluginFormatConfigKey = "pluginFormat";
        constexpr const char* kPluginNameConfigKey = "pluginName";
        constexpr const char* kPluginManufacturerConfigKey = "pluginManufacturer";
        constexpr const char* kPluginLastErrorCodeConfigKey = "lastErrorCode";
        constexpr const char* kHostedPluginTraceLogFileName = "logs/session-log.txt";
        constexpr std::uint64_t kFNVOffsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t kFNVPrime = 1099511628211ull;

        // Backward-compatible hosted plugin state envelope: raw JUCE state chunk plus
        // current program and host-visible automatable parameter values.
        constexpr char kHostedPluginStateEnvelopeMagic[] = { 'G', 'F', 'X', 'H', 'P', 'S', 'T', '1' };
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

        enum class HostedPluginStateEnvelopeDecodeResult {
            notEnvelope,
            success,
            invalid,
        };

        std::string SummarizePluginSnapshot (juce::AudioPluginInstance& plugin);

        double Clamp (double value, double minimum, double maximum)
        {
            return std::min (maximum, std::max (minimum, value));
        }

        float DbToLinear (double db)
        {
            return static_cast<float> (std::pow (10.0, db / 20.0));
        }

        juce::String ToJucePath (const std::filesystem::path& path)
        {
#if JUCE_WINDOWS
            const auto widePath = path.wstring();
            return juce::String (widePath.c_str());
#else
            return juce::String (path.string());
#endif
        }

        std::string ToDisplayPath (const std::filesystem::path& path)
        {
            return path.string();
        }

        std::string FromJuceString (const juce::String& value)
        {
            return value.toStdString();
        }

        std::string NormalizePluginIdentityToken (std::string_view value)
        {
            std::string normalized;
            normalized.reserve (value.size());

            bool lastWasSeparator = false;
            for (const char raw : value)
            {
                const unsigned char ch = static_cast<unsigned char> (raw);
                if (std::isalnum (ch))
                {
                    normalized.push_back (static_cast<char> (std::tolower (ch)));
                    lastWasSeparator = false;
                    continue;
                }

                if (!normalized.empty() && !lastWasSeparator)
                {
                    normalized.push_back ('-');
                    lastWasSeparator = true;
                }
            }

            while (!normalized.empty() && normalized.back() == '-')
                normalized.pop_back();

            return normalized;
        }

        bool ContainsCaseInsensitive (std::string_view text, std::string_view token)
        {
            if (token.empty() || text.size() < token.size())
                return false;

            auto toLowerAscii = [] (unsigned char ch) {
                return static_cast<char> (std::tolower (ch));
            };

            for (std::size_t i = 0; i <= text.size() - token.size(); ++i)
            {
                bool matches = true;
                for (std::size_t j = 0; j < token.size(); ++j)
                {
                    if (toLowerAscii (static_cast<unsigned char> (text[i + j]))
                        != toLowerAscii (static_cast<unsigned char> (token[j])))
                    {
                        matches = false;
                        break;
                    }
                }
                if (matches)
                    return true;
            }

            return false;
        }

        bool IsBlockedSelfHostedPluginCandidate (const juce::PluginDescription& description,
                                                 const std::filesystem::path& resolvedPath)
        {
            const std::string pluginName = FromJuceString (description.name);
            const std::string pluginIdentifier = FromJuceString (description.createIdentifierString());
            const std::string pluginFileOrId = FromJuceString (description.fileOrIdentifier);
            const std::string pluginFormat = FromJuceString (description.pluginFormatName);
            const std::string pathText = ToDisplayPath (resolvedPath);

            return ContainsCaseInsensitive (pluginName, "soundshed");
        }

        std::string BuildPluginStableId (const juce::PluginDescription& description,
            const std::filesystem::path& pluginPath)
        {
            const std::string normalizedManufacturer = NormalizePluginIdentityToken (FromJuceString (description.manufacturerName));
            std::string normalizedName = NormalizePluginIdentityToken (FromJuceString (description.name));
            if (normalizedName.empty())
                normalizedName = NormalizePluginIdentityToken (pluginPath.stem().string());

            if (!normalizedManufacturer.empty() && !normalizedName.empty())
                return normalizedManufacturer + "." + normalizedName;
            if (!normalizedName.empty())
                return normalizedName;
            return normalizedManufacturer;
        }

        std::string ToLowerAscii (std::string value)
        {
            std::transform (value.begin(), value.end(), value.begin(),
                [] (unsigned char ch) { return static_cast<char> (std::tolower (ch)); });
            return value;
        }

        // Map stored format hints ("vst3", "au", "AudioUnit", ...) onto the names JUCE
        // format objects report, so resource-metadata hints never block scanning.
        juce::String NormalizePluginFormatHint (const std::string& hint)
        {
            const std::string lower = ToLowerAscii (hint);
            if (lower.empty())
                return {};
            if (lower == "vst3")
                return "VST3";
            if (lower == "au" || lower == "audiounit" || lower == "audio unit" || lower == "component")
                return "AudioUnit";
            if (lower == "lv2")
                return "LV2";
            if (lower == "vst" || lower == "vst2")
                return "VST";
            return juce::String (hint);
        }

        std::string GetSupportedPluginFormatsDescription()
        {
            using pluginpath::PluginFormat;
            using pluginpath::PluginFormatDisplayName;

            std::string description { PluginFormatDisplayName (PluginFormat::VST3) };
#if JUCE_MAC
            description += ", ";
            description += PluginFormatDisplayName (PluginFormat::AudioUnit);
#endif
            description += " and ";
            description += PluginFormatDisplayName (PluginFormat::LV2);
            return description;
        }

        // The single owner of "why can't this load?" wording. The browse dialog
        // deliberately does not second-guess a selection — it hands whatever the user
        // picked to the loader, and every rejection is explained from here.
        //
        // Returns an empty string when the path looks like a supported plugin type.
        std::string DescribeUnsupportedPluginFile (const std::filesystem::path& path)
        {
            // A plugin is a file or a bundle directory. A plain folder is neither.
            std::error_code ec;
            if (std::filesystem::is_directory (path, ec) && !ec
                && !pluginpath::HasPluginBundleSuffix (path))
            {
                return "'" + ToDisplayPath (path) + "' is a folder, not a plugin. Select the plugin"
                       " itself, or a file inside its bundle. Supported formats: "
                       + GetSupportedPluginFormatsDescription() + ".";
            }

            switch (pluginpath::PluginFormatFromPath (path))
            {
                case pluginpath::PluginFormat::VST3:
                case pluginpath::PluginFormat::LV2:
                    return {};

                case pluginpath::PluginFormat::AudioUnit:
#if JUCE_MAC
                    return {};
#else
                    return "Audio Unit plugins are only supported on macOS. Please select the VST3 version of this plugin instead.";
#endif

                case pluginpath::PluginFormat::VST2:
                    return "This looks like a VST2 plugin, which is not supported. Please install and select the VST3 version of this plugin instead.";

                case pluginpath::PluginFormat::CLAP:
                    return "CLAP plugins are not supported. Please select the VST3 version of this plugin instead.";

                case pluginpath::PluginFormat::AAX:
                    return "AAX (Pro Tools) plugins are not supported. Please select the VST3 version of this plugin instead.";

                case pluginpath::PluginFormat::Unknown:
                    break;
            }

            const std::string extension = ToLowerAscii (path.extension().string());

#if JUCE_LINUX
            // A bare .so outside any bundle can still be an LV2 binary; let the scanner decide.
            if (extension == ".so")
                return {};
#endif

            return "Unrecognized plugin file type '" + (extension.empty() ? std::string { "(none)" } : extension)
                   + "'. Supported plugin formats on this platform: " + GetSupportedPluginFormatsDescription() + ".";
        }

        std::string DescribeRegisteredFormats (const juce::AudioPluginFormatManager& manager)
        {
            std::ostringstream stream;
            stream << "count=" << manager.getNumFormats() << " [";
            for (int i = 0; i < manager.getNumFormats(); ++i)
            {
                if (i > 0)
                    stream << ", ";

                auto* format = manager.getFormat (i);
                stream << (format != nullptr ? FromJuceString (format->getName()) : std::string { "<null>" });
            }
            stream << "]";
            return stream.str();
        }

        std::string DescribePluginDescriptionForLog (const juce::PluginDescription& description)
        {
            std::ostringstream stream;
            stream << "name='" << FromJuceString (description.name)
                   << "', format='" << FromJuceString (description.pluginFormatName)
                   << "', manufacturer='" << FromJuceString (description.manufacturerName)
                   << "', category='" << FromJuceString (description.category)
                   << "', fileOrIdentifier='" << FromJuceString (description.fileOrIdentifier)
                   << "', identifier='" << FromJuceString (description.createIdentifierString())
                   << "'";
            return stream.str();
        }

        std::string DescribeBundlePayloadForLog (const std::filesystem::path& path)
        {
#if JUCE_MAC
            std::error_code ec;
            if (!std::filesystem::is_directory (path, ec))
                return "path is not a directory bundle";

            const auto macOsDir = path / "Contents" / "MacOS";
            if (!std::filesystem::exists (macOsDir, ec))
                return "missing Contents/MacOS";

            if (!std::filesystem::is_directory (macOsDir, ec))
                return "Contents/MacOS exists but is not a directory";

            std::size_t executableCount = 0;
            std::vector<std::string> sampleNames;
            for (std::filesystem::directory_iterator it (macOsDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
                 it != end && !ec; it.increment (ec))
            {
                const auto candidate = it->path();
                if (std::filesystem::is_regular_file (candidate, ec))
                {
                    ++executableCount;
                    if (sampleNames.size() < 4)
                        sampleNames.push_back (candidate.filename().string());
                }
            }

            std::ostringstream stream;
            stream << "Contents/MacOS regularFiles=" << executableCount;
            if (!sampleNames.empty())
            {
                stream << " [";
                for (std::size_t i = 0; i < sampleNames.size(); ++i)
                {
                    if (i > 0)
                        stream << ", ";
                    stream << sampleNames[i];
                }
                stream << "]";
            }
            return stream.str();
#else
            juce::ignoreUnused (path);
            return {};
#endif
        }

        void AppendHostedPluginTrace (const std::string& message)
        {
            FileSystem fileSystem;
            const auto logPath = fileSystem.ResolveSettingsDirectory() / kHostedPluginTraceLogFileName;
            [[maybe_unused]] const auto ensuredLogDir = fileSystem.EnsureDirectory (logPath.parent_path());

            std::ofstream output (logPath, std::ios::app);
            if (output)
                output << "[HostedPluginEffect] " << message << "\n";

            std::cerr << "[JuceHostedPluginEffect] " << message << std::endl;
        }

        void HashBytes (std::uint64_t& hash, const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*> (data);
            for (std::size_t index = 0; index < size; ++index)
            {
                hash ^= static_cast<std::uint64_t> (bytes[index]);
                hash *= kFNVPrime;
            }
        }

        std::string HashStringForLog (std::string_view value)
        {
            std::uint64_t hash = kFNVOffsetBasis;
            HashBytes (hash, value.data(), value.size());

            std::ostringstream stream;
            stream << "0x" << std::hex << std::setw (16) << std::setfill ('0') << hash;
            return stream.str();
        }

        std::string GetHostedParameterId (const juce::AudioProcessorParameter& parameter)
        {
            if (const auto* hostedParameter = dynamic_cast<const juce::HostedAudioProcessorParameter*> (&parameter))
                return FromJuceString (hostedParameter->getParameterID());

            return {};
        }

        bool DecodeBase64State (const std::string& value, juce::MemoryBlock& state)
        {
            juce::MemoryOutputStream standardDecoded;
            if (juce::Base64::convertFromBase64 (standardDecoded, juce::String (value)))
            {
                state = standardDecoded.getMemoryBlock();
                return true;
            }

            return state.fromBase64Encoding (juce::String (value));
        }

        bool HasHostedPluginStateData (const HostedPluginStateSnapshot& snapshot)
        {
            return !snapshot.rawPluginState.isEmpty()
                   || snapshot.currentProgram >= 0
                   || !snapshot.parameters.empty();
        }

        HostedPluginStateSnapshot CaptureHostedPluginStateSnapshot (juce::AudioPluginInstance& plugin)
        {
            HostedPluginStateSnapshot snapshot;
            plugin.getStateInformation (snapshot.rawPluginState);

            if (plugin.getNumPrograms() > 1)
                snapshot.currentProgram = plugin.getCurrentProgram();

            const auto& parameters = plugin.getParameters();
            snapshot.parameters.reserve (parameters.size());
            for (auto* parameter : parameters)
            {
                if (parameter == nullptr || !parameter->isAutomatable())
                    continue;

                snapshot.parameters.push_back (HostedParameterState {
                    parameter->getParameterIndex(),
                    GetHostedParameterId (*parameter),
                    parameter->getValue(),
                });
            }

            return snapshot;
        }

        std::string EncodeHostedPluginStateBase64 (const HostedPluginStateSnapshot& snapshot)
        {
            if (!HasHostedPluginStateData (snapshot))
                return {};

            juce::MemoryOutputStream encoded;
            encoded.write (kHostedPluginStateEnvelopeMagic, sizeof (kHostedPluginStateEnvelopeMagic));
            encoded.writeInt (kHostedPluginStateEnvelopeVersion);
            encoded.writeInt (snapshot.currentProgram);
            encoded.writeInt (static_cast<int> (snapshot.parameters.size()));
            encoded.writeInt (static_cast<int> (snapshot.rawPluginState.getSize()));

            if (!snapshot.rawPluginState.isEmpty())
                encoded.write (snapshot.rawPluginState.getData(), snapshot.rawPluginState.getSize());

            for (const auto& parameter : snapshot.parameters)
            {
                encoded.writeInt (parameter.index);
                encoded.writeString (juce::String (parameter.parameterId));
                encoded.write (&parameter.value, sizeof (parameter.value));
            }

            return FromJuceString (juce::Base64::toBase64 (encoded.getData(), encoded.getDataSize()));
        }

        HostedPluginStateEnvelopeDecodeResult DecodeHostedPluginStateEnvelope (const juce::MemoryBlock& state,
            HostedPluginStateSnapshot& snapshot)
        {
            snapshot = {};

            if (state.getSize() < sizeof (kHostedPluginStateEnvelopeMagic))
                return HostedPluginStateEnvelopeDecodeResult::notEnvelope;

            if (std::memcmp (state.getData(), kHostedPluginStateEnvelopeMagic, sizeof (kHostedPluginStateEnvelopeMagic)) != 0)
                return HostedPluginStateEnvelopeDecodeResult::notEnvelope;

            juce::MemoryInputStream stream (state, false);
            char magic[sizeof (kHostedPluginStateEnvelopeMagic)] {};
            if (stream.read (magic, sizeof (magic)) != static_cast<int> (sizeof (magic))
                || std::memcmp (magic, kHostedPluginStateEnvelopeMagic, sizeof (magic)) != 0)
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

            snapshot.rawPluginState.setSize (static_cast<size_t> (rawStateSize));
            if (rawStateSize > 0
                && stream.read (snapshot.rawPluginState.getData(), rawStateSize) != rawStateSize)
            {
                return HostedPluginStateEnvelopeDecodeResult::invalid;
            }

            snapshot.parameters.reserve (static_cast<size_t> (parameterCount));
            for (int index = 0; index < parameterCount; ++index)
            {
                HostedParameterState parameter;
                parameter.index = stream.readInt();
                parameter.parameterId = FromJuceString (stream.readString());

                float value = 0.0f;
                if (stream.read (&value, sizeof (value)) != static_cast<int> (sizeof (value)))
                    return HostedPluginStateEnvelopeDecodeResult::invalid;

                if (!std::isfinite (value))
                    return HostedPluginStateEnvelopeDecodeResult::invalid;

                parameter.value = std::clamp (value, 0.0f, 1.0f);
                snapshot.parameters.push_back (std::move (parameter));
            }

            return HostedPluginStateEnvelopeDecodeResult::success;
        }

        bool DecodeHostedPluginStateBase64 (const std::string& value, HostedPluginStateSnapshot& snapshot)
        {
            juce::MemoryBlock decoded;
            if (!DecodeBase64State (value, decoded))
                return false;

            const auto result = DecodeHostedPluginStateEnvelope (decoded, snapshot);
            if (result == HostedPluginStateEnvelopeDecodeResult::invalid)
                return false;

            if (result == HostedPluginStateEnvelopeDecodeResult::notEnvelope)
            {
                snapshot = {};
                snapshot.rawPluginState = decoded;
            }

            return true;
        }

        juce::AudioProcessorParameter* FindHostedPluginParameter (juce::AudioPluginInstance& plugin,
            const HostedParameterState& savedParameter)
        {
            const auto& parameters = plugin.getParameters();

            if (!savedParameter.parameterId.empty())
            {
                for (auto* parameter : parameters)
                {
                    if (parameter == nullptr || !parameter->isAutomatable())
                        continue;

                    if (GetHostedParameterId (*parameter) == savedParameter.parameterId)
                        return parameter;
                }
            }

            if (savedParameter.index >= 0 && savedParameter.index < static_cast<int> (parameters.size()))
            {
                auto* parameter = parameters[static_cast<size_t> (savedParameter.index)];
                if (parameter != nullptr && parameter->isAutomatable())
                    return parameter;
            }

            return nullptr;
        }

        std::string ApplyHostedPluginStateSnapshot (juce::AudioPluginInstance& plugin,
            const HostedPluginStateSnapshot& snapshot)
        {
            if (!snapshot.rawPluginState.isEmpty())
            {
                plugin.setStateInformation (snapshot.rawPluginState.getData(),
                    static_cast<int> (snapshot.rawPluginState.getSize()));
            }

            if (snapshot.currentProgram >= 0 && plugin.getNumPrograms() > 1)
            {
                const int programCount = plugin.getNumPrograms();
                if (snapshot.currentProgram < programCount)
                    plugin.setCurrentProgram (snapshot.currentProgram);
            }

            for (const auto& savedParameter : snapshot.parameters)
            {
                auto* parameter = FindHostedPluginParameter (plugin, savedParameter);
                if (parameter == nullptr)
                    continue;

                if (std::abs (parameter->getValue() - savedParameter.value) <= 1.0e-6f)
                    continue;

                parameter->setValueNotifyingHost (savedParameter.value);
            }

            return SummarizePluginSnapshot (plugin);
        }

        std::string SummarizePluginSnapshot (juce::AudioPluginInstance& plugin)
        {
            std::uint64_t hash = kFNVOffsetBasis;
            std::size_t parameterCount = 0;
            std::ostringstream preview;
            preview << std::fixed << std::setprecision (6);

            const auto& parameters = plugin.getParameters();
            for (auto* parameter : parameters)
            {
                if (parameter == nullptr)
                    continue;

                const float value = parameter->getValue();
                std::uint32_t bits = 0;
                static_assert (sizeof (bits) == sizeof (value));
                std::memcpy (&bits, &value, sizeof (bits));
                HashBytes (hash, &bits, sizeof (bits));

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
            HashBytes (hash, &currentProgram, sizeof (currentProgram));

            std::ostringstream summary;
            summary << "program=";
            if (currentProgram >= 0)
                summary << currentProgram;
            else
                summary << "<none>";
            summary << ", paramCount=" << parameterCount
                    << ", paramHash=0x" << std::hex << std::setw (16) << std::setfill ('0') << hash << std::dec
                    << ", preview=[" << preview.str() << ']';
            return summary.str();
        }

        class HostedPluginEditorWindow final : public juce::DocumentWindow
        {
        public:
            HostedPluginEditorWindow (const juce::String& title,
                juce::AudioProcessorEditor* editor,
                std::function<void()> onClose)
                : DocumentWindow (title,
                      juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      juce::DocumentWindow::closeButton),
                  mOnClose (std::move (onClose))
            {
                setUsingNativeTitleBar (true);
                setContentOwned (editor, true);
                centreWithSize (std::max (360, getWidth()), std::max (220, getHeight()));
                setResizable (editor != nullptr && editor->isResizable(), true);
                setVisible (true);
                toFront (true);
            }

            void closeButtonPressed() override
            {
                setVisible (false);
                if (mOnClose)
                    mOnClose();
            }

        private:
            std::function<void()> mOnClose;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedPluginEditorWindow)
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

        juce::addDefaultFormatsToManager (mFormatManager);
        mFormatsAdded = true;
        AppendHostedPluginTrace ("EnsureFormatsAdded registeredFormats=" + DescribeRegisteredFormats (mFormatManager));
    }

    void JuceHostedPluginEffect::Prepare (double sampleRate, int maxBlockSize)
    {
        if (!ValidatePrepare (sampleRate, maxBlockSize))
            return;

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mPrepared = true;
        UpdateWorkBufferForPlugin();
        AppendHostedPluginTrace ("Prepare sampleRate=" + std::to_string (sampleRate)
                                 + ", blockSize=" + std::to_string (maxBlockSize)
                                 + ", pluginLoaded=" + std::string { mPlugin ? "true" : "false" }
                                 + ", pendingStateLength=" + std::to_string (mPluginStateBase64.size()));
        PrepareLoadedPlugin();
    }

    void JuceHostedPluginEffect::Reset()
    {
        mMidiBuffer.clear();
        if (mPlugin)
        {
            const juce::SpinLock::ScopedTryLockType lock (mPluginProcessLock);
            if (!lock.isLocked())
                return;

            AppendHostedPluginTrace ("Reset plugin=" + FromJuceString (mPlugin->getName()));
            mPlugin->reset();
        }
    }

    void JuceHostedPluginEffect::Process (float** inputs, float** outputs, int numSamples)
    {
        if (!inputs || !outputs || numSamples <= 0)
            return;

        if (!mPlugin || !mPrepared || numSamples > mWorkBuffer.getNumSamples())
        {
            Passthrough (inputs, outputs, numSamples);
            return;
        }

        // The work buffer must cover every channel of the plugin's bus layout.
        // Hosts (e.g. JUCE's LV2 wrapper) connect any port beyond the buffer's
        // channel count to nullptr, which multi-bus plugins will read/write.
        if (mWorkBuffer.getNumChannels() < std::max (mPlugin->getTotalNumInputChannels(),
                                                     mPlugin->getTotalNumOutputChannels()))
        {
            Passthrough (inputs, outputs, numSamples);
            return;
        }

        // Never block the audio thread: when the message thread is mutating the
        // hosted plugin (editor open/close, prepare, state restore, swap), pass
        // the signal through instead of racing the plugin's process call.
        const juce::SpinLock::ScopedTryLockType processLock (mPluginProcessLock);
        if (!processLock.isLocked())
        {
            Passthrough (inputs, outputs, numSamples);
            return;
        }

        CopyInputToWorkBuffer (inputs, numSamples);
        mMidiBuffer.clear();

        // Expose only numSamples frames to the plugin. mWorkBuffer is sized to the
        // maximum block declared at prepareToPlay; when the host delivers a smaller
        // real-time block (e.g. an ASIO buffer below 256) the tail beyond numSamples
        // still holds the previous block's samples. Passing the whole buffer makes
        // the plugin process that stale tail, corrupting its internal state.
        juce::AudioBuffer<float> blockView (mWorkBuffer.getArrayOfWritePointers(),
                                            mWorkBuffer.getNumChannels(),
                                            numSamples);
        mPlugin->processBlock (blockView, mMidiBuffer);

        CopyWorkBufferToOutputs (inputs, outputs, numSamples);
    }

    void JuceHostedPluginEffect::SetParam (const std::string& key, double value)
    {
        if (key == "mix")
            mMix = Clamp (value, 0.0, 1.0);
        else if (key == "inputGain")
            mInputGainDb = Clamp (value, -24.0, 24.0);
        else if (key == "outputGain")
            mOutputGainDb = Clamp (value, -24.0, 24.0);
    }

    double JuceHostedPluginEffect::GetParam (const std::string& key) const
    {
        if (key == "mix")
            return mMix;
        if (key == "inputGain")
            return mInputGainDb;
        if (key == "outputGain")
            return mOutputGainDb;
        return 0.0;
    }

    void JuceHostedPluginEffect::SetConfig (const std::string& key, const std::string& value)
    {
        if (key == "pluginPath")
        {
            AppendHostedPluginTrace ("SetConfig pluginPath=" + value);
            if (!value.empty())
                LoadPluginFromPath (std::filesystem::path (value));
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
            AppendHostedPluginTrace ("SetConfig pluginStateBase64 length=" + std::to_string (value.size())
                                     + ", prepared=" + std::string { mPrepared ? "true" : "false" }
                                     + ", pluginLoaded=" + std::string { mPlugin ? "true" : "false" });
            ApplyPendingPluginState();
            return;
        }

        if (key == "showPluginEditor" || key == "openPluginEditor")
        {
            if (value == "0" || value == "false")
                ClosePluginEditor();
            else
                OpenPluginEditor();
            return;
        }
    }

    void JuceHostedPluginEffect::SetRuntimeConfigChangedCallback (RuntimeConfigChangedCallback callback)
    {
        mRuntimeConfigChangedCallback = std::move (callback);
    }

    std::string JuceHostedPluginEffect::GetConfig (const std::string& key) const
    {
        if (key == "pluginPath")
            return ToDisplayPath (mPluginPath);
        if (key == "pluginFormat")
            return mPluginFormat;
        if (key == "pluginIdentifier")
            return mPluginIdentifier;
        if (key == "pluginName")
            return FromJuceString (mPluginDescription.name);
        if (key == kPluginStateBase64ConfigKey)
            return CapturePluginStateBase64();
        if (key == "lastError")
            return mLastError;
        if (key == kPluginLastErrorCodeConfigKey || key == "lastErrorCode")
            return mLastErrorCode;
        return {};
    }

    bool JuceHostedPluginEffect::LoadResource (const std::filesystem::path& path)
    {
        AppendHostedPluginTrace ("LoadResource path=" + ToDisplayPath (path));
        return LoadPluginFromPath (path);
    }

    bool JuceHostedPluginEffect::LoadResources (const std::vector<ResourceRef>& refs,
        const std::vector<std::filesystem::path>& paths)
    {
        for (std::size_t i = 0; i < refs.size(); ++i)
        {
            const auto& ref = refs[i];
            if (ref.resourceType != "plugin" && ref.resourceType != "audio-plugin" && ref.resourceType != "midi-plugin")
                continue;

            if (i < paths.size())
                return LoadPluginFromPath (paths[i]);
        }

        if (!paths.empty())
            return LoadPluginFromPath (paths.front());

        SetError ("No plugin file was provided. Use Browse to select a plugin. Supported formats: "
                  + GetSupportedPluginFormatsDescription() + ".",
                  "resource-missing");
        return false;
    }

    int JuceHostedPluginEffect::GetLatencySamples() const
    {
        return mPlugin ? mPlugin->getLatencySamples() : 0;
    }

    bool JuceHostedPluginEffect::LoadPluginFromPath (const std::filesystem::path& path)
    {
        // JUCE plugin scanning and instantiation must run on the message thread
        // (AU and VST3 enforce this on macOS). Hop across when a message loop exists.
        if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
            messageManager != nullptr && !messageManager->isThisTheMessageThread())
        {
            if (const auto loaded = juce::MessageManager::callSync ([this, &path] { return LoadPluginFromPath (path); }))
                return *loaded;

            SetError ("Plugin loading could not be scheduled on the UI thread. Please try again.",
                      "thread-scheduling");
            return false;
        }

        EnsureFormatsAdded();

        // Heal stored paths that point inside a plugin bundle (e.g. the inner .so
        // of a Linux VST3, or the .dll / manifest.ttl of an LV2 bundle picked
        // through the Windows files-only dialog). Applied on every load, not just
        // on fresh picks, so paths arriving from presets, synced libraries or a
        // hand-edited entry are normalized identically.
        const std::filesystem::path resolvedPath = pluginpath::ResolvePluginBundlePath (path);

        AppendHostedPluginTrace ("LoadPluginFromPath begin path=" + ToDisplayPath (resolvedPath)
                                 + ", pendingStateLength=" + std::to_string (mPluginStateBase64.size())
                                 + ", sampleRate=" + std::to_string (mSampleRate)
                                 + ", blockSize=" + std::to_string (mMaxBlockSize));

        const juce::File pluginFile (ToJucePath (resolvedPath));
        if (!pluginFile.exists())
        {
            SetError ("Plugin file was not found: " + ToDisplayPath (resolvedPath)
                      + ". The plugin may have been moved or uninstalled.",
                      "file-not-found");
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        // A plain folder cannot be scanned at all, so reject it up front rather than
        // letting the scanner fail with a much vaguer message. Every other kind of
        // bad selection reaches the scanner first and is reported below.
        if (pluginFile.isDirectory() && !pluginpath::HasPluginBundleSuffix (resolvedPath))
        {
            SetError (DescribeUnsupportedPluginFile (resolvedPath), "not-a-plugin-target");
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        juce::OwnedArray<juce::PluginDescription> descriptions;
        const auto fileOrIdentifier = pluginFile.getFullPathName();
        const juce::String formatHint = NormalizePluginFormatHint (mPluginFormat);
        std::ostringstream scanLog;

        const auto scanWithFormats = [this, &descriptions, &fileOrIdentifier, &scanLog] (const juce::String& restrictToFormat)
        {
            scanLog << "scan pass restrictTo='" << FromJuceString (restrictToFormat) << "'\n";
            for (int i = 0; i < mFormatManager.getNumFormats(); ++i)
            {
                auto* format = mFormatManager.getFormat (i);
                if (!format)
                {
                    scanLog << "  format[" << i << "] <null>\n";
                    continue;
                }

                const std::string formatName = FromJuceString (format->getName());

                if (restrictToFormat.isNotEmpty() && !format->getName().equalsIgnoreCase (restrictToFormat))
                {
                    scanLog << "  format='" << formatName << "' skippedByRestrict=true\n";
                    continue;
                }

                const bool fileMightContain = format->fileMightContainThisPluginType (fileOrIdentifier);
                const bool attempted = fileMightContain || restrictToFormat.isNotEmpty();
                const int before = descriptions.size();
                if (attempted)
                    format->findAllTypesForFile (descriptions, fileOrIdentifier);

                const int added = descriptions.size() - before;
                scanLog << "  format='" << formatName
                        << "', fileMightContain=" << (fileMightContain ? "true" : "false")
                        << ", attempted=" << (attempted ? "true" : "false")
                        << ", addedDescriptions=" << added << "\n";
            }
        };

        scanWithFormats (formatHint);

        // A stale or mismatched format hint must not block loading; rescan with every format.
        if (descriptions.isEmpty() && formatHint.isNotEmpty())
            scanWithFormats ({});

        if (descriptions.isEmpty())
        {
            std::string message = DescribeUnsupportedPluginFile (resolvedPath);
            if (message.empty())
                message = "No loadable plugin was found at: " + ToDisplayPath (resolvedPath)
                          + ". Check that it is a 64-bit plugin built for this platform. Supported formats: "
                          + GetSupportedPluginFormatsDescription() + ".";

            AppendHostedPluginTrace ("LoadPluginFromPath scan failed path=" + ToDisplayPath (resolvedPath)
                                     + ", formatHint='" + FromJuceString (formatHint) + "', registeredFormats="
                                     + DescribeRegisteredFormats (mFormatManager)
                                     + ", bundlePayload=" + DescribeBundlePayloadForLog (resolvedPath)
                                     + "\n" + scanLog.str());
            SetError (message, "scan-no-descriptions");
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        for (int i = 0; i < descriptions.size(); ++i)
        {
            auto* description = descriptions[i];
            if (!description)
                continue;

            AppendHostedPluginTrace ("LoadPluginFromPath candidate[" + std::to_string (i) + "] "
                                     + DescribePluginDescriptionForLog (*description));
        }

        juce::PluginDescription* selected = descriptions.getFirst();
        if (!mPluginIdentifier.empty())
        {
            for (auto* description : descriptions)
            {
                if (description && (description->createIdentifierString() == juce::String (mPluginIdentifier) || description->fileOrIdentifier == juce::String (mPluginIdentifier)))
                {
                    selected = description;
                    break;
                }
            }
        }

        if (!selected)
        {
            SetError ("Plugin scan returned no selectable plugin descriptions", "scan-no-selection");
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        if (IsBlockedSelfHostedPluginCandidate (*selected, resolvedPath))
        {
            SetError ("Soundshed Guitar cannot be loaded inside the hosted plugin slot."
                      " Please choose a different plugin.",
                      "plugin-blocked-self");
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        juce::String error;
        // Loading is already on the JUCE message thread. Keep creation synchronous:
        // createPluginInstanceAsync requires a nested dispatch loop here, which caused
        // valid AU/VST3 initializations to hit an artificial timeout.
        auto instance = mFormatManager.createPluginInstance (*selected, mSampleRate, mMaxBlockSize, error);
        if (!instance)
        {
            const std::string pluginName = FromJuceString (selected->name.isNotEmpty()
                                                               ? selected->name
                                                               : pluginFile.getFileNameWithoutExtension());
            std::string message = "Failed to open plugin '" + pluginName + "'";
            std::string errorCode = "instantiate-failed";
            if (error.isNotEmpty())
                message += ": " + FromJuceString (error);
            else
                message += ". The plugin may be incompatible with this host or built for a different architecture.";

            AppendHostedPluginTrace ("LoadPluginFromPath instantiate failed selected="
                                     + DescribePluginDescriptionForLog (*selected)
                                     + ", sampleRate=" + std::to_string (mSampleRate)
                                     + ", blockSize=" + std::to_string (mMaxBlockSize)
                                     + ", juceError='" + FromJuceString (error) + "'");
            SetError (message, errorCode);
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        if (!ConfigurePluginBuses (*instance))
        {
            AppendHostedPluginTrace ("LoadPluginFromPath bus layout unsupported for plugin='"
                                     + FromJuceString (instance->getName())
                                     + "' inputBusCount=" + std::to_string (instance->getBusCount (true))
                                     + ", outputBusCount=" + std::to_string (instance->getBusCount (false)));
            SetError ("Plugin '" + FromJuceString (instance->getName())
                      + "' does not support a mono or stereo layout, so it cannot be used in the signal chain.",
                      "bus-layout-unsupported");
            instance->releaseResources();
            ReleaseHostedPlugin();
            ClearLoadedPluginMetadata();
            return false;
        }

        mPluginDescription = *selected;
        mPluginPath = resolvedPath;
        mPluginFormat = FromJuceString (selected->pluginFormatName);
        mPluginIdentifier = FromJuceString (selected->createIdentifierString());
        ClosePluginEditor();
        ReleaseHostedPlugin();
        {
            const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);
            mPlugin = std::move (instance);
        }
        AttachHostedPluginListeners();
        mLastError.clear();
        mLastErrorCode.clear();

        if (mRuntimeConfigChangedCallback)
        {
            const std::string pluginName = FromJuceString (mPluginDescription.name);
            const std::string pluginManufacturer = FromJuceString (mPluginDescription.manufacturerName);
            const std::string pluginStableId = BuildPluginStableId (mPluginDescription, mPluginPath);

            mRuntimeConfigChangedCallback (kPluginFormatConfigKey, mPluginFormat);
            mRuntimeConfigChangedCallback (kPluginIdentifierConfigKey, mPluginIdentifier);
            if (!pluginName.empty())
                mRuntimeConfigChangedCallback (kPluginNameConfigKey, pluginName);
            if (!pluginManufacturer.empty())
                mRuntimeConfigChangedCallback (kPluginManufacturerConfigKey, pluginManufacturer);
            if (!pluginStableId.empty())
                mRuntimeConfigChangedCallback (kPluginStableIdConfigKey, pluginStableId);
        }

        AppendHostedPluginTrace ("LoadPluginFromPath instantiated name=" + FromJuceString (mPluginDescription.name)
                                 + ", format=" + mPluginFormat + ", identifier=" + mPluginIdentifier);
        if (!mPluginStateBase64.empty())
        {
            AppendHostedPluginTrace ("LoadPluginFromPath applying pending state length=" + std::to_string (mPluginStateBase64.size()));
            ApplyPluginStateBase64 (mPluginStateBase64);
        }
        PrepareLoadedPlugin();
        return true;
    }

    bool JuceHostedPluginEffect::ConfigurePluginBuses (juce::AudioPluginInstance& plugin) const
    {
        const juce::AudioChannelSet stereo = juce::AudioChannelSet::stereo();
        const juce::AudioChannelSet mono = juce::AudioChannelSet::mono();

        const bool hasMainInput = plugin.getBusCount (true) > 0;
        const bool hasMainOutput = plugin.getBusCount (false) > 0;
        if (!hasMainOutput)
            return false;

        const juce::AudioProcessor::BusesLayout stereoLayout {
            hasMainInput ? juce::Array<juce::AudioChannelSet> { stereo } : juce::Array<juce::AudioChannelSet> {},
            juce::Array<juce::AudioChannelSet> { stereo }
        };
        if (plugin.checkBusesLayoutSupported (stereoLayout) && plugin.setBusesLayout (stereoLayout))
            return true;

        const juce::AudioProcessor::BusesLayout monoLayout {
            hasMainInput ? juce::Array<juce::AudioChannelSet> { mono } : juce::Array<juce::AudioChannelSet> {},
            juce::Array<juce::AudioChannelSet> { mono }
        };
        if (plugin.checkBusesLayoutSupported (monoLayout) && plugin.setBusesLayout (monoLayout))
            return true;

        // Multi-bus plugins: enable ALL buses at their declared default channel
        // sets. Leaving optional buses disabled is unsafe for LV2 plugins whose
        // ports are merely "connectionOptional" but whose DSP does not actually
        // tolerate nullptr connections (e.g. KV Element FX with 17 stereo
        // groups): JUCE connects ports of disabled buses to nullptr.
        {
            juce::AudioProcessor::BusesLayout fullLayout;
            bool valid = true;
            for (int i = 0; i < plugin.getBusCount (true) && valid; ++i)
            {
                if (auto* bus = plugin.getBus (true, i))
                    fullLayout.inputBuses.add (bus->getDefaultLayout());
                else
                    valid = false;
            }
            for (int i = 0; i < plugin.getBusCount (false) && valid; ++i)
            {
                if (auto* bus = plugin.getBus (false, i))
                    fullLayout.outputBuses.add (bus->getDefaultLayout());
                else
                    valid = false;
            }

            if (valid && fullLayout.outputBuses.size() > 0)
            {
                // Prefer a stereo main bus alongside the fully enabled aux buses.
                auto mainStereo = fullLayout;
                if (mainStereo.inputBuses.size() > 0)
                    mainStereo.inputBuses.getReference (0) = stereo;
                mainStereo.outputBuses.getReference (0) = stereo;
                if (plugin.checkBusesLayoutSupported (mainStereo) && plugin.setBusesLayout (mainStereo))
                    return true;

                if (plugin.checkBusesLayoutSupported (fullLayout) && plugin.setBusesLayout (fullLayout))
                    return true;
            }
        }

        // Fall back to the plugin's existing layout untouched, as long as it has
        // a usable main output. The work buffer is sized to the full channel
        // count, so extra buses are fed silence and ignored on output.
        const auto current = plugin.getBusesLayout();
        return current.outputBuses.size() > 0
               && current.outputBuses.getReference (0).size() > 0;
    }

    void JuceHostedPluginEffect::PrepareLoadedPlugin()
    {
        if (!mPlugin || !mPrepared)
            return;

        AppendHostedPluginTrace ("PrepareLoadedPlugin plugin=" + FromJuceString (mPlugin->getName())
                                 + ", sampleRate=" + std::to_string (mSampleRate)
                                 + ", blockSize=" + std::to_string (mMaxBlockSize)
                                 + ", pendingStateLength=" + std::to_string (mPluginStateBase64.size()));
        {
            // JUCE's LV2 host destroys and recreates the plugin view/instance
            // internals inside prepareToPlay; keep the audio thread out.
            const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);
            mPlugin->setRateAndBufferSizeDetails (mSampleRate, mMaxBlockSize);
            mPlugin->prepareToPlay (mSampleRate, mMaxBlockSize);
            // A host must resume the plugins it prepares. Without this, a plugin
            // that gates its processing on isSuspended() loads without error but
            // silently passes audio through unprocessed.
            mPlugin->suspendProcessing (false);
            UpdateWorkBufferForPlugin();
        }
        ApplyPendingPluginState();
    }

    void JuceHostedPluginEffect::UpdateWorkBufferForPlugin()
    {
        if (mMaxBlockSize <= 0)
            return;

        // Allocate one channel per plugin channel (like AudioProcessorGraph does
        // for its nodes). Multi-bus plugins (e.g. KV Element with 17 stereo
        // groups) crash if ports beyond the buffer channel count are left
        // connected to nullptr.
        const int pluginChannels = mPlugin
            ? std::max (mPlugin->getTotalNumInputChannels(), mPlugin->getTotalNumOutputChannels())
            : 0;
        const int channels = std::max (2, pluginChannels);
        if (mWorkBuffer.getNumChannels() != channels || mWorkBuffer.getNumSamples() < mMaxBlockSize)
            mWorkBuffer.setSize (channels, mMaxBlockSize, false, true, true);
    }

    void JuceHostedPluginEffect::CopyInputToWorkBuffer (float** inputs, int numSamples)
    {
        const float inputGain = DbToLinear (mInputGainDb);
        const int totalChannels = mWorkBuffer.getNumChannels();
        for (int ch = 0; ch < totalChannels; ++ch)
        {
            auto* dest = mWorkBuffer.getWritePointer (ch);
            const float* source = ch < 2 ? inputs[ch] : nullptr;
            if (source)
            {
                for (int i = 0; i < numSamples; ++i)
                    dest[i] = source[i] * inputGain;
            }
            else
            {
                std::fill (dest, dest + numSamples, 0.0f);
            }
        }
    }

    void JuceHostedPluginEffect::CopyWorkBufferToOutputs (float** inputs, float** outputs, int numSamples)
    {
        const float outputGain = DbToLinear (mOutputGainDb);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (!outputs[ch])
                continue;

            const float* dry = inputs[ch];
            const float* wet = mWorkBuffer.getReadPointer (std::min (ch, mWorkBuffer.getNumChannels() - 1));
            for (int i = 0; i < numSamples; ++i)
            {
                const float drySample = dry ? dry[i] : 0.0f;
                outputs[ch][i] = static_cast<float> ((drySample * (1.0 - mMix)) + (wet[i] * outputGain * mMix));
            }
        }
    }

    void JuceHostedPluginEffect::Passthrough (float** inputs, float** outputs, int numSamples) const
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            if (!outputs[ch])
                continue;

            const float* source = inputs[ch];
            if (source)
                std::copy (source, source + numSamples, outputs[ch]);
            else
                std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);
        }
    }

    void JuceHostedPluginEffect::ApplyPluginStateBase64 (const std::string& value)
    {
        if (!mPlugin || value.empty())
            return;

        AppendHostedPluginTrace ("ApplyPluginStateBase64 begin plugin=" + FromJuceString (mPlugin->getName())
                                 + ", encodedLength=" + std::to_string (value.size())
                                 + ", encodedHash=" + HashStringForLog (value)
                                 + ", preApply=" + SummarizePluginSnapshot (*mPlugin));

        HostedPluginStateSnapshot snapshot;
        if (!DecodeHostedPluginStateBase64 (value, snapshot))
        {
            SetError ("Invalid hosted plugin state encoding");
            return;
        }

        AppendHostedPluginTrace ("ApplyPluginStateBase64 decodedBytes=" + std::to_string (snapshot.rawPluginState.getSize())
                                 + ", currentProgram=" + std::to_string (snapshot.currentProgram)
                                 + ", parameterCount=" + std::to_string (snapshot.parameters.size()));

        const auto applyState = [this, snapshot]() -> std::string {
            if (!mPlugin)
                return "plugin missing";

            ++mAutoCaptureSuppressionDepth;
            const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);
            return ApplyHostedPluginStateSnapshot (*mPlugin, snapshot);
        };

        std::string applySummary;

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            applySummary = applyState();
            --mAutoCaptureSuppressionDepth;
        }
        else if (auto result = juce::MessageManager::callSync (applyState))
        {
            applySummary = *result;
            --mAutoCaptureSuppressionDepth;
        }
        else
        {
            mAutoCaptureSuppressionDepth.store (0, std::memory_order_release);
            SetError ("Failed to restore hosted plugin state on the message thread");
            return;
        }

        AppendHostedPluginTrace ("ApplyPluginStateBase64 complete plugin=" + FromJuceString (mPlugin->getName())
                                 + ", postApply=" + applySummary);
        mLastError.clear();
    }

    void JuceHostedPluginEffect::ApplyPendingPluginState()
    {
        if (mPlugin && mPrepared && !mPluginStateBase64.empty())
        {
            AppendHostedPluginTrace ("ApplyPendingPluginState applying plugin=" + FromJuceString (mPlugin->getName())
                                     + ", stateLength=" + std::to_string (mPluginStateBase64.size()));
            ApplyPluginStateBase64 (mPluginStateBase64);
        }
        else
        {
            AppendHostedPluginTrace ("ApplyPendingPluginState skipped pluginLoaded=" + std::string { mPlugin ? "true" : "false" }
                                     + ", prepared=" + std::string { mPrepared ? "true" : "false" }
                                     + ", stateLength=" + std::to_string (mPluginStateBase64.size()));
        }
    }

    std::string JuceHostedPluginEffect::CapturePluginStateBase64() const
    {
        if (!mPlugin)
            return mPluginStateBase64;

        const auto captureState = [this]() -> std::string {
            if (!mPlugin)
                return mPluginStateBase64;

            const auto snapshot = CaptureHostedPluginStateSnapshot (*mPlugin);
            return EncodeHostedPluginStateBase64 (snapshot);
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            return captureState();
        }

        if (auto captured = juce::MessageManager::callSync (captureState))
        {
            return *captured;
        }

        return {};
    }

    void JuceHostedPluginEffect::AttachHostedPluginListeners()
    {
        if (!mPlugin || mHostedPluginListenerAttached)
            return;

        mPlugin->addListener (this);
        AttachHostedPluginParameterListeners();
        mHostedPluginListenerAttached = true;
    }

    void JuceHostedPluginEffect::AttachHostedPluginParameterListeners()
    {
        DetachHostedPluginParameterListeners();

        if (!mPlugin)
            return;

        const auto& parameters = mPlugin->getParameters();
        mHostedParametersWithListeners.reserve (parameters.size());
        for (auto* parameter : parameters)
        {
            if (parameter == nullptr)
                continue;

            parameter->addListener (this);
            mHostedParametersWithListeners.push_back (parameter);
        }
    }

    void JuceHostedPluginEffect::DetachHostedPluginParameterListeners()
    {
        for (auto* parameter : mHostedParametersWithListeners)
        {
            if (parameter != nullptr)
                parameter->removeListener (this);
        }

        mHostedParametersWithListeners.clear();
    }

    void JuceHostedPluginEffect::ReleaseHostedPlugin()
    {
        ClosePluginEditor();
        cancelPendingUpdate();
        mForceAutoCaptureNotification.store (false, std::memory_order_release);
        mAutoCaptureSuppressionDepth.store (0, std::memory_order_release);

        DetachHostedPluginParameterListeners();

        if (mHostedPluginListenerAttached && mPlugin)
            mPlugin->removeListener (this);

        mHostedPluginListenerAttached = false;

        if (mPlugin)
        {
            const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);
            mPlugin->releaseResources();
            mPlugin.reset();
        }
    }

    void JuceHostedPluginEffect::ClearLoadedPluginMetadata()
    {
        mPluginDescription = {};
        mPluginPath.clear();
        mPluginFormat.clear();
        mPluginIdentifier.clear();
    }

    void JuceHostedPluginEffect::ScheduleAutoCapture (bool forceNotify)
    {
        if (!mPlugin)
            return;

        if (forceNotify)
            mForceAutoCaptureNotification.store (true, std::memory_order_release);

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            CaptureAndPublishPluginState (mForceAutoCaptureNotification.exchange (false, std::memory_order_acq_rel));
            return;
        }

        triggerAsyncUpdate();
    }

    void JuceHostedPluginEffect::CaptureAndPublishPluginState (bool forceNotify)
    {
        if (!mPlugin)
            return;

        PublishCapturedPluginState (CapturePluginStateBase64(), forceNotify);
    }

    void JuceHostedPluginEffect::PublishCapturedPluginState (const std::string& capturedState, bool forceNotify)
    {
        const bool changed = capturedState != mPluginStateBase64;
        if (!changed && !forceNotify)
            return;

        mPluginStateBase64 = capturedState;

        if (mRuntimeConfigChangedCallback)
            mRuntimeConfigChangedCallback (kPluginStateBase64ConfigKey, mPluginStateBase64);
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
            SetError ("Cannot open hosted plugin UI before a plugin is loaded");
            return;
        }

        auto open = [this]() {
            try
            {
                if (!mPlugin)
                    return;

                if (sActiveHostedPluginEditorOwner != nullptr && sActiveHostedPluginEditorOwner != this)
                    sActiveHostedPluginEditorOwner->ClosePluginEditor();

                if (mEditorWindow)
                {
                    EnsurePluginStateBaseline();
                    mEditorWindow->setVisible (true);
                    mEditorWindow->toFront (true);
                    sActiveHostedPluginEditorOwner = this;
                    return;
                }

                // Plugin editor/view creation can touch state shared with the audio
                // thread (LV2 view creation in particular); suspend processing
                // (passthrough) for the duration.
                const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);

                auto* editor = mPlugin->hasEditor()
                                   ? mPlugin->createEditorAndMakeActive()
                                   : static_cast<juce::AudioProcessorEditor*> (new juce::GenericAudioProcessorEditor (*mPlugin));
                if (!editor)
                {
                    SetError ("Hosted plugin did not provide an editor");
                    return;
                }

                const auto title = mPluginDescription.name.isNotEmpty()
                                       ? mPluginDescription.name
                                       : mPlugin->getName();
                EnsurePluginStateBaseline();
                mEditorWindow = std::make_unique<HostedPluginEditorWindow> (title, editor, [this]() {
                    if (sActiveHostedPluginEditorOwner == this)
                        sActiveHostedPluginEditorOwner = nullptr;
                    ScheduleAutoCapture (false);
                });
                sActiveHostedPluginEditorOwner = this;
            }
            catch (const std::exception& ex)
            {
                SetError ("Hosted plugin editor creation threw: " + std::string (ex.what()));
                mEditorWindow.reset();
            }
            catch (...)
            {
                SetError ("Hosted plugin editor creation threw an unknown exception");
                mEditorWindow.reset();
            }
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            open();
        else
        {
            if (!juce::MessageManager::callAsync (std::move (open)))
                SetError ("Failed to schedule hosted plugin editor open on the message thread");
        }
    }

    bool JuceHostedPluginEffect::ClosePluginEditor()
    {
        if (!mEditorWindow)
        {
            if (sActiveHostedPluginEditorOwner == this)
                sActiveHostedPluginEditorOwner = nullptr;
            return true;
        }

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);
            mEditorWindow.reset();
            if (sActiveHostedPluginEditorOwner == this)
                sActiveHostedPluginEditorOwner = nullptr;
            return true;
        }

        if (auto closed = juce::MessageManager::callSync ([this]() {
                const juce::SpinLock::ScopedLockType lock (mPluginProcessLock);
                mEditorWindow.reset();
                if (sActiveHostedPluginEditorOwner == this)
                    sActiveHostedPluginEditorOwner = nullptr;
                return true;
            }))
        {
            return *closed;
        }

        SetError ("Failed to close hosted plugin editor on the message thread");
        return false;
    }

    void JuceHostedPluginEffect::SetError (const std::string& message, const std::string& code)
    {
        mLastError = message;
        mLastErrorCode = code;
        AppendHostedPluginTrace ("SetError code=" + code + ": " + message);
    }

    void JuceHostedPluginEffect::parameterValueChanged (int,
        float)
    {
        if (!mPlugin || mAutoCaptureSuppressionDepth.load (std::memory_order_acquire) > 0)
            return;

        ScheduleAutoCapture (false);
    }

    void JuceHostedPluginEffect::parameterGestureChanged (int,
        bool gestureIsStarting)
    {
        if (gestureIsStarting || !mPlugin || mAutoCaptureSuppressionDepth.load (std::memory_order_acquire) > 0)
            return;

        ScheduleAutoCapture (false);
    }

    void JuceHostedPluginEffect::audioProcessorParameterChanged (juce::AudioProcessor* processor,
        int,
        float)
    {
        if (processor != mPlugin.get() || mAutoCaptureSuppressionDepth.load (std::memory_order_acquire) > 0)
            return;

        ScheduleAutoCapture (false);
    }

    void JuceHostedPluginEffect::audioProcessorChanged (juce::AudioProcessor* processor,
        const juce::AudioProcessorListener::ChangeDetails& details)
    {
        if (processor != mPlugin.get() || mAutoCaptureSuppressionDepth.load (std::memory_order_acquire) > 0)
            return;

        if (details.parameterInfoChanged)
            AttachHostedPluginParameterListeners();

        if (details.programChanged || details.nonParameterStateChanged || details.parameterInfoChanged)
            ScheduleAutoCapture (details.parameterInfoChanged);
    }

    void JuceHostedPluginEffect::handleAsyncUpdate()
    {
        CaptureAndPublishPluginState (mForceAutoCaptureNotification.exchange (false, std::memory_order_acq_rel));
    }

    void RegisterJuceHostedPluginEffect()
    {
        EffectTypeInfo info;
        info.type = EffectGuids::kPluginHost;
        info.aliases = { "plugin_host", "juce_plugin_host" };
        info.displayName = "Plugin Host";
        info.category = "utility";
        info.description = "Host an external JUCE-supported audio plugin inside the signal path";
        info.requiresResource = true;
        info.resourceType = "plugin";
        info.parameters = {
            { "mix", "Mix", 1.0, 0.0, 1.0, "", "", false, 0.01 },
            { "inputGain", "Input", 0.0, -24.0, 24.0, "dB" },
            { "outputGain", "Output", 0.0, -24.0, 24.0, "dB" }
        };
        info.exposedResources = {
            { "plugin", "Plugin", "", "plugin", 0, true }
        };

        EffectRegistry::Instance().Register (info.type, info, []() {
            return std::make_unique<JuceHostedPluginEffect>();
        });
    }

} // namespace guitarfx
