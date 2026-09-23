#include "nativeui/theme/NanoLookAndFeel.h"

namespace soundshed::nano
{
namespace
{
juce::Typeface::Ptr loadTypeface (const juce::File& file)
{
    juce::MemoryBlock data;

    if (! file.existsAsFile() || ! file.loadFileAsData (data) || data.getSize() == 0)
        return nullptr;

    return juce::Typeface::createSystemTypefaceFor (data.getData(), data.getSize());
}
} // namespace

NanoLookAndFeel::NanoLookAndFeel (const NanoTheme& themeIn, const juce::File& fontsFolder)
    : theme (themeIn)
{
    // The web UI's face (core/ui/css/fonts). A variable font: JUCE draws its default
    // instance, Regular, and synthesises no bold, so emphasis here is size and colour.
    regular = loadTypeface (fontsFolder.getChildFile ("Inter-VariableFont_opsz,wght.ttf"));
    refreshColours();
}

juce::Typeface::Ptr NanoLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    if (regular != nullptr)
        return regular;

    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

juce::Font NanoLookAndFeel::font (float height, bool bold) const
{
    auto options = juce::FontOptions {}.withHeight (height);

    if (regular != nullptr)
        options = options.withTypeface (regular);

    juce::Font result (options);
    return bold ? result.boldened() : result;
}

void NanoLookAndFeel::refreshColours()
{
    const auto bg = theme.background();
    const auto panel = theme.panel();
    const auto text = theme.text();
    const auto accent = theme.accent();

    // windowBackground, widgetBackground, menuBackground, outline, defaultText, defaultFill,
    // highlightedText, highlightedFill, menuText
    setColourScheme ({ bg, panel, theme.elevated(), theme.border(), text, accent, juce::Colours::white, accent, text });

    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::backgroundColourId, bg);
    setColour (juce::TextButton::buttonColourId, theme.card());
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::TextEditor::backgroundColourId, theme.input());
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, theme.border());
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::ComboBox::backgroundColourId, theme.input());
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, theme.border());
    setColour (juce::ComboBox::arrowColourId, theme.textSecondary());
    setColour (juce::PopupMenu::backgroundColourId, theme.elevated());
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, theme.textMuted().withAlpha (0.5f));
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::AlertWindow::backgroundColourId, theme.elevated());
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, theme.border());
}

void NanoLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto ringThickness = juce::jmax (3.0f, size * 0.08f);
    const auto ringRadius = size * 0.5f - ringThickness * 0.5f;
    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const auto& t = theme.t();

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (NanoTheme::argb (t.knobRingTrack).withMultipliedAlpha (slider.isEnabled() ? 1.0f : 0.5f));
    g.strokePath (track, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (sliderPos > 0.0f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, angle, true);
        const auto start = NanoTheme::argb (t.knobRingStart);
        const auto end = NanoTheme::argb (t.knobRingEnd);
        g.setGradientFill (juce::ColourGradient (start, centre.x - ringRadius, centre.y + ringRadius,
                                                 start.interpolatedWith (end, sliderPos), centre.x + ringRadius, centre.y - ringRadius, false));

        // A light theme's ring can start at white; the accent keeps it visible.
        if (theme.isLight())
            g.setColour (theme.accent());

        g.strokePath (value, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const auto knobRadius = ringRadius - ringThickness * 1.3f;
    const auto knobBounds = juce::Rectangle<float> (knobRadius * 2.0f, knobRadius * 2.0f).withCentre (centre);
    g.setColour (NanoTheme::argb (t.controlKnobBg));
    g.fillEllipse (knobBounds);
    g.setColour (NanoTheme::argb (t.controlBorder));
    g.drawEllipse (knobBounds, 1.0f);

    const auto pointerLength = knobRadius * 0.62f;
    const juce::Point<float> tip (centre.x + pointerLength * std::sin (angle), centre.y - pointerLength * std::cos (angle));
    const juce::Point<float> base (centre.x + knobRadius * 0.15f * std::sin (angle), centre.y - knobRadius * 0.15f * std::cos (angle));
    g.setColour (NanoTheme::argb (t.controlIndicator));
    g.drawLine ({ base, tip }, juce::jmax (2.0f, size * 0.045f));
}

void NanoLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                        juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const bool horizontal = style == juce::Slider::LinearHorizontal;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float thickness = 6.0f;
    const auto track = horizontal ? juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - thickness * 0.5f, bounds.getWidth(), thickness)
                                  : juce::Rectangle<float> (bounds.getCentreX() - thickness * 0.5f, bounds.getY(), thickness, bounds.getHeight());

    g.setColour (NanoTheme::argb (theme.t().knobRingTrack).withMultipliedAlpha (slider.isEnabled() ? 1.0f : 0.5f));
    g.fillRoundedRectangle (track, thickness * 0.5f);

    auto filled = track;

    if (horizontal)
        filled = filled.withRight (sliderPos);
    else
        filled = filled.withTop (sliderPos);

    g.setColour (theme.accent().withMultipliedAlpha (slider.isEnabled() ? 1.0f : 0.5f));
    g.fillRoundedRectangle (filled, thickness * 0.5f);

    const float thumb = juce::jmin (22.0f, (horizontal ? bounds.getHeight() : bounds.getWidth()) - 4.0f);
    const auto thumbCentre = horizontal ? juce::Point<float> (sliderPos, bounds.getCentreY())
                                        : juce::Point<float> (bounds.getCentreX(), sliderPos);
    g.setColour (NanoTheme::argb (theme.t().controlKnobBg));
    g.fillEllipse (juce::Rectangle<float> (thumb, thumb).withCentre (thumbCentre));
    g.setColour (theme.accent());
    g.drawEllipse (juce::Rectangle<float> (thumb, thumb).withCentre (thumbCentre), 2.0f);
}

void NanoLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                            bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    auto colour = button.getToggleState() ? button.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;

    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.5f);
    else if (down)
        colour = colour.darker (0.15f);
    else if (highlighted)
        colour = colour.brighter (0.08f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, (float) NanoTheme::radius);

    if (! button.getToggleState() && colour.getAlpha() > 0)
    {
        g.setColour (theme.border());
        g.drawRoundedRectangle (bounds, (float) NanoTheme::radius, 1.0f);
    }
}

juce::Font NanoLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (juce::jlimit (12.0f, 16.0f, (float) buttonHeight * 0.4f));
}

juce::Font NanoLookAndFeel::getLabelFont (juce::Label& label)
{
    return font (label.getFont().getHeight());
}

juce::Font NanoLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return font (juce::jlimit (12.0f, 16.0f, (float) box.getHeight() * 0.42f));
}

juce::Font NanoLookAndFeel::getPopupMenuFont()
{
    return font (16.0f);
}

void NanoLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // A switch, as the web UI draws toggles: track and thumb, then the label.
    const auto bounds = button.getLocalBounds().toFloat();
    const float trackHeight = juce::jmin (22.0f, bounds.getHeight() - 8.0f);
    const float trackWidth = trackHeight * 1.8f;
    const auto track = juce::Rectangle<float> (bounds.getX() + 2.0f, bounds.getCentreY() - trackHeight * 0.5f, trackWidth, trackHeight);
    const bool on = button.getToggleState();

    g.setColour (on ? theme.accent() : NanoTheme::argb (theme.t().toggleBg));
    g.fillRoundedRectangle (track, trackHeight * 0.5f);

    const float thumb = trackHeight - 4.0f;
    const float thumbX = on ? track.getRight() - thumb - 2.0f : track.getX() + 2.0f;
    g.setColour (juce::Colours::white.withAlpha (highlighted ? 1.0f : 0.95f));
    g.fillEllipse (thumbX, track.getY() + 2.0f, thumb, thumb);

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.setFont (font (juce::jmin (16.0f, bounds.getHeight() * 0.45f)));
        g.drawFittedText (button.getButtonText(),
                          bounds.withTrimmedLeft (trackWidth + 10.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1);
    }
}

void NanoLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                                     bool vertical, int thumbStart, int thumbSize, bool over, bool down)
{
    juce::ignoreUnused (x, y);
    const auto thumb = vertical ? juce::Rectangle<int> (0, thumbStart, width, thumbSize)
                                : juce::Rectangle<int> (thumbStart, 0, thumbSize, height);
    g.setColour (bar.findColour (juce::ScrollBar::thumbColourId).withMultipliedAlpha (over || down ? 1.0f : 0.7f));
    g.fillRoundedRectangle (thumb.toFloat().reduced (1.0f), 3.0f);
}
} // namespace soundshed::nano
