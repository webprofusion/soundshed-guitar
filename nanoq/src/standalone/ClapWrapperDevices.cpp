// ClapWrapperDevices - the device backend (StandaloneDevices.h) over clap-wrapper's standalone host.
//
// Compiled into the standalone executable only (nanoq/CMakeLists.txt adds it to nanoq_standalone
// and keeps it out of nanoq_impl): getStandaloneHost() and RtAudio exist nowhere else, and the
// CLAP, VST3 and AU builds must link without them. It registers itself at static initialisation,
// which a source compiled straight into the executable is guaranteed to run; the shared code finds
// it through RegisteredDeviceBackend() and sees none in a plugin.
//
// Everything here runs on the app's main thread, where clap-wrapper's own settings window drives
// the same host. The setters follow that window's handlers (windows_standalone.cpp): change the
// host's fields, restart the stream, then save the setup to the host's settings file, which the
// standalone reads at the next launch.

#include "StandaloneDevices.h"

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_host.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace nanoq
{
namespace
{
namespace sa = freeaudio::clap_wrapper::standalone;

/// RtAudio's errors during one request, kept for the reply rather than shown in the wrapper's
/// message box. RtAudio raises them on the calling thread while a stream is opened.
class ErrorCapture
{
  public:
    explicit ErrorCapture(sa::StandaloneHost& host) : mHost(host), mPrevious(std::move(host.displayAudioError))
    {
        mHost.displayAudioError = [this](const std::string& text) {
            if (mText.empty())
            {
                mText = text;
            }
        };
    }

    ~ErrorCapture()
    {
        mHost.displayAudioError = std::move(mPrevious);
    }

    ErrorCapture(const ErrorCapture&) = delete;
    ErrorCapture& operator=(const ErrorCapture&) = delete;

    [[nodiscard]] const std::string& Text() const
    {
        return mText;
    }

  private:
    sa::StandaloneHost& mHost;
    std::function<void(const std::string&)> mPrevious;
    std::string mText;
};

bool IsLinkedApi(RtAudio::Api api)
{
    // ASIO opens a driver's inputs and outputs as one device.
    return api == RtAudio::Api::WINDOWS_ASIO;
}

bool StreamRunning(sa::StandaloneHost& host)
{
    return host.rtaDac && host.rtaDac->isStreamRunning();
}

/// How many channels to open each device with, as clap-wrapper's window sets it.
void RefreshChannelCounts(sa::StandaloneHost& host)
{
    for (const auto& device : host.getOutputAudioDevices())
    {
        if (device.ID == host.audioOutputDeviceID)
        {
            host.deviceOutputChannels = device.outputChannels;
        }
    }

    for (const auto& device : host.getInputAudioDevices())
    {
        if (device.ID == host.audioInputDeviceID)
        {
            host.deviceInputChannels = device.inputChannels;
        }
    }
}

void Save(sa::StandaloneHost& host)
{
    host.captureAudioSettings();

    // A run on a throwaway profile (SOUNDSHED_NANOQ_PROFILE: run-isolated.ps1, stress-ui.ps1)
    // leaves the setup file alone: it is clap-wrapper's, outside the profile, and the real
    // session's.
    if (const char* profile = std::getenv("SOUNDSHED_NANOQ_PROFILE"); profile != nullptr && *profile != '\0')
    {
        return;
    }

    host.saveStandaloneSettings();
}

/// Opens the stream on the host's current choice. The setup is saved only once it opens (or when
/// both sides were turned off on purpose), so a failed choice is not what the next launch tries.
std::string Start(sa::StandaloneHost& host, const ErrorCapture& errors)
{
    const bool useInput = host.audioInputUsed && host.numAudioInputs > 0;
    const bool useOutput = host.audioOutputUsed && host.numAudioOutputs > 0;
    host.startAudioThreadOn(host.audioInputDeviceID, host.deviceInputChannels, useInput, host.audioOutputDeviceID,
                            host.deviceOutputChannels, useOutput, host.currentSampleRate);

    if (StreamRunning(host) || (!host.audioInputUsed && !host.audioOutputUsed))
    {
        Save(host);
        return {};
    }

    return errors.Text().empty() ? std::string("Couldn't open the audio device.")
                                 : "Couldn't open the audio device: " + errors.Text();
}

class ClapWrapperBackend final : public DeviceBackend
{
  public:
    DeviceSnapshot Snapshot(bool rescan) override
    {
        DeviceSnapshot s;
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return s;
        }

        sa::StandaloneHost::SilenceAudioErrors silence(host);

        // Enumerating ASIO loads and unloads every installed driver, which some do not survive
        // while one of them is streaming (standalone_host.h); its list is refreshed by choosing
        // the driver again.
        if (rescan && !IsLinkedApi(host->audioApi))
        {
            host->invalidateDeviceList();
        }

        for (const auto api : host->getCompiledApi())
        {
            if (api != RtAudio::Api::RTAUDIO_DUMMY)
            {
                s.apis.push_back(RtAudio::getApiDisplayName(api));
            }
        }

        s.api = host->audioApiDisplayName;
        s.separateIO = !IsLinkedApi(host->audioApi);

        for (const auto& device : host->getInputAudioDevices())
        {
            s.inputs.push_back(device.name);
        }

        for (const auto& device : host->getOutputAudioDevices())
        {
            s.outputs.push_back(device.name);
        }

        if (host->audioInputUsed && host->isKnownDevice(host->audioInputDeviceID))
        {
            s.input = host->deviceName(host->audioInputDeviceID);
            s.inputChannels = static_cast<int>(host->deviceInfoFor(host->audioInputDeviceID).inputChannels);
        }

        if (host->audioOutputUsed && host->isKnownDevice(host->audioOutputDeviceID))
        {
            s.output = host->deviceName(host->audioOutputDeviceID);
            s.outputChannels = static_cast<int>(host->deviceInfoFor(host->audioOutputDeviceID).outputChannels);
        }

        s.open = StreamRunning(*host);

        for (const auto rate : host->getSampleRates())
        {
            s.sampleRates.push_back(static_cast<int>(rate));
        }

        for (const auto size : host->getBufferSizes())
        {
            s.bufferSizes.push_back(static_cast<int>(size));
        }

        s.sampleRate = static_cast<int>(host->currentSampleRate);
        s.bufferSize = static_cast<int>(host->currentBufferSize);

        if (s.open)
        {
            s.latency = static_cast<int>(host->rtaDac->getStreamLatency());
        }

        // Ports are known by name: RtMidi has no stable id, and the name is what the host saves.
        const auto& wanted = host->settings.midiPortNames;

        for (const auto& name : host->getMidiPortNames())
        {
            s.midiInputs.push_back(name);
            s.midiEnabled.push_back(host->settings.midiBindAllPorts ||
                                    std::find(wanted.begin(), wanted.end(), name) != wanted.end());
        }

        return s;
    }

    std::string SetApi(const std::string& displayName) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        ErrorCapture errors(*host);

        auto chosen = RtAudio::Api::UNSPECIFIED;

        for (const auto api : host->getCompiledApi())
        {
            if (RtAudio::getApiDisplayName(api) == displayName || RtAudio::getApiName(api) == displayName)
            {
                chosen = api;
            }
        }

        if (chosen == RtAudio::Api::UNSPECIFIED)
        {
            return "Couldn't switch to " + displayName + ".";
        }

        if (chosen == host->audioApi && host->rtaDac)
        {
            return {}; // the driver in use: keep its devices
        }

        host->setAudioApi(chosen);

        if (host->audioApi != chosen)
        {
            return "Couldn't switch to " + displayName + ".";
        }

        // The new driver's default devices, as clap-wrapper's window picks them.
        auto [input, output, sampleRate] = host->getDefaultAudioInOutSampleRate();
        host->audioInputDeviceID = input;
        host->audioInputUsed = host->isKnownDevice(input);
        host->audioOutputDeviceID = output;
        host->audioOutputUsed = host->isKnownDevice(output);
        host->currentSampleRate = sampleRate;
        host->currentBufferSize = sa::StandaloneSettings::defaultBufferSize;
        RefreshChannelCounts(*host);
        return Start(*host, errors);
    }

    std::string SetDevice(bool input, const std::string& name) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        ErrorCapture errors(*host);

        if (name.empty())
        {
            (input ? host->audioInputUsed : host->audioOutputUsed) = false;
            return Start(*host, errors);
        }

        const auto devices = input ? host->getInputAudioDevices() : host->getOutputAudioDevices();
        const auto it =
            std::find_if(devices.begin(), devices.end(), [&](const RtAudio::DeviceInfo& d) { return d.name == name; });

        if (it == devices.end())
        {
            return "There is no device called " + name + ".";
        }

        if (input)
        {
            host->audioInputDeviceID = it->ID;
            host->deviceInputChannels = it->inputChannels;
            host->audioInputUsed = true;
        }
        else
        {
            host->audioOutputDeviceID = it->ID;
            host->deviceOutputChannels = it->outputChannels;
            host->audioOutputUsed = true;
        }

        return Start(*host, errors);
    }

    std::string SetLinkedDevice(const std::string& name) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        ErrorCapture errors(*host);

        if (name.empty())
        {
            host->audioInputUsed = false;
            host->audioOutputUsed = false;
            return Start(*host, errors);
        }

        const auto& devices = host->deviceList();
        const auto it =
            std::find_if(devices.begin(), devices.end(), [&](const RtAudio::DeviceInfo& d) { return d.name == name; });

        if (it == devices.end())
        {
            return "There is no device called " + name + ".";
        }

        const auto device = *it; // the list may be rebuilt by what follows
        host->audioInputDeviceID = device.ID;
        host->deviceInputChannels = device.inputChannels;
        host->audioInputUsed = device.inputChannels > 0;
        host->audioOutputDeviceID = device.ID;
        host->deviceOutputChannels = device.outputChannels;
        host->audioOutputUsed = device.outputChannels > 0;
        return Start(*host, errors);
    }

    std::string SetSampleRate(int rate) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        if (rate <= 0)
        {
            return "That is not a sample rate.";
        }

        ErrorCapture errors(*host);
        host->currentSampleRate = rate;
        return Start(*host, errors);
    }

    std::string SetBufferSize(int frames) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        if (frames <= 0)
        {
            return "That is not a buffer size.";
        }

        ErrorCapture errors(*host);
        // RtAudio grants what the device can do and writes it back, so the reply shows that.
        host->currentBufferSize = static_cast<uint32_t>(frames);
        return Start(*host, errors);
    }

    std::string Restart() override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        ErrorCapture errors(*host);

        // A device unplugged and plugged back in is looked for again by name, outside ASIO (see
        // Snapshot), falling back to the system's default when it has gone.
        if (!IsLinkedApi(host->audioApi))
        {
            const auto inputName = host->deviceName(host->audioInputDeviceID);
            const auto outputName = host->deviceName(host->audioOutputDeviceID);
            host->stopAudioThread();
            host->invalidateDeviceList();
            host->audioInputDeviceID = host->resolveInputDevice(inputName);
            host->audioOutputDeviceID = host->resolveOutputDevice(outputName);
            host->audioInputUsed = host->audioInputUsed && host->isKnownDevice(host->audioInputDeviceID);
            host->audioOutputUsed = host->audioOutputUsed && host->isKnownDevice(host->audioOutputDeviceID);
            RefreshChannelCounts(*host);
        }

        return Start(*host, errors);
    }

    std::string SetMidiInput(const std::string& name, bool enabled) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return "The audio host is not running.";
        }

        // From "every port" to an explicit list the first time one is touched, as the wrapper's
        // own window does; names wanted but unplugged stay wanted.
        auto chosen = host->settings.midiBindAllPorts ? host->getMidiPortNames() : host->settings.midiPortNames;
        const auto it = std::find(chosen.begin(), chosen.end(), name);

        if (enabled && it == chosen.end())
        {
            chosen.push_back(name);
        }
        else if (!enabled && it != chosen.end())
        {
            chosen.erase(it);
        }

        host->openMidiPorts(chosen, false);
        host->settings.midiBindAllPorts = false;
        host->settings.midiPortNames = chosen;
        Save(*host);
        return {};
    }

    std::string ReadSetting(const std::string& key) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return {};
        }

        for (const auto& [k, v] : host->settings.unknownKeys)
        {
            if (k == key)
            {
                return v;
            }
        }

        return {};
    }

    void WriteSetting(const std::string& key, const std::string& value) override
    {
        auto* host = sa::getStandaloneHost();

        if (host == nullptr)
        {
            return;
        }

        auto& keys = host->settings.unknownKeys;
        const auto it = std::find_if(keys.begin(), keys.end(), [&](const auto& kv) { return kv.first == key; });

        if (it != keys.end())
        {
            it->second = value;
        }
        else
        {
            keys.emplace_back(key, value);
        }

        Save(*host);
    }
};

ClapWrapperBackend gBackend;
[[maybe_unused]] const bool gRegistered = (RegisterDeviceBackend(&gBackend), true);
} // namespace
} // namespace nanoq
