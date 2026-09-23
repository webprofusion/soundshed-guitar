#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/widgets/IconButton.h"
#include "nativeui/widgets/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace soundshed::nano
{
/// Its children side by side at equal widths (a row of choice chips).
class EvenRow final : public juce::Component
{
public:
    void resized() override;
};

/// A settings page is a column of labelled rows in a scrolling view.
class SettingsColumn : public juce::Component
{
public:
    explicit SettingsColumn (NanoContext& context);

    void resized() override;

protected:
    void addHeading (const juce::String& text);
    void addRow (const juce::String& label, juce::Component& control, int height = 0);
    void addNote (const juce::String& text);
    void clearRows();
    void relayout();

    NanoContext& context;
    juce::Viewport viewport;
    juce::Component content;

private:
    struct Row
    {
        std::unique_ptr<juce::Label> label;
        juce::Component* control = nullptr;
        int height = 0;
        bool heading = false;
    };

    std::vector<Row> rows;
};

/// Appearance, the audio devices (standalone), a few engine options, and where to go for what
/// Nano leaves out. Every setting is the shared app setting the web UI writes.
class SettingsPage final : public SettingsColumn
{
public:
    SettingsPage (NanoContext& context, ShellActions& actions);
    ~SettingsPage() override;

    void refresh();

private:
    void rebuild();

    ShellActions& actions;
    std::array<std::unique_ptr<IconButton>, 3> themeButtons;
    EvenRow themeRow;
    juce::Slider scaleSlider;
    IconButton devicesButton;
    juce::ToggleButton limiterToggle;
    juce::ComboBox tailsBox;
    juce::Slider qualitySlider;
    juce::ComboBox oversamplingBox;
    juce::ComboBox antiAliasBox;
    juce::Label dspLabel;
    juce::Label aboutLabel;
    IconButton websiteButton;
    bool builtWithDevices = false;
    guitarfx::uiclient::Subscription sessionSubscription;
    guitarfx::uiclient::Subscription telemetrySubscription;
};

/// The standalone app's audio and MIDI devices, driven by the engine's "audioDevice" messages
/// exactly as Soundshed Guitar's settings page is (juce/source/StandaloneAudioSettings).
class DevicePage final : public SettingsColumn
{
public:
    DevicePage (NanoContext& context, ShellActions& actions);
    ~DevicePage() override;

    void visibilityChanged() override;
    void refresh();

private:
    void rebuild();

    ShellActions& actions;
    IconButton backButton;
    IconButton rescanButton;
    EvenRow headerRow;
    juce::ComboBox typeBox;
    juce::ComboBox inputBox;
    juce::ComboBox outputBox;
    juce::ComboBox inputChannelsBox;
    juce::ComboBox outputChannelsBox;
    juce::ComboBox rateBox;
    juce::ComboBox bufferBox;
    LevelMeter inputMeter;
    juce::Label statusLabel;
    juce::ToggleButton muteInputToggle { "Mute input (feedback risk)" };
    IconButton testToneButton;
    IconButton controlPanelButton;
    IconButton resetButton;
    IconButton permissionButton;
    std::vector<std::unique_ptr<juce::ToggleButton>> midiInputs;
    juce::ComboBox midiOutputBox;
    std::string builtFor;

    guitarfx::uiclient::Subscription deviceSubscription;
    guitarfx::uiclient::Subscription levelSubscription;
};
} // namespace soundshed::nano
