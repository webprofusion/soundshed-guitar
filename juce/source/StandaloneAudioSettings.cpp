#include "StandaloneAudioSettings.h"

// Same include order as Main.cpp: the standalone window header expects the audio
// and GUI modules to be visible before it.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <nlohmann/json.hpp>

namespace
{
    // The input mute chosen for each device pair, as a JSON object in the holder's
    // settings file beside JUCE's own "audioSetup" and "shouldMuteInput".
    constexpr auto kInputMuteKey = "soundshedInputMute";

    // JUCE saves this one on desktop only (StandalonePluginHolder::saveAudioDeviceState).
    constexpr auto kJuceShouldMuteInputKey = "shouldMuteInput";

    // The meter lease: how long one renewal keeps the feed alive, and how often it sends.
    constexpr juce::uint32 kLevelWatchLeaseMs = 5000;
    constexpr int kLevelFeedHz = 15;

    constexpr float kLevelFloorDb = -100.0f;

    std::string toStd (const juce::String& s)
    {
        return s.toStdString();
    }

    nlohmann::json toJson (const juce::StringArray& strings)
    {
        auto array = nlohmann::json::array();

        for (const auto& s : strings)
            array.push_back (toStd (s));

        return array;
    }

    juce::String stringArg (const nlohmann::json& request, const char* key)
    {
        const auto it = request.find (key);
        return it != request.end() && it->is_string() ? juce::String (it->get<std::string>()) : juce::String();
    }

    std::optional<double> numberArg (const nlohmann::json& request, const char* key)
    {
        const auto it = request.find (key);

        if (it == request.end() || ! it->is_number())
            return std::nullopt;

        return it->get<double>();
    }

    bool boolArg (const nlohmann::json& request, const char* key, bool fallback)
    {
        const auto it = request.find (key);
        return it != request.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    // "Input 1 + 2" for a pair, trimmed the way JUCE's own channel list trims it so
    // the labels match what a user saw in its dialog.
    juce::String nameForChannelPair (const juce::String& name1, const juce::String& name2)
    {
        juce::String commonBit;

        for (int i = 0; i < name1.length(); ++i)
            if (name1.substring (0, i).equalsIgnoreCase (name2.substring (0, i)))
                commonBit = name1.substring (0, i);

        // Split only at whitespace, so "input 11" + "input 12" does not become "input 11 + 2".
        while (commonBit.isNotEmpty() && ! juce::CharacterFunctions::isWhitespace (commonBit.getLastCharacter()))
            commonBit = commonBit.dropLastCharacters (1);

        return name1.trim() + " + " + name2.substring (commonBit.length()).trim();
    }

    // The channels in groups of `size`, each with whether any channel in it is on.
    nlohmann::json channelGroups (const juce::StringArray& names, const juce::BigInteger& active, int size)
    {
        auto groups = nlohmann::json::array();

        for (int first = 0; first < names.size(); first += size)
        {
            const auto count = juce::jmin (size, names.size() - first);
            const auto name = count == 2 ? nameForChannelPair (names[first], names[first + 1]) : names[first].trim();

            bool on = false;

            for (int i = first; i < first + count; ++i)
                on = on || active[i];

            groups.push_back ({ { "name", toStd (name) }, { "active", on } });
        }

        return groups;
    }

    // The feedback-risk heuristic: a built-in microphone playing into speakers is the
    // one setup where an unmuted input howls. Device names are all the OS offers, and
    // they read the same way everywhere: "Microphone (Realtek...)" and "Speakers" on
    // Windows, "MacBook Pro Microphone" on macOS, "... built-in microphone" and
    // "... built-in speaker" from Android's Oboe driver.
    bool looksLikeMicrophone (const juce::String& name)
    {
        const auto n = name.toLowerCase();
        return n.contains ("microphone") || n.contains ("mic array") || n.containsWholeWord ("mic");
    }

    bool looksLikeSpeakers (const juce::String& name)
    {
        const auto n = name.toLowerCase();
        return n.contains ("speaker") || n.contains ("built-in output");
    }

    bool inputPermissionGranted()
    {
        return ! juce::RuntimePermissions::isRequired (juce::RuntimePermissions::recordAudio)
               || juce::RuntimePermissions::isGranted (juce::RuntimePermissions::recordAudio);
    }
} // namespace

//==============================================================================
StandaloneAudioSettings::StandaloneAudioSettings (juce::StandalonePluginHolder& holder, SendToUI sendToUI)
    : mHolder (holder),
      mDeviceManager (holder.deviceManager),
      mSendToUI (std::move (sendToUI))
{
    mDeviceManager.addChangeListener (this);

    mManageMidiDevices = ! holder.autoOpenMidiDevices;

    if (mManageMidiDevices)
    {
        loadWantedMidiDevices();

        // The device manager has opened what its saved setup named; a port turned on
        // in a session that ended while it was unplugged is not among them.
        reconcileMidiDevices();

        // Deferred, so it runs after the device manager's own handler for the same
        // change, which reopens the ports its startup setup named whether or not
        // they are still wanted.
        mMidiDeviceListConnection = juce::MidiDeviceListConnection::make ([this] {
            juce::MessageManager::callAsync ([weakThis = juce::WeakReference<StandaloneAudioSettings> (this)] {
                if (auto* self = weakThis.get())
                    self->reconcileMidiDevices();
            });
        });
    }

    // On a first run the device is not open yet: the holder waits for the record
    // permission before opening anything. The change message that open sends
    // lands in changeListenerCallback, which does this again.
    applyMonitoringPolicy();
}

StandaloneAudioSettings::~StandaloneAudioSettings()
{
    stopTimer();
    mInputLevel = nullptr;
    mDeviceManager.removeChangeListener (this);
}

void StandaloneAudioSettings::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Every device manager change lands here: our own setters, a device unplugged,
    // a driver panel edit, a MIDI device enabled. Also the first open on a first
    // run, which happens long after construction.
    if (juce::RuntimePermissions::isRequired (juce::RuntimePermissions::recordAudio))
    {
        // The holder opens without input when record permission is refused, and
        // nothing reopens it if the permission is then granted from the system
        // settings while the app keeps running. Notice that here and reopen.
        if (! inputPermissionGranted() && mDeviceManager.getCurrentAudioDevice() != nullptr)
            mInputNeedsReopen = true;
        else if (inputPermissionGranted() && mInputNeedsReopen)
            enableInput();
    }

    applyMonitoringPolicy();
    sendState();
}

//==============================================================================
void StandaloneAudioSettings::handleRequest (const std::string& requestJson)
{
    const auto request = nlohmann::json::parse (requestJson, nullptr, false);

    if (! request.is_object())
    {
        juce::Logger::writeToLog ("[audio] ignoring a malformed audioDevice request");
        return;
    }

    const auto action = stringArg (request, "action");
    juce::String error;

    if (action == "getState")
    {
        sendState ({}, boolArg (request, "rescan", false));
        return;
    }

    if (action == "watchLevels")
    {
        watchLevels (boolArg (request, "enabled", true));
        return;
    }

    if (action == "setDeviceType")
        error = setDeviceType (stringArg (request, "deviceType"));
    else if (action == "setDevice")
        error = setDevice (stringArg (request, "kind"), stringArg (request, "name"));
    else if (action == "setInputChannels" || action == "setOutputChannels")
        error = setChannelGroup (action == "setInputChannels", static_cast<int> (numberArg (request, "group").value_or (-1.0)));
    else if (action == "setSampleRate")
        error = setSampleRate (numberArg (request, "sampleRate").value_or (0.0));
    else if (action == "setBufferSize")
        error = setBufferSize (static_cast<int> (numberArg (request, "bufferSize").value_or (0.0)));
    else if (action == "setInputMuted")
        error = setInputMuted (boolArg (request, "muted", true));
    else if (action == "playTestTone")
    {
        if (mDeviceManager.getCurrentAudioDevice() == nullptr)
            error = "No output device is open.";
        else
            mDeviceManager.playTestSound();
    }
    else if (action == "showControlPanel")
        error = showControlPanel();
    else if (action == "resetDevice")
        error = resetDevice();
    else if (action == "requestInputPermission")
        error = requestInputPermission();
    else if (action == "setMidiInputEnabled")
        error = setMidiInputEnabled (stringArg (request, "identifier"), boolArg (request, "enabled", true));
    else if (action == "setMidiOutput")
        error = setMidiOutput (stringArg (request, "identifier"));
    else
        error = "Unknown audio device action: " + action;

    if (error.isNotEmpty())
        juce::Logger::writeToLog ("[audio] " + action + " failed: " + error);

    sendState (error);
}

//==============================================================================
void StandaloneAudioSettings::sendState (const juce::String& error, bool rescan)
{
    if (! mSendToUI)
        return;

    nlohmann::json message;
    message["type"] = "audioDeviceState";
    message["available"] = true;
    message["error"] = toStd (error);
    message["state"] = buildState (rescan);

    mSendToUI (juce::String (message.dump()));
}

nlohmann::json StandaloneAudioSettings::buildState (bool rescan)
{
    nlohmann::json state;

    auto deviceTypes = nlohmann::json::array();

    for (auto* type : mDeviceManager.getAvailableDeviceTypes())
        deviceTypes.push_back (toStd (type->getTypeName()));

    state["deviceTypes"] = std::move (deviceTypes);
    state["deviceType"] = toStd (mDeviceManager.getCurrentAudioDeviceType());

    auto* type = mDeviceManager.getCurrentDeviceTypeObject();
    state["separateIO"] = type == nullptr || type->hasSeparateInputsAndOutputs();

    if (type != nullptr && rescan)
        type->scanForDevices();

    state["inputDevices"] = toJson (type != nullptr ? type->getDeviceNames (true) : juce::StringArray());
    state["outputDevices"] = toJson (type != nullptr ? type->getDeviceNames (false) : juce::StringArray());

    // Requested channels, as JUCE's dialog shows them, rather than what the
    // device reports active: some drivers open more channels than were asked for.
    const auto setup = mDeviceManager.getAudioDeviceSetup();
    state["inputDevice"] = toStd (setup.inputDeviceName);
    state["outputDevice"] = toStd (setup.outputDeviceName);

    auto* device = mDeviceManager.getCurrentAudioDevice();
    const bool open = device != nullptr && device->isOpen();
    state["deviceOpen"] = open;

    auto inputChannels = nlohmann::json::array();
    auto outputChannels = nlohmann::json::array();
    auto sampleRates = nlohmann::json::array();
    auto bufferSizes = nlohmann::json::array();

    if (device != nullptr)
    {
        inputChannels = channelGroups (device->getInputChannelNames(), setup.inputChannels, groupSize (true));
        outputChannels = channelGroups (device->getOutputChannelNames(), setup.outputChannels, groupSize (false));

        for (const auto rate : device->getAvailableSampleRates())
            sampleRates.push_back (rate);

        for (const auto size : device->getAvailableBufferSizes())
            bufferSizes.push_back (size);
    }

    state["inputChannels"] = std::move (inputChannels);
    state["outputChannels"] = std::move (outputChannels);
    state["sampleRates"] = std::move (sampleRates);
    state["bufferSizes"] = std::move (bufferSizes);

    // What the device actually runs at, not what was asked for.
    state["sampleRate"] = open ? device->getCurrentSampleRate() : 0.0;
    state["bufferSize"] = open ? device->getCurrentBufferSizeSamples() : 0;
    state["inputLatency"] = open ? device->getInputLatencyInSamples() : 0;
    state["outputLatency"] = open ? device->getOutputLatencyInSamples() : 0;
    state["hasControlPanel"] = device != nullptr && device->hasControlPanel();

    state["canMuteInput"] = mHolder.getProcessorHasPotentialFeedbackLoop();
    state["inputMuted"] = static_cast<bool> (mHolder.getMuteInputValue().getValue());
    state["feedbackRisk"] = computeFeedbackRisk();
    state["inputPermission"] = inputPermissionGranted() ? "granted" : "denied";

    auto midiInputs = nlohmann::json::array();

    for (const auto& input : juce::MidiInput::getAvailableDevices())
        midiInputs.push_back ({ { "identifier", toStd (input.identifier) },
            { "name", toStd (input.name) },
            { "enabled", mDeviceManager.isMidiInputDeviceEnabled (input.identifier) } });

    auto midiOutputs = nlohmann::json::array();

    for (const auto& output : juce::MidiOutput::getAvailableDevices())
        midiOutputs.push_back ({ { "identifier", toStd (output.identifier) }, { "name", toStd (output.name) } });

    state["midiInputs"] = std::move (midiInputs);
    state["midiOutputs"] = std::move (midiOutputs);
    state["midiOutput"] = toStd (mDeviceManager.getDefaultMidiOutputIdentifier());

    // JUCE's dialog only offers an output to a plugin that produces MIDI; this one
    // does, for the controller display (docs/automation-and-midi-mapping.md).
    state["showMidiOutput"] = mHolder.processor != nullptr && mHolder.processor->producesMidi();

    return state;
}

//==============================================================================
juce::String StandaloneAudioSettings::setDeviceType (const juce::String& typeName)
{
    // Opens the new type's default devices, as JUCE's dialog does.
    mDeviceManager.setCurrentAudioDeviceType (typeName, true);

    if (mDeviceManager.getCurrentAudioDeviceType() != typeName)
        return "Couldn't switch to " + typeName + ".";

    saveDeviceSetup();
    return {};
}

juce::String StandaloneAudioSettings::setDevice (const juce::String& kind, const juce::String& name)
{
    auto* type = mDeviceManager.getCurrentDeviceTypeObject();
    auto setup = mDeviceManager.getAudioDeviceSetup();

    // An empty name is "no device", a legitimate choice on either side. A device
    // on the side that changed starts on its default channels, as in JUCE's dialog.
    if (kind == "linked" || (type != nullptr && ! type->hasSeparateInputsAndOutputs()))
    {
        setup.inputDeviceName = name;
        setup.outputDeviceName = name;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
    }
    else if (kind == "input")
    {
        setup.inputDeviceName = name;
        setup.useDefaultInputChannels = true;
    }
    else if (kind == "output")
    {
        setup.outputDeviceName = name;
        setup.useDefaultOutputChannels = true;
    }
    else
    {
        return "Unknown device kind: " + kind;
    }

    const auto error = mDeviceManager.setAudioDeviceSetup (setup, true);

    if (error.isEmpty())
        saveDeviceSetup();

    return error;
}

juce::String StandaloneAudioSettings::setChannelGroup (bool isInput, int group)
{
    auto* device = mDeviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return "No audio device is open.";

    // One group at most, or none: JUCE's dialog, with its min of 0 and max of one
    // bus width, allows exactly that.
    const auto names = isInput ? device->getInputChannelNames() : device->getOutputChannelNames();
    const auto size = groupSize (isInput);
    juce::BigInteger channels;

    if (group >= 0)
    {
        const auto first = group * size;

        if (! juce::isPositiveAndBelow (first, names.size()))
            return "Those channels no longer exist.";

        channels.setRange (first, juce::jmin (size, names.size() - first), true);
    }

    auto setup = mDeviceManager.getAudioDeviceSetup();

    if (isInput)
    {
        setup.useDefaultInputChannels = false;
        setup.inputChannels = channels;
    }
    else
    {
        setup.useDefaultOutputChannels = false;
        setup.outputChannels = channels;
    }

    const auto error = mDeviceManager.setAudioDeviceSetup (setup, true);

    if (error.isEmpty())
        saveDeviceSetup();

    return error;
}

juce::String StandaloneAudioSettings::setSampleRate (double sampleRate)
{
    if (sampleRate <= 0.0)
        return "Invalid sample rate.";

    auto setup = mDeviceManager.getAudioDeviceSetup();
    setup.sampleRate = sampleRate;

    const auto error = mDeviceManager.setAudioDeviceSetup (setup, true);

    if (error.isEmpty())
        saveDeviceSetup();

    return error;
}

juce::String StandaloneAudioSettings::setBufferSize (int bufferSize)
{
    if (bufferSize <= 0)
        return "Invalid buffer size.";

    auto setup = mDeviceManager.getAudioDeviceSetup();
    setup.bufferSize = bufferSize;

    const auto error = mDeviceManager.setAudioDeviceSetup (setup, true);

    if (error.isEmpty())
        saveDeviceSetup();

    return error;
}

juce::String StandaloneAudioSettings::setInputMuted (bool muted)
{
    mHolder.getMuteInputValue().setValue (muted);

    // A choice made by hand holds for this device pair from now on, and the
    // automatic policy does not reconsider the pair this session.
    mMonitoringPairKey = currentDevicePairKey();

    if (mMonitoringPairKey.isNotEmpty())
        rememberMute (mMonitoringPairKey, muted);

    saveDeviceSetup();
    return {};
}

juce::String StandaloneAudioSettings::showControlPanel()
{
    auto* device = mDeviceManager.getCurrentAudioDevice();

    if (device == nullptr || ! device->hasControlPanel())
        return "This driver has no control panel.";

    bool shown = false;

    {
        // What JUCE's dialog does: a modal component of our own keeps the message
        // loop sane while the vendor's panel runs its own.
        juce::Component modalWindow;
        modalWindow.setOpaque (true);
        modalWindow.addToDesktop (0);
        modalWindow.enterModalState();

        shown = device->showControlPanel();
    }

    // The panel can change the setup behind our back, so reopen to read it back.
    return shown ? resetDevice() : juce::String();
}

juce::String StandaloneAudioSettings::resetDevice()
{
    mDeviceManager.closeAudioDevice();
    mDeviceManager.restartLastAudioDevice();

    return mDeviceManager.getCurrentAudioDevice() != nullptr ? juce::String()
                                                             : juce::String ("Couldn't reopen the audio device.");
}

juce::String StandaloneAudioSettings::requestInputPermission()
{
    if (inputPermissionGranted())
    {
        enableInput();
        return {};
    }

    juce::WeakReference<StandaloneAudioSettings> weakThis (this);

    juce::RuntimePermissions::request (juce::RuntimePermissions::recordAudio, [weakThis] (bool granted) {
        juce::MessageManager::callAsync ([weakThis, granted] {
            auto* self = weakThis.get();

            if (self == nullptr)
                return;

            if (granted)
                self->enableInput();

            // Android stops asking once the user has said no twice; from then on
            // only the system settings can grant it.
            self->sendState (granted ? juce::String()
                                     : juce::String ("Microphone access was not granted. Allow it for Soundshed Guitar "
                                                     "in the system settings, under Apps, then Permissions."));
        });
    });

    return {};
}

void StandaloneAudioSettings::enableInput()
{
    mInputNeedsReopen = false;

    // The holder opened with no input channels at all, so choosing an input device
    // now would still open none. Initialising again with input enabled is the only
    // way to change that, and it starts from the setup saved here first.
    mHolder.saveAudioDeviceState();
    mHolder.reloadAudioDeviceState (true, {}, nullptr);

    // A setup saved while input was off names no input device. Take the default.
    auto setup = mDeviceManager.getAudioDeviceSetup();
    auto* type = mDeviceManager.getCurrentDeviceTypeObject();

    if (setup.inputDeviceName.isEmpty() && type != nullptr && type->hasSeparateInputsAndOutputs())
    {
        const auto names = type->getDeviceNames (true);

        if (! names.isEmpty())
        {
            setup.inputDeviceName = names[juce::jmax (0, type->getDefaultDeviceIndex (true))];
            setup.useDefaultInputChannels = true;

            if (const auto error = mDeviceManager.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                juce::Logger::writeToLog ("[audio] opening the default input failed: " + error);
        }
    }

    saveDeviceSetup();
}

//==============================================================================
int StandaloneAudioSettings::groupSize (bool isInput) const
{
    // What JUCE's dialog uses as the most channels one side may have on: the main
    // bus's default width (see StandalonePluginHolder::showAudioSettingsDialog).
    auto* processor = mHolder.processor.get();
    auto* bus = processor != nullptr ? processor->getBus (isInput, 0) : nullptr;
    const auto width = bus != nullptr ? bus->getDefaultLayout().size() : 2;

    return juce::jlimit (1, 2, width);
}

void StandaloneAudioSettings::saveDeviceSetup()
{
    mHolder.saveAudioDeviceState();

    if (auto* settings = mHolder.settings.get())
        if (auto* file = dynamic_cast<juce::PropertiesFile*> (settings))
            file->saveIfNeeded();
}

//==============================================================================
void StandaloneAudioSettings::applyMonitoringPolicy()
{
    if (! mHolder.getProcessorHasPotentialFeedbackLoop())
        return;

    const auto key = currentDevicePairKey();

    if (key.isEmpty() || key == mMonitoringPairKey)
        return;

    mMonitoringPairKey = key;

    auto& muteValue = mHolder.getMuteInputValue();
    const bool mutedNow = static_cast<bool> (muteValue.getValue());
    bool muted = false;
    auto* settings = mHolder.settings.get();

    if (const auto remembered = rememberedMute (key))
    {
        muted = *remembered;
    }
    else if (settings != nullptr && ! settings->containsKey (kInputMuteKey) && settings->containsKey (kJuceShouldMuteInputKey))
    {
        // The first run with this policy, on a desktop that has used JUCE's dialog.
        // The holder has restored the mute from it; keep it for this pair.
        muted = mutedNow;
        rememberMute (key, muted);
    }
    else
    {
        muted = computeFeedbackRisk();
    }

    if (muted != mutedNow)
    {
        juce::Logger::writeToLog (juce::String ("[audio] input ") + (muted ? "muted" : "unmuted") + " for " + key);
        muteValue.setValue (muted);
    }
}

bool StandaloneAudioSettings::computeFeedbackRisk() const
{
    auto* device = mDeviceManager.getCurrentAudioDevice();

    if (device == nullptr || device->getActiveInputChannels().isZero() || device->getActiveOutputChannels().isZero())
        return false;

    const auto setup = mDeviceManager.getAudioDeviceSetup();
    return looksLikeMicrophone (setup.inputDeviceName) && looksLikeSpeakers (setup.outputDeviceName);
}

juce::String StandaloneAudioSettings::currentDevicePairKey() const
{
    if (mDeviceManager.getCurrentAudioDevice() == nullptr)
        return {};

    const auto setup = mDeviceManager.getAudioDeviceSetup();
    return mDeviceManager.getCurrentAudioDeviceType() + "|" + setup.inputDeviceName + "|" + setup.outputDeviceName;
}

std::optional<bool> StandaloneAudioSettings::rememberedMute (const juce::String& pairKey) const
{
    auto* settings = mHolder.settings.get();

    if (settings == nullptr)
        return std::nullopt;

    const auto remembered = juce::JSON::parse (settings->getValue (kInputMuteKey));
    const auto value = remembered.getProperty (juce::Identifier (pairKey), {});

    if (! value.isBool())
        return std::nullopt;

    return static_cast<bool> (value);
}

void StandaloneAudioSettings::rememberMute (const juce::String& pairKey, bool muted)
{
    auto* settings = mHolder.settings.get();

    if (settings == nullptr)
        return;

    auto remembered = juce::JSON::parse (settings->getValue (kInputMuteKey));

    if (! remembered.isObject())
        remembered = juce::var (new juce::DynamicObject());

    remembered.getDynamicObject()->setProperty (juce::Identifier (pairKey), muted);
    settings->setValue (kInputMuteKey, juce::JSON::toString (remembered, true));
}

//==============================================================================
void StandaloneAudioSettings::watchLevels (bool enabled)
{
    if (! enabled)
    {
        stopTimer();
        mInputLevel = nullptr;
        return;
    }

    mLevelWatchExpiresMs = juce::Time::getMillisecondCounter() + kLevelWatchLeaseMs;

    // Holding the getter is what switches the device manager's input metering on.
    if (mInputLevel == nullptr)
        mInputLevel = mDeviceManager.getInputLevelGetter();

    if (! isTimerRunning())
        startTimerHz (kLevelFeedHz);
}

void StandaloneAudioSettings::timerCallback()
{
    if (juce::Time::getMillisecondCounter() > mLevelWatchExpiresMs)
    {
        watchLevels (false);
        return;
    }

    if (! mSendToUI || mInputLevel == nullptr)
        return;

    auto* device = mDeviceManager.getCurrentAudioDevice();

    nlohmann::json message;
    message["type"] = "audioDeviceLevels";
    message["input"] = juce::Decibels::gainToDecibels (static_cast<float> (mInputLevel->getCurrentLevel()), kLevelFloorDb);

    // Dropouts since the device opened, or -1 where the driver cannot count them.
    message["xruns"] = device != nullptr ? device->getXRunCount() : -1;

    mSendToUI (juce::String (message.dump()));
}
