#include "nativeui/widgets/ParamControl.h"

#include "uiclient/ParamFormat.h"

namespace soundshed::nano
{
using guitarfx::uiclient::FormatParamValue;
using guitarfx::uiclient::IsEnumParam;
using guitarfx::uiclient::IsToggleParam;
using guitarfx::uiclient::ParamPositionToValue;
using guitarfx::uiclient::ParamValueToPosition;
using guitarfx::uiclient::SnapParamValue;

namespace
{
constexpr int kLongPressMs = 550;
constexpr int kDragPixelsForFullRange = 200;

// A knob cell: the name over the knob, the value under it.
constexpr int kNameHeight = 17;
constexpr int kValueHeight = 17;

int knobSize (const NanoContext& context, int cellWidth)
{
    return juce::jlimit (36, context.touch ? 64 : 54, cellWidth - 16);
}
} // namespace

/// A juce::Slider whose travel follows the parameter's taper, and which opens the value menu
/// on a long press (touch) or a right click (mouse).
class ParamControl::TaperedSlider final : public juce::Slider,
                                          private juce::Timer
{
public:
    TaperedSlider (ParamControl& ownerIn) : owner (ownerIn) {}

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            owner.showValueMenu();
            return;
        }

        valueAtPress = getValue();
        longPressed = false;
        startTimer (kLongPressMs);
        juce::Slider::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 6)
            stopTimer();

        if (! longPressed)
            juce::Slider::mouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();

        if (! longPressed)
            juce::Slider::mouseUp (e);
    }

private:
    void timerCallback() override
    {
        stopTimer();

        if (! isMouseButtonDown())
            return;

        // A press that did not move: the menu, and the value it may have nudged put back.
        longPressed = true;
        setValue (valueAtPress, juce::sendNotificationSync);
        owner.showValueMenu();
    }

    ParamControl& owner;
    double valueAtPress = 0.0;
    bool longPressed = false;
};

ParamControl::ParamControl (NanoContext& contextIn, const guitarfx::uiclient::EffectParamInfo& infoIn, Style styleIn)
    : context (contextIn), info (infoIn), style (styleIn), value (infoIn.defaultValue)
{
    setComponentID ("param:" + juce::String (info.key));
    nameLabel.setText (juce::String::fromUTF8 (info.name.c_str()), juce::dontSendNotification);
    nameLabel.setJustificationType (style == Style::Knob ? juce::Justification::centred : juce::Justification::centredLeft);
    nameLabel.setInterceptsMouseClicks (false, false);
    valueLabel.setJustificationType (style == Style::Knob ? juce::Justification::centred : juce::Justification::centredRight);
    valueLabel.setInterceptsMouseClicks (false, false);

    if (IsToggleParam (info))
    {
        toggle = std::make_unique<juce::ToggleButton> (juce::String::fromUTF8 (info.name.c_str()));
        toggle->setComponentID ("param:" + juce::String (info.key));
        toggle->onClick = [this] { commit (toggle->getToggleState() ? info.maxValue : info.minValue); };
        addAndMakeVisible (*toggle);
        setComponentID ({});
    }
    else if (IsEnumParam (info))
    {
        picker = std::make_unique<juce::TextButton>();
        picker->setComponentID ("param:" + juce::String (info.key));
        picker->onClick = [this] { showValueMenu(); };
        addAndMakeVisible (*picker);
        addAndMakeVisible (nameLabel);
        setComponentID ({});
    }
    else
    {
        slider = std::make_unique<TaperedSlider> (*this);
        slider->setComponentID ("param:" + juce::String (info.key));
        setComponentID ({});

        const auto paramInfo = info;
        juce::NormalisableRange<double> range (
            info.minValue, info.maxValue,
            [paramInfo] (double, double, double position) { return ParamPositionToValue (paramInfo, position); },
            [paramInfo] (double, double, double v) { return ParamValueToPosition (paramInfo, v); },
            [paramInfo] (double, double, double v) { return SnapParamValue (paramInfo, v); });
        slider->setNormalisableRange (range);
        slider->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider->setDoubleClickReturnValue (true, info.defaultValue);
        slider->setScrollWheelEnabled (true);

        if (style == Style::Knob)
        {
            slider->setSliderStyle (juce::Slider::RotaryVerticalDrag);
            slider->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
            slider->setMouseDragSensitivity (kDragPixelsForFullRange);
        }
        else
        {
            slider->setSliderStyle (juce::Slider::LinearHorizontal);
            slider->setSliderSnapsToMousePosition (false);
        }

        slider->setValue (info.defaultValue, juce::dontSendNotification);
        slider->onValueChange = [this] { commit (slider->getValue()); };
        addAndMakeVisible (*slider);
        addAndMakeVisible (nameLabel);
        addAndMakeVisible (valueLabel);
    }

    setTitle (juce::String::fromUTF8 (info.name.c_str()));
    lookAndFeelChanged();
    refreshText();
}

ParamControl::~ParamControl() = default;

void ParamControl::setValue (double newValue)
{
    if (std::abs (newValue - value) < 1e-12)
        return;

    value = newValue;

    if (slider != nullptr && ! slider->isMouseButtonDown())
        slider->setValue (newValue, juce::dontSendNotification);

    if (toggle != nullptr)
        toggle->setToggleState (newValue >= 0.5 * (info.minValue + info.maxValue), juce::dontSendNotification);

    refreshText();
}

void ParamControl::commit (double newValue)
{
    newValue = SnapParamValue (info, newValue);

    if (std::abs (newValue - value) < 1e-12)
        return;

    value = newValue;
    refreshText();

    if (onChange)
        onChange (newValue);
}

void ParamControl::setFormatter (std::function<juce::String (double)> formatter)
{
    customFormatter = std::move (formatter);
    refreshText();
}

void ParamControl::refreshText()
{
    const auto text = customFormatter ? customFormatter (value) : juce::String::fromUTF8 (FormatParamValue (info, value).c_str());
    valueLabel.setText (text, juce::dontSendNotification);

    if (picker != nullptr)
        picker->setButtonText (text);

    if (toggle != nullptr)
        toggle->setToggleState (value >= 0.5 * (info.minValue + info.maxValue), juce::dontSendNotification);
}

void ParamControl::showValueMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader (juce::String::fromUTF8 (info.name.c_str()));

    if (IsEnumParam (info))
    {
        for (std::size_t i = 0; i < info.labels.size(); ++i)
        {
            const auto option = static_cast<double> (i);
            menu.addItem (juce::String::fromUTF8 (info.labels[i].c_str()), true, std::llround (value) == (long long) i,
                          [this, option] { commit (option); });
        }
    }
    else
    {
        const auto defaultText = juce::String::fromUTF8 (FormatParamValue (info, info.defaultValue).c_str());
        menu.addItem ("Reset to " + defaultText, [this] {
            if (slider != nullptr)
                slider->setValue (info.defaultValue, juce::sendNotificationSync);
            else
                commit (info.defaultValue);
        });

        if (slider != nullptr)
        {
            menu.addItem ("Type a value...", [this] {
                auto* window = new juce::AlertWindow (juce::String::fromUTF8 (info.name.c_str()),
                                                      "Between " + juce::String (info.minValue) + " and " + juce::String (info.maxValue)
                                                          + (info.unit.empty() || info.unit == "amount" ? juce::String() : " " + juce::String (info.unit)),
                                                      juce::MessageBoxIconType::NoIcon, this);
                window->setLookAndFeel (&getLookAndFeel());
                window->addTextEditor ("value", juce::String (value, 3));
                window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                juce::Component::SafePointer<ParamControl> safeThis (this);
                window->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, window] (int result) {
                                             if (result != 1 || safeThis == nullptr || safeThis->slider == nullptr)
                                                 return;

                                             const auto typed = window->getTextEditorContents ("value").getDoubleValue();
                                             safeThis->slider->setValue (typed, juce::sendNotificationSync);
                                         }),
                                         true);
            });
        }
    }

    context.showMenu (menu, this);
}

int ParamControl::preferredHeight (int width) const
{
    if (style == Style::SliderRow || toggle != nullptr)
        return context.touch ? 48 : 40;

    // A knob sized for a thumb, not for the cell: name, knob, value.
    return kNameHeight + knobSize (context, width) + kValueHeight + 4; // + resized()'s margin
}

void ParamControl::resized()
{
    auto area = getLocalBounds().reduced (2);

    if (toggle != nullptr)
    {
        toggle->setBounds (area);
        return;
    }

    if (style == Style::Knob)
    {
        nameLabel.setBounds (area.removeFromTop (kNameHeight));

        if (picker != nullptr)
        {
            picker->setBounds (area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), context.touch ? 44 : 34)));
            return;
        }

        valueLabel.setBounds (area.removeFromBottom (kValueHeight));

        if (slider != nullptr)
            slider->setBounds (area.withSizeKeepingCentre (knobSize (context, area.getWidth()), area.getHeight()));

        return;
    }

    // A row: name, control, value.
    const int nameWidth = juce::jlimit (70, 150, area.getWidth() * 30 / 100);
    nameLabel.setBounds (area.removeFromLeft (nameWidth));

    if (picker != nullptr)
    {
        picker->setBounds (area.reduced (0, 4));
        return;
    }

    valueLabel.setBounds (area.removeFromRight (juce::jlimit (56, 90, area.getWidth() / 4)));

    if (slider != nullptr)
        slider->setBounds (area);
}

void ParamControl::paint (juce::Graphics&) {}

void ParamControl::lookAndFeelChanged()
{
    const auto& theme = context.theme;
    const bool knob = style == Style::Knob;
    nameLabel.setFont (context.font (knob ? NanoTheme::textCaption : NanoTheme::textLabel));
    nameLabel.setColour (juce::Label::textColourId, theme.textSecondary());
    valueLabel.setFont (context.font (knob ? NanoTheme::textCaption : NanoTheme::textLabel, FontWeight::medium));
    valueLabel.setColour (juce::Label::textColourId, theme.text());
}
} // namespace soundshed::nano
