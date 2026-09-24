#include "nativeui/views/SettingsPage.h"

#include "ProductInfo.h"

namespace soundshed::nano
{
namespace
{
// The keys Soundshed Guitar's settings pages write (core/ui/ts/settings/keys.ts), with
// their defaults, so the two products read one setting the same way.
constexpr const char* kOutputLimiterSetting = "audio.dsp.outputLimiterEnabled";
constexpr const char* kPresetSwitchTailSetting = "audio.presetSwitch.tailBars";
constexpr int kPresetSwitchTailDefault = 2;
constexpr const char* kNamSlimmableSizeSetting = "audio.nam.slimmableSize";
constexpr const char* kNamOversamplingSetting = "audio.nam.oversampling";
constexpr const char* kNamAntiAliasPhaseSetting = "audio.nam.antiAliasPhase";
constexpr const char* kUiScaleSetting = "nativeUi.scale";
constexpr const char* kChainWrapSetting = "nativeUi.chainWrap";

constexpr const char* kWebsite = "https://soundshed.com";

int rowHeightFor (const NanoContext& context)
{
    return context.touch ? 48 : 38;
}

double numberSetting (const nlohmann::json& settings, const char* key, double fallback)
{
    const auto it = settings.find (key);
    return it != settings.end() && it->is_number() ? it->get<double>() : fallback;
}

bool boolSetting (const nlohmann::json& settings, const char* key, bool fallback)
{
    const auto it = settings.find (key);
    return it != settings.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

juce::String text (const std::string& utf8)
{
    return juce::String::fromUTF8 (utf8.c_str());
}

void selectById (juce::ComboBox& box, int id)
{
    if (box.getSelectedId() != id)
        box.setSelectedId (id, juce::dontSendNotification);
}
} // namespace

//==============================================================================
void EvenRow::resized()
{
    const int count = getNumChildComponents();

    if (count == 0)
        return;

    auto area = getLocalBounds();
    const int width = area.getWidth() / count;

    for (int i = 0; i < count; ++i)
        getChildComponent (i)->setBounds (i == count - 1 ? area : area.removeFromLeft (width));
}

//==============================================================================
SettingsColumn::SettingsColumn (NanoContext& contextIn) : context (contextIn)
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::nonHover);
    addAndMakeVisible (viewport);
}

void SettingsColumn::addHeading (const juce::String& heading)
{
    auto label = std::make_unique<juce::Label> (juce::String(), heading.toUpperCase());
    label->setFont (context.font (NanoTheme::textOverline, FontWeight::semibold).withExtraKerningFactor (0.06f));
    label->setColour (juce::Label::textColourId, context.theme.textMuted());
    content.addAndMakeVisible (*label);
    rows.push_back ({ std::move (label), nullptr, 0, true });
}

void SettingsColumn::addRow (const juce::String& labelText, juce::Component& control, int height)
{
    std::unique_ptr<juce::Label> label;

    if (labelText.isNotEmpty())
    {
        label = std::make_unique<juce::Label> (juce::String(), labelText);
        label->setFont (context.font (NanoTheme::textBody));
        label->setColour (juce::Label::textColourId, context.theme.text());
        content.addAndMakeVisible (*label);
    }

    content.addAndMakeVisible (control);
    rows.push_back ({ std::move (label), &control, height, false });
}

void SettingsColumn::addNote (const juce::String& note)
{
    auto label = std::make_unique<juce::Label> (juce::String(), note);
    label->setFont (context.font (NanoTheme::textCaption));
    label->setColour (juce::Label::textColourId, context.theme.textMuted());
    label->setJustificationType (juce::Justification::topLeft);
    content.addAndMakeVisible (*label);
    rows.push_back ({ std::move (label), nullptr, -1, false });
}

void SettingsColumn::clearRows()
{
    for (auto& row : rows)
        if (row.control != nullptr)
            content.removeChildComponent (row.control);

    rows.clear();
}

void SettingsColumn::resized()
{
    viewport.setBounds (getLocalBounds());
    relayout();
}

void SettingsColumn::relayout()
{
    const int width = juce::jmax (0, viewport.getWidth() - viewport.getScrollBarThickness());
    const int rowHeight = rowHeightFor (context);
    const bool stackLabels = width < 440;
    const int labelWidth = juce::jmin (220, width * 2 / 5);
    int y = 8;

    for (auto& row : rows)
    {
        const auto inner = juce::Rectangle<int> (12, y, juce::jmax (0, width - 24), 0);

        if (row.heading)
        {
            y += 10;
            row.label->setBounds (inner.withY (y).withHeight (24));
            y += 28;
            continue;
        }

        if (row.control == nullptr)
        {
            // A note wraps over as many lines as it needs.
            const auto font = context.font (NanoTheme::textCaption);
            const int lines = juce::jmax (1, (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, row.label->getText())
                                                          / juce::jmax (1.0f, (float) inner.getWidth() - 8.0f)));
            const int height = lines * 17 + 6;
            row.label->setBounds (inner.withHeight (height));
            y += height + 2;
            continue;
        }

        const int controlHeight = row.height > 0 ? row.height : rowHeight;

        if (row.label == nullptr)
        {
            row.control->setBounds (inner.withHeight (controlHeight));
            y += controlHeight + 6;
        }
        else if (stackLabels)
        {
            row.label->setBounds (inner.withHeight (22));
            row.control->setBounds (inner.withY (y + 22).withHeight (controlHeight));
            y += 22 + controlHeight + 8;
        }
        else
        {
            auto line = inner.withHeight (controlHeight);
            row.label->setBounds (line.removeFromLeft (labelWidth));
            row.control->setBounds (line);
            y += controlHeight + 6;
        }
    }

    content.setSize (width, y + 12);
}

//==============================================================================
SettingsPage::SettingsPage (NanoContext& contextIn, ShellActions& actionsIn)
    : SettingsColumn (contextIn),
      actions (actionsIn),
      devicesButton (contextIn, "settings-devices", "speaker", "Audio & MIDI devices"),
      websiteButton (contextIn, "settings-website", {}, "soundshed.com")
{
    setComponentID ("page-settings");

    const std::array<std::array<const char*, 2>, 3> themes { { { "dark", "Dark" }, { "light", "Light" }, { "classic", "Classic" } } };

    for (std::size_t i = 0; i < themes.size(); ++i)
    {
        const std::string theme = themes[i][0];
        themeButtons[i] = std::make_unique<IconButton> (context, juce::String ("theme-") + themes[i][0], juce::String(), themes[i][1]);
        themeButtons[i]->setClickingTogglesState (false);
        themeButtons[i]->onClick = [this, theme] { context.commands.SetTheme (theme); };
        themeRow.addAndMakeVisible (*themeButtons[i]);
    }

    scaleSlider.setComponentID ("settings-scale");
    scaleSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    scaleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 24);
    scaleSlider.setRange (80.0, 150.0, 5.0);
    scaleSlider.setTextValueSuffix ("%");
    scaleSlider.onDragEnd = [this] { context.commands.SetSetting (kUiScaleSetting, scaleSlider.getValue() / 100.0); };
    scaleSlider.onValueChange = [this] {
        if (! scaleSlider.isMouseButtonDown())
            context.commands.SetSetting (kUiScaleSetting, scaleSlider.getValue() / 100.0);
    };

    chainWrapToggle.setComponentID ("settings-chain-wrap");
    chainWrapToggle.setButtonText ("Wrap the chain to fit the page");
    chainWrapToggle.onClick = [this] { context.commands.SetSetting (kChainWrapSetting, chainWrapToggle.getToggleState()); };

    devicesButton.onClick = [this] {
        if (actions.showPage)
            actions.showPage (Page::Device);
    };

    limiterToggle.setComponentID ("settings-limiter");
    limiterToggle.setButtonText ("Soft limit the output");
    limiterToggle.onClick = [this] { context.commands.SetSetting (kOutputLimiterSetting, limiterToggle.getToggleState()); };

    tailsBox.setComponentID ("settings-tails");
    tailsBox.addItem ("Off", 1);

    for (int bars = 1; bars <= 4; ++bars)
        tailsBox.addItem (juce::String (bars) + (bars == 1 ? " bar" : " bars"), bars + 1);

    tailsBox.onChange = [this] { context.commands.SetSetting (kPresetSwitchTailSetting, tailsBox.getSelectedId() - 1); };

    qualitySlider.setComponentID ("settings-nam-quality");
    qualitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    qualitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 24);
    qualitySlider.setRange (0.0, 1.0, 0.01);
    qualitySlider.textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)) + "%"; };
    qualitySlider.valueFromTextFunction = [] (const juce::String& value) { return value.getDoubleValue() / 100.0; };
    qualitySlider.onDragEnd = [this] { context.commands.SetSetting (kNamSlimmableSizeSetting, qualitySlider.getValue()); };
    qualitySlider.onValueChange = [this] {
        if (! qualitySlider.isMouseButtonDown())
            context.commands.SetSetting (kNamSlimmableSizeSetting, qualitySlider.getValue());
    };

    oversamplingBox.setComponentID ("settings-nam-oversampling");
    oversamplingBox.addItemList ({ "Off", "2x", "4x", "8x", "16x", "32x" }, 1);
    oversamplingBox.onChange = [this] { context.commands.SetSetting (kNamOversamplingSetting, oversamplingBox.getSelectedId() - 1); };

    antiAliasBox.setComponentID ("settings-nam-antialias");
    antiAliasBox.addItemList ({ "Minimum Phase", "Linear Short", "Linear Long" }, 1);
    antiAliasBox.onChange = [this] { context.commands.SetSetting (kNamAntiAliasPhaseSetting, antiAliasBox.getSelectedId() - 1); };

    for (auto* label : { &dspLabel, &aboutLabel })
    {
        label->setFont (context.font (NanoTheme::textLabel));
        label->setColour (juce::Label::textColourId, context.theme.textMuted());
    }

    devicesButton.setAlignLeft (true);
    websiteButton.setFlat (true);
    websiteButton.setAlignLeft (true);
    websiteButton.onClick = [this] { context.commands.OpenUrl (kWebsite); };

    rebuild();

    sessionSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Session, [this] {
        if (builtWithDevices != context.state().environment.audioDeviceSettings)
            rebuild();

        refresh();
    });
    telemetrySubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Telemetry, [this] {
        if (! isShowing())
            return;

        const auto& telemetry = context.state().telemetry;
        dspLabel.setText ("DSP load " + juce::String (telemetry.dspLoadPercent, 0) + "%", juce::dontSendNotification);
    });
    refresh();
}

SettingsPage::~SettingsPage() = default;

void SettingsPage::rebuild()
{
    clearRows();
    builtWithDevices = context.state().environment.audioDeviceSettings;

    addHeading ("Appearance");
    addRow ("Theme", themeRow);
    addRow ("Size", scaleSlider);
    addRow ("Signal chain", chainWrapToggle);

    if (builtWithDevices)
    {
        addHeading ("Audio");
        addRow ({}, devicesButton);
    }

    addHeading ("Output");
    addRow ({}, limiterToggle);
    addRow ("Preset switch tails", tailsBox);
    addNote ("How many bars of delay and reverb tail carry over when you change preset.");

    addHeading ("Amp models (NAM)");
    addRow ("Quality", qualitySlider);
    addNote ("Lower quality reduces CPU use on models that support it. Watch the DSP load while you adjust it.");
    addRow ({}, dspLabel, 24);
    addRow ("Oversampling", oversamplingBox);
    addRow ("Anti-alias filter", antiAliasBox);

    addHeading ("About");
    addRow ({}, aboutLabel, 44);
    addRow ({}, websiteButton);
    addNote ("Tone sharing, riff capture, the JAM tools, plugin hosting and preset blending are in Soundshed Guitar, "
             "which shares this app's presets, library and settings.");

    relayout();
}

void SettingsPage::refresh()
{
    const auto& state = context.state();
    const auto& settings = state.appSettings;

    const std::array<const char*, 3> themes { "dark", "light", "classic" };

    for (std::size_t i = 0; i < themes.size(); ++i)
        themeButtons[i]->setToggleState (state.theme == themes[i], juce::dontSendNotification);

    if (! scaleSlider.isMouseButtonDown())
        scaleSlider.setValue (juce::jlimit (80.0, 150.0, numberSetting (settings, kUiScaleSetting, 1.0) * 100.0), juce::dontSendNotification);

    chainWrapToggle.setToggleState (boolSetting (settings, kChainWrapSetting, false), juce::dontSendNotification);
    limiterToggle.setToggleState (boolSetting (settings, kOutputLimiterSetting, false), juce::dontSendNotification);
    selectById (tailsBox, juce::jlimit (0, 4, (int) numberSetting (settings, kPresetSwitchTailSetting, kPresetSwitchTailDefault)) + 1);

    if (! qualitySlider.isMouseButtonDown())
        qualitySlider.setValue (juce::jlimit (0.0, 1.0, numberSetting (settings, kNamSlimmableSizeSetting, 1.0)), juce::dontSendNotification);

    selectById (oversamplingBox, juce::jlimit (0, 5, (int) numberSetting (settings, kNamOversamplingSetting, 0)) + 1);
    selectById (antiAliasBox, juce::jlimit (0, 2, (int) numberSetting (settings, kNamAntiAliasPhaseSetting, 0)) + 1);

    const auto& environment = state.environment;
    auto about = juce::String (soundshed::product::isNano ? "Soundshed Guitar Nano" : "Soundshed Guitar");

    if (! environment.version.empty())
        about << " " << text (environment.version);

    about << (environment.standalone ? " (app)" : " (plugin)");

    if (! environment.os.empty())
        about << "\n" << text (environment.os);

    aboutLabel.setText (about, juce::dontSendNotification);
}

//==============================================================================
DevicePage::DevicePage (NanoContext& contextIn, ShellActions& actionsIn)
    : SettingsColumn (contextIn),
      actions (actionsIn),
      backButton (contextIn, "device-back", "arrow-left", "Settings"),
      rescanButton (contextIn, "device-rescan", {}, "Rescan"),
      inputMeter (contextIn, "In"),
      testToneButton (contextIn, "device-test-tone", {}, "Test tone"),
      controlPanelButton (contextIn, "device-control-panel", {}, "Driver panel"),
      resetButton (contextIn, "device-reset", {}, "Reset to default"),
      permissionButton (contextIn, "device-permission", {}, "Allow microphone input")
{
    setComponentID ("page-device");

    backButton.setFlat (true);
    backButton.onClick = [this] {
        if (actions.showPage)
            actions.showPage (Page::Settings);
    };
    rescanButton.onClick = [this] { context.commands.AudioDevice ("getState", { { "rescan", true } }); };
    headerRow.addAndMakeVisible (backButton);
    headerRow.addAndMakeVisible (rescanButton);

    typeBox.setComponentID ("device-type");
    typeBox.onChange = [this] { context.commands.AudioDevice ("setDeviceType", { { "deviceType", typeBox.getText().toStdString() } }); };
    inputBox.setComponentID ("device-input");
    inputBox.onChange = [this] {
        const auto name = inputBox.getSelectedId() == 1 ? std::string() : inputBox.getText().toStdString();
        context.commands.AudioDevice ("setDevice", { { "kind", "input" }, { "name", name } });
    };
    outputBox.setComponentID ("device-output");
    outputBox.onChange = [this] { context.commands.AudioDevice ("setDevice", { { "kind", "output" }, { "name", outputBox.getText().toStdString() } }); };
    inputChannelsBox.setComponentID ("device-input-channels");
    inputChannelsBox.onChange = [this] { context.commands.AudioDevice ("setInputChannels", { { "group", inputChannelsBox.getSelectedId() - 1 } }); };
    outputChannelsBox.setComponentID ("device-output-channels");
    outputChannelsBox.onChange = [this] { context.commands.AudioDevice ("setOutputChannels", { { "group", outputChannelsBox.getSelectedId() - 1 } }); };
    rateBox.setComponentID ("device-sample-rate");
    rateBox.onChange = [this] { context.commands.AudioDevice ("setSampleRate", { { "sampleRate", rateBox.getText().getDoubleValue() } }); };
    bufferBox.setComponentID ("device-buffer-size");
    bufferBox.onChange = [this] { context.commands.AudioDevice ("setBufferSize", { { "bufferSize", bufferBox.getText().getIntValue() } }); };
    midiOutputBox.setComponentID ("device-midi-output");
    midiOutputBox.onChange = [this] {
        const auto& outputs = context.state().device.state.value ("midiOutputs", nlohmann::json::array());
        const int index = midiOutputBox.getSelectedId() - 2;
        const auto identifier = index >= 0 && index < (int) outputs.size() ? outputs[(std::size_t) index].value ("identifier", std::string()) : std::string();
        context.commands.AudioDevice ("setMidiOutput", { { "identifier", identifier } });
    };

    muteInputToggle.setComponentID ("device-mute-input");
    muteInputToggle.onClick = [this] { context.commands.AudioDevice ("setInputMuted", { { "muted", muteInputToggle.getToggleState() } }); };
    testToneButton.onClick = [this] { context.commands.AudioDevice ("playTestTone"); };
    controlPanelButton.onClick = [this] { context.commands.AudioDevice ("showControlPanel"); };
    resetButton.onClick = [this] {
        if (actions.confirm)
            actions.confirm ("Reset audio device", "Go back to the system's default audio device and settings?", "Reset",
                             [this] { context.commands.AudioDevice ("resetDevice"); });
    };
    permissionButton.onClick = [this] { context.commands.AudioDevice ("requestInputPermission"); };

    statusLabel.setFont (context.font (NanoTheme::textLabel));
    statusLabel.setColour (juce::Label::textColourId, context.theme.textMuted());
    inputMeter.setVertical (false);

    deviceSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Device, [this] { refresh(); });
    levelSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::DeviceLevels, [this] {
        // Only the meter: refilling the lists here would fight an open drop-down.
        inputMeter.setLevel (context.state().device.inputLevelDb, context.state().device.inputLevelDb > -0.1, NanoContext::now());
    });
    refresh();
}

DevicePage::~DevicePage()
{
    context.client.SetDeviceLevelWatch (false);
}

void DevicePage::visibilityChanged()
{
    const bool showing = isVisible();
    context.client.SetDeviceLevelWatch (showing);

    if (showing)
        context.commands.AudioDevice ("getState");
}

void DevicePage::rebuild()
{
    clearRows();
    midiInputs.clear();

    const auto& device = context.state().device;
    const auto& state = device.state;
    addRow ({}, headerRow);

    if (device.known && ! device.available)
    {
        addNote ("In a plugin, the host (your DAW) owns the audio and MIDI devices.");
        relayout();
        return;
    }

    addHeading ("Audio device");

    if (state.value ("deviceTypes", nlohmann::json::array()).size() > 1)
        addRow ("Driver", typeBox);

    if (state.value ("separateIO", true))
    {
        addRow ("Input", inputBox);
        addRow ("Output", outputBox);
    }
    else
    {
        addRow ("Device", outputBox);
    }

    if (state.value ("inputChannels", nlohmann::json::array()).size() > 1)
        addRow ("Input channels", inputChannelsBox);

    if (state.value ("outputChannels", nlohmann::json::array()).size() > 1)
        addRow ("Output channels", outputChannelsBox);

    addRow ("Sample rate", rateBox);
    addRow ("Buffer size", bufferBox);
    addRow ("Input level", inputMeter, 20);
    addRow ({}, statusLabel, 40);

    if (state.value ("inputPermission", std::string ("granted")) != "granted")
        addRow ({}, permissionButton);

    if (state.value ("canMuteInput", false))
        addRow ({}, muteInputToggle);

    addRow ({}, testToneButton);

    if (state.value ("hasControlPanel", false))
        addRow ({}, controlPanelButton);

    addRow ({}, resetButton);

    const auto& inputs = state.value ("midiInputs", nlohmann::json::array());
    const bool showMidiOutput = state.value ("showMidiOutput", false);

    if (! inputs.empty() || showMidiOutput)
        addHeading ("MIDI");

    if (inputs.empty())
        addNote ("No MIDI inputs found. Connect a controller and press Rescan.");

    for (const auto& input : inputs)
    {
        auto toggle = std::make_unique<juce::ToggleButton> (text (input.value ("name", std::string())));
        const auto identifier = input.value ("identifier", std::string());
        toggle->setComponentID ("device-midi-" + text (identifier));
        auto* raw = toggle.get();
        toggle->onClick = [this, identifier, raw] {
            context.commands.AudioDevice ("setMidiInputEnabled", { { "identifier", identifier }, { "enabled", raw->getToggleState() } });
        };
        addRow ({}, *toggle);
        midiInputs.push_back (std::move (toggle));
    }

    if (showMidiOutput)
        addRow ("MIDI output", midiOutputBox);

    relayout();
}

void DevicePage::refresh()
{
    const auto& device = context.state().device;
    const auto& state = device.state;
    const auto list = [&state] (const char* key) { return state.value (key, nlohmann::json::array()); };

    // Rebuild the rows only when what they are changes, not for every value.
    std::string shape = device.known ? (device.available ? "a" : "p") : "?";
    shape += state.value ("separateIO", true) ? "s" : "d";
    shape += std::to_string (list ("deviceTypes").size()) + "/" + std::to_string (list ("inputChannels").size()) + "/"
             + std::to_string (list ("outputChannels").size());
    shape += state.value ("inputPermission", std::string ("granted")) + (state.value ("canMuteInput", false) ? "m" : "")
             + (state.value ("hasControlPanel", false) ? "c" : "") + (state.value ("showMidiOutput", false) ? "o" : "");

    for (const auto& input : list ("midiInputs"))
        shape += "|" + input.value ("identifier", std::string());

    if (shape != builtFor)
    {
        builtFor = shape;
        rebuild();
    }

    const auto fillNames = [] (juce::ComboBox& box, const nlohmann::json& names, const std::string& current, bool withNone) {
        box.clear (juce::dontSendNotification);
        int id = 1;

        if (withNone)
            box.addItem ("None", id++);

        int selected = withNone && current.empty() ? 1 : 0;

        for (const auto& name : names)
        {
            const auto value = name.is_string() ? name.get<std::string>() : name.dump();
            box.addItem (text (value), id);

            if (value == current)
                selected = id;

            ++id;
        }

        box.setSelectedId (selected, juce::dontSendNotification);
    };

    fillNames (typeBox, list ("deviceTypes"), state.value ("deviceType", std::string()), false);
    fillNames (inputBox, list ("inputDevices"), state.value ("inputDevice", std::string()), true);
    fillNames (outputBox, list ("outputDevices"), state.value ("outputDevice", std::string()), false);

    const auto fillGroups = [] (juce::ComboBox& box, const nlohmann::json& groups) {
        box.clear (juce::dontSendNotification);
        int id = 1;

        for (const auto& group : groups)
        {
            box.addItem (text (group.value ("name", std::string())), id);

            if (group.value ("active", false))
                box.setSelectedId (id, juce::dontSendNotification);

            ++id;
        }
    };

    fillGroups (inputChannelsBox, list ("inputChannels"));
    fillGroups (outputChannelsBox, list ("outputChannels"));

    const auto fillNumbers = [] (juce::ComboBox& box, const nlohmann::json& values, double current, const char* suffix) {
        box.clear (juce::dontSendNotification);
        int id = 1;

        for (const auto& value : values)
        {
            const double number = value.is_number() ? value.get<double>() : 0.0;
            box.addItem (juce::String (number, 0) + suffix, id);

            if (std::abs (number - current) < 0.5)
                box.setSelectedId (id, juce::dontSendNotification);

            ++id;
        }
    };

    const double rate = state.value ("sampleRate", 0.0);
    const int buffer = state.value ("bufferSize", 0);
    fillNumbers (rateBox, list ("sampleRates"), rate, " Hz");
    fillNumbers (bufferBox, list ("bufferSizes"), (double) buffer, " samples");

    midiOutputBox.clear (juce::dontSendNotification);
    midiOutputBox.addItem ("None", 1);
    midiOutputBox.setSelectedId (1, juce::dontSendNotification);
    int outputId = 2;

    for (const auto& output : list ("midiOutputs"))
    {
        midiOutputBox.addItem (text (output.value ("name", std::string())), outputId);

        if (output.value ("identifier", std::string()) == state.value ("midiOutput", std::string()))
            midiOutputBox.setSelectedId (outputId, juce::dontSendNotification);

        ++outputId;
    }

    const auto& inputs = list ("midiInputs");

    for (std::size_t i = 0; i < midiInputs.size() && i < inputs.size(); ++i)
        midiInputs[i]->setToggleState (inputs[i].value ("enabled", false), juce::dontSendNotification);

    muteInputToggle.setToggleState (state.value ("inputMuted", false), juce::dontSendNotification);

    juce::String status;

    if (! device.error.empty())
        status = text (device.error);
    else if (! state.value ("deviceOpen", false))
        status = "No audio device is open.";
    else if (rate > 0.0)
    {
        const int latency = state.value ("inputLatency", 0) + state.value ("outputLatency", 0) + buffer;
        status = "Round trip about " + juce::String (latency * 1000.0 / rate, 1) + " ms";

        if (device.xruns > 0)
            status << ", " << device.xruns << " dropouts";
    }

    statusLabel.setText (status, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, device.error.empty() ? context.theme.textMuted() : context.theme.error());
}
} // namespace soundshed::nano
