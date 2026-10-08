// The MIDI half of StandaloneAudioSettings: the ports the user chose, and putting them
// back when one is plugged in again (see the class comment). The rest of the class is in
// StandaloneAudioSettings.cpp.
#include "StandaloneAudioSettings.h"

// Same include order as StandaloneAudioSettings.cpp: the standalone window header expects
// the audio and GUI modules to be visible before it.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <nlohmann/json.hpp>

namespace
{
    // The MIDI ports the user chose, as JSON {inputs: [{name, identifier}], output},
    // kept apart from JUCE's "audioSetup", which forgets a port that is unplugged.
    constexpr auto kMidiDevicesKey = "soundshedMidiDevices";
    constexpr auto kJuceAudioSetupKey = "audioSetup";

    std::string toStd (const juce::String& s)
    {
        return s.toStdString();
    }

    juce::String stringArg (const nlohmann::json& object, const char* key)
    {
        const auto it = object.find (key);
        return it != object.end() && it->is_string() ? juce::String (it->get<std::string>()) : juce::String();
    }

    nlohmann::json toJson (const juce::MidiDeviceInfo& info)
    {
        return { { "name", toStd (info.name) }, { "identifier", toStd (info.identifier) } };
    }

    juce::MidiDeviceInfo midiDeviceFromJson (const nlohmann::json& json)
    {
        return json.is_object() ? juce::MidiDeviceInfo (stringArg (json, "name"), stringArg (json, "identifier"))
                                : juce::MidiDeviceInfo();
    }

    bool isEmpty (const juce::MidiDeviceInfo& info)
    {
        return info.identifier.isEmpty() && info.name.isEmpty();
    }

    // The port a remembered one is now, matched as JUCE matches a saved setup: by
    // identifier, else by name, since an identifier can change with the USB socket.
    std::optional<juce::MidiDeviceInfo> findMidiDevice (const juce::Array<juce::MidiDeviceInfo>& available,
        const juce::MidiDeviceInfo& wanted)
    {
        for (const auto& device : available)
            if (device.identifier == wanted.identifier)
                return device;

        if (wanted.name.isNotEmpty())
            for (const auto& device : available)
                if (device.name == wanted.name)
                    return device;

        return std::nullopt;
    }
} // namespace

//==============================================================================
juce::String StandaloneAudioSettings::setMidiInputEnabled (const juce::String& identifier, bool enabled)
{
    if (identifier.isEmpty())
        return "No MIDI input given.";

    mDeviceManager.setMidiInputDeviceEnabled (identifier, enabled);

    if (mManageMidiDevices)
    {
        mWantedMidiInputs.removeIf ([&] (const auto& wanted) { return wanted.identifier == identifier; });

        // Only a port that opened, so what is remembered matches what the UI shows.
        if (enabled && mDeviceManager.isMidiInputDeviceEnabled (identifier))
            if (const auto device = findMidiDevice (juce::MidiInput::getAvailableDevices(), { {}, identifier }))
                mWantedMidiInputs.add (*device);

        saveWantedMidiDevices();
    }

    saveDeviceSetup();
    return {};
}

juce::String StandaloneAudioSettings::setMidiOutput (const juce::String& identifier)
{
    // Empty is "none". The holder's player picks the new output up when the device
    // manager restarts its callbacks, which this does.
    mDeviceManager.setDefaultMidiOutputDevice (identifier);

    if (mManageMidiDevices)
    {
        auto* output = mDeviceManager.getDefaultMidiOutput();
        mWantedMidiOutput = output != nullptr ? output->getDeviceInfo() : juce::MidiDeviceInfo();
        saveWantedMidiDevices();
    }

    saveDeviceSetup();
    return {};
}

//==============================================================================
void StandaloneAudioSettings::loadWantedMidiDevices()
{
    auto* settings = mHolder.settings.get();

    if (settings != nullptr && settings->containsKey (kMidiDevicesKey))
    {
        const auto saved = nlohmann::json::parse (toStd (settings->getValue (kMidiDevicesKey)), nullptr, false);

        if (saved.is_object())
        {
            if (const auto inputs = saved.find ("inputs"); inputs != saved.end() && inputs->is_array())
                for (const auto& input : *inputs)
                    if (const auto info = midiDeviceFromJson (input); ! isEmpty (info))
                        mWantedMidiInputs.add (info);

            if (const auto output = saved.find ("output"); output != saved.end())
                mWantedMidiOutput = midiDeviceFromJson (*output);

            return;
        }
    }

    // The first run with this in place: start from JUCE's saved setup, which still
    // names inputs that were enabled but unplugged, and add what it has open now.
    if (settings != nullptr)
    {
        if (const auto xml = settings->getXmlValue (kJuceAudioSetupKey))
        {
            for (auto* input : xml->getChildWithTagNameIterator ("MIDIINPUT"))
                mWantedMidiInputs.addIfNotAlreadyThere ({ input->getStringAttribute ("name"), input->getStringAttribute ("identifier") });

            mWantedMidiOutput = { xml->getStringAttribute ("defaultMidiOutput"), xml->getStringAttribute ("defaultMidiOutputDevice") };
        }
    }

    for (const auto& input : juce::MidiInput::getAvailableDevices())
        if (mDeviceManager.isMidiInputDeviceEnabled (input.identifier))
            mWantedMidiInputs.addIfNotAlreadyThere (input);

    if (auto* output = mDeviceManager.getDefaultMidiOutput())
        mWantedMidiOutput = output->getDeviceInfo();

    saveWantedMidiDevices();
}

void StandaloneAudioSettings::saveWantedMidiDevices()
{
    auto* settings = mHolder.settings.get();

    if (settings == nullptr)
        return;

    auto inputs = nlohmann::json::array();

    for (const auto& input : mWantedMidiInputs)
        inputs.push_back (toJson (input));

    nlohmann::json saved;
    saved["inputs"] = std::move (inputs);
    saved["output"] = toJson (mWantedMidiOutput);

    settings->setValue (kMidiDevicesKey, juce::String (saved.dump()));
}

void StandaloneAudioSettings::reconcileMidiDevices()
{
    bool wantedChanged = false;

    const auto inputs = juce::MidiInput::getAvailableDevices();
    juce::StringArray openInputs;

    for (auto& wanted : mWantedMidiInputs)
    {
        if (const auto device = findMidiDevice (inputs, wanted))
        {
            // Found by name under a new identifier: remember that one, so the dead
            // connection can be found under it once this port goes away.
            if (device->identifier != wanted.identifier)
            {
                wanted = *device;
                wantedChanged = true;
            }

            openInputs.addIfNotAlreadyThere (device->identifier);
        }
        else if (mDeviceManager.isMidiInputDeviceEnabled (wanted.identifier))
        {
            // Gone. Drop the dead connection, or the port coming back looks open already.
            juce::Logger::writeToLog ("[audio] MIDI input went away: " + wanted.name);
            mDeviceManager.setMidiInputDeviceEnabled (wanted.identifier, false);
        }
    }

    for (const auto& input : inputs)
    {
        const bool open = mDeviceManager.isMidiInputDeviceEnabled (input.identifier);
        const bool wanted = openInputs.contains (input.identifier);

        if (wanted && ! open)
        {
            juce::Logger::writeToLog ("[audio] reopening MIDI input " + input.name);
            mDeviceManager.setMidiInputDeviceEnabled (input.identifier, true);
        }
        else if (open && ! wanted)
        {
            // One the device manager reopened from its startup setup after the user
            // turned it off.
            mDeviceManager.setMidiInputDeviceEnabled (input.identifier, false);
        }
    }

    // The output: the remembered one if it is plugged in, else none, which also
    // releases the dead connection of one that went away. Changing it restarts the
    // audio callbacks, so only when it differs.
    juce::String output;

    if (! isEmpty (mWantedMidiOutput))
    {
        if (const auto device = findMidiDevice (juce::MidiOutput::getAvailableDevices(), mWantedMidiOutput))
        {
            if (device->identifier != mWantedMidiOutput.identifier)
            {
                mWantedMidiOutput = *device;
                wantedChanged = true;
            }

            output = device->identifier;
        }
    }

    if (output != mDeviceManager.getDefaultMidiOutputIdentifier())
    {
        juce::Logger::writeToLog (output.isNotEmpty() ? "[audio] reopening MIDI output " + mWantedMidiOutput.name
                                                      : "[audio] MIDI output went away: " + mWantedMidiOutput.name);
        mDeviceManager.setDefaultMidiOutputDevice (output);
    }

    if (wantedChanged)
        saveWantedMidiDevices();
}
