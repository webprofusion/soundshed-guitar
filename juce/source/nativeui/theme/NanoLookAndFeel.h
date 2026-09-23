#pragma once

#include "nativeui/theme/NanoTheme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
/// JUCE's stock widgets drawn in the Soundshed look: Inter, the theme's colours, rounded
/// controls, and knobs with the web UI's value ring.
class NanoLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    NanoLookAndFeel (const NanoTheme& theme, const juce::File& fontsFolder);

    /// Re-reads the theme's colours into JUCE's colour ids.
    void refreshColours();

    [[nodiscard]] juce::Font font (float height, bool bold = false) const;

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    int getDefaultScrollbarWidth() override { return 6; }

private:
    const NanoTheme& theme;
    juce::Typeface::Ptr regular;
};
} // namespace soundshed::nano
