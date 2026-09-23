#pragma once

#include "nativeui/NanoContext.h"
#include "uiclient/ClientState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace soundshed::nano
{
/// One effect parameter as a control, drawn the way its catalog entry asks: a switch for a
/// toggle, a picker for an enum, otherwise a knob (landscape grids) or a slider row (portrait
/// lists). Travel follows the parameter's taper through the engine's own ParamTaper.h, and the
/// value reads as the web UI's knob shows it (uiclient::FormatParamValue).
///
/// Knob behaviour matches the web UI's: vertical drag over 200 px for the full range, a
/// double tap (or double click) back to the default, the mouse wheel on desktop. A long press
/// opens a menu to type a value or reset.
class ParamControl final : public juce::Component
{
public:
    enum class Style
    {
        Knob,
        SliderRow
    };

    ParamControl (NanoContext& context, const guitarfx::uiclient::EffectParamInfo& info, Style style);
    ~ParamControl() override;

    /// Shows a value from the engine without calling onChange.
    void setValue (double value);

    [[nodiscard]] double getValue() const noexcept { return value; }
    [[nodiscard]] const guitarfx::uiclient::EffectParamInfo& getInfo() const noexcept { return info; }

    /// Called with the new value while the user moves the control.
    std::function<void (double)> onChange;

    /// Replaces the catalog formatting, for the few controls the web UI formats its own way.
    void setFormatter (std::function<juce::String (double)> formatter);

    /// The height this control wants at a given width (knobs are square-ish, rows are one line).
    [[nodiscard]] int preferredHeight (int width) const;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void lookAndFeelChanged() override;

private:
    class TaperedSlider;

    void showValueMenu();
    void commit (double newValue);
    void refreshText();

    NanoContext& context;
    guitarfx::uiclient::EffectParamInfo info;
    Style style;
    double value = 0.0;
    std::function<juce::String (double)> customFormatter;

    std::unique_ptr<TaperedSlider> slider;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::TextButton> picker;
    juce::Label nameLabel;
    juce::Label valueLabel;
};
} // namespace soundshed::nano
