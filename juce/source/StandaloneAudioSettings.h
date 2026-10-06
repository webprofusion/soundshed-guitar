#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <optional>
#include <string>

namespace juce
{
    class StandalonePluginHolder;
}

//==============================================================================
/**
 * The standalone app's audio and MIDI device settings, driven from the web UI.
 *
 * This stands in for JUCE's Audio/MIDI Settings dialog
 * (StandalonePluginHolder::showAudioSettingsDialog). That dialog is a window JUCE
 * draws itself, and on Android it cannot share the screen with the WebView the
 * whole UI lives in; everywhere else it looks nothing like the rest of the app.
 * What it offers is all here, with the same semantics, so a setup made in one
 * reads back unchanged in the other:
 *
 *  - the driver type, and the input and output devices (one device for drivers
 *    such as ASIO that open both sides together);
 *  - the active input and output channels, picked as JUCE picks them: in groups
 *    as wide as the plugin's main bus (stereo pairs here), one group at most;
 *  - the sample rate and buffer size, from the device's own lists;
 *  - the driver's control panel, a device reset, and the test tone;
 *  - which MIDI inputs are enabled, and the MIDI output;
 *  - the holder's feedback-loop input mute.
 *
 * Two things the dialog does not do, both of which Android needs:
 *
 *  - It remembers the input mute. JUCE saves that only on desktop and starts a
 *    phone muted on every launch, so the amp hears nothing until the dialog is
 *    found. Here the mute is remembered per device pair, and a pair seen for the
 *    first time is muted only if it looks like a built-in mic feeding speakers.
 *  - It saves the device setup as soon as it changes. JUCE writes it on a clean
 *    exit, which a phone rarely gives an app.
 *
 * And one the device manager gets wrong on desktop: MIDI ports that come and go.
 * It reopens a returning port only if the setup it loaded at startup named it, and
 * a port that goes away leaves a dead connection behind that it still counts as
 * enabled, so the port coming back is never reopened. Here the MIDI ports the user
 * chose are remembered on their own, and the open ports are matched to them
 * whenever the device list changes.
 *
 * The UI sends {"type": "audioDevice", "action": ...}. Every request is answered
 * with a whole "audioDeviceState" snapshot, and so is every change the device
 * manager broadcasts (a device unplugged, a driver panel edit), so the UI keeps
 * no device state of its own. See docs/user-interface.md for the protocol.
 *
 * Message thread only, like the device manager itself.
 */
class StandaloneAudioSettings final : private juce::ChangeListener,
                                      private juce::Timer
{
public:
    using SendToUI = std::function<void (const juce::String& json)>;

    StandaloneAudioSettings (juce::StandalonePluginHolder& holder, SendToUI sendToUI);
    ~StandaloneAudioSettings() override;

    /// One "audioDevice" message from the UI, as JSON text.
    void handleRequest (const std::string& requestJson);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    // Every reply is the whole state, with the error from the request that caused
    // it, if any. A rescan asks the driver for its device lists again, which is
    // slow for some drivers, so only an explicit getState does it.
    void sendState (const juce::String& error = {}, bool rescan = false);
    [[nodiscard]] nlohmann::json buildState (bool rescan);

    juce::String setDeviceType (const juce::String& typeName);
    juce::String setDevice (const juce::String& kind, const juce::String& name);
    juce::String setChannelGroup (bool isInput, int group);
    juce::String setSampleRate (double sampleRate);
    juce::String setBufferSize (int bufferSize);
    juce::String setInputMuted (bool muted);
    juce::String showControlPanel();
    juce::String resetDevice();
    juce::String requestInputPermission();
    juce::String setMidiInputEnabled (const juce::String& identifier, bool enabled);
    juce::String setMidiOutput (const juce::String& identifier);

    /// Opens the input side again once record permission has been granted.
    void enableInput();

    /// Channels per group: the width of the plugin's main bus on that side, as JUCE's dialog uses.
    [[nodiscard]] int groupSize (bool isInput) const;

    /// A change the user made: persist the device setup now rather than on a clean exit.
    void saveDeviceSetup();

    // Input mute, remembered per device pair (see the class comment).
    void applyMonitoringPolicy();
    [[nodiscard]] bool computeFeedbackRisk() const;
    [[nodiscard]] juce::String currentDevicePairKey() const;
    [[nodiscard]] std::optional<bool> rememberedMute (const juce::String& pairKey) const;
    void rememberMute (const juce::String& pairKey, bool muted);

    // The MIDI ports the user chose (see the class comment).
    void loadWantedMidiDevices();
    void saveWantedMidiDevices();
    void reconcileMidiDevices();

    // The level meter lease. The UI renews it every couple of seconds while the
    // meter is on screen; it lapses on its own, so a page that went away without
    // saying so does not leave the feed running.
    void watchLevels (bool enabled);

    juce::StandalonePluginHolder& mHolder;
    juce::AudioDeviceManager& mDeviceManager;
    SendToUI mSendToUI;

    juce::AudioDeviceManager::LevelMeter::Ptr mInputLevel;
    juce::uint32 mLevelWatchExpiresMs = 0;

    // The device pair the input mute was last decided for. The mute is only
    // reconsidered when the pair changes, so a hand-picked mute is left alone.
    juce::String mMonitoringPairKey;

    // Android: the holder opened the device without input because record
    // permission was refused. Set until the input has been opened again.
    bool mInputNeedsReopen = false;

    // The MIDI inputs and output the user turned on, whether or not they are
    // plugged in now, by the identifier they were last opened under. Off on
    // mobile, where the holder opens every MIDI input that appears by itself.
    bool mManageMidiDevices = false;
    juce::Array<juce::MidiDeviceInfo> mWantedMidiInputs;
    juce::MidiDeviceInfo mWantedMidiOutput;
    juce::MidiDeviceListConnection mMidiDeviceListConnection;

    JUCE_DECLARE_WEAK_REFERENCEABLE (StandaloneAudioSettings)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StandaloneAudioSettings)
};
