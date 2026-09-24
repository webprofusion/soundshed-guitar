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

FontWeight weightOf (const juce::Font& font)
{
    const auto style = font.getTypefaceStyle();

    if (font.isBold() || style.containsIgnoreCase ("semibold"))
        return FontWeight::semibold;

    return style.containsIgnoreCase ("medium") ? FontWeight::medium : FontWeight::regular;
}
} // namespace

NanoLookAndFeel::NanoLookAndFeel (const NanoTheme& themeIn, const juce::File& fontsFolder)
    : theme (themeIn)
{
    // Static cuts of the web UI's Inter (tools/gen-nano-fonts.mjs). JUCE cannot choose a
    // variable font's weight: given one, Windows draws its first instance, Thin.
    typefaces = { loadTypeface (fontsFolder.getChildFile ("Inter-Regular.ttf")),
                  loadTypeface (fontsFolder.getChildFile ("Inter-Medium.ttf")),
                  loadTypeface (fontsFolder.getChildFile ("Inter-SemiBold.ttf")) };

    // Text that does not fit ends in an ellipsis. JUCE's default squeezes it to 70% width
    // first, which reads as a different, cramped face.
    juce::Font::setDefaultMinimumHorizontalScaleFactor (1.0f);

    refreshColours();
}

juce::Typeface::Ptr NanoLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    if (const auto& typeface = typefaces[(std::size_t) weightOf (f)]; typeface != nullptr)
        return typeface;

    if (typefaces[0] != nullptr)
        return typefaces[0];

    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

juce::Font NanoLookAndFeel::font (float size, FontWeight weight) const
{
    // The size is the em size, as CSS gives it, not JUCE's legacy height (ascent plus
    // descent), which for Inter draws text a fifth smaller than the number says.
    auto options = juce::FontOptions {}.withPointHeight (size);
    auto typeface = typefaces[(std::size_t) weight] != nullptr ? typefaces[(std::size_t) weight] : typefaces[0];

    if (typeface != nullptr)
        return juce::Font (options.withTypeface (typeface));

    return juce::Font (weight == FontWeight::regular ? options : options.withStyle ("Bold"));
}

void NanoLookAndFeel::refreshColours()
{
    const auto bg = theme.background();
    const auto panel = theme.panel();
    const auto text = theme.text();
    const auto accent = theme.accent();

    // windowBackground, widgetBackground, menuBackground, outline, defaultText, defaultFill,
    // highlightedText, highlightedFill, menuText
    setColourScheme ({ bg, panel, theme.elevated(), theme.border(), text, theme.accentFill(), theme.onAccent(), theme.selectedFill(), text });

    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::backgroundColourId, bg);
    setColour (juce::TextButton::buttonColourId, theme.card());
    setColour (juce::TextButton::buttonOnColourId, theme.accentFill());
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, theme.onAccent());
    setColour (juce::Label::textColourId, text);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxTextColourId, theme.textSecondary());
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.35f));
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::TextEditor::backgroundColourId, theme.input());
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, theme.borderStrong().interpolatedWith (theme.border(), 0.5f));
    setColour (juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.7f));
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.3f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::ComboBox::backgroundColourId, theme.input());
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, theme.border());
    setColour (juce::ComboBox::arrowColourId, theme.textMuted());
    setColour (juce::PopupMenu::backgroundColourId, theme.elevated());
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, theme.textMuted());
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme.selectedFill());
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, theme.textMuted().withAlpha (0.45f));
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::AlertWindow::backgroundColourId, theme.elevated());
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, theme.border());
}

void NanoLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto ringThickness = juce::jlimit (2.5f, 4.0f, size * 0.06f);
    const auto ringRadius = size * 0.5f - ringThickness * 0.5f;
    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const auto enabledAlpha = slider.isEnabled() ? 1.0f : 0.5f;
    const auto& t = theme.t();
    const juce::PathStrokeType ringStroke (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (NanoTheme::argb (t.knobRingTrack).withMultipliedAlpha (enabledAlpha));
    g.strokePath (track, ringStroke);

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

        g.strokePath (value, ringStroke);
    }

    // The cap: a flat disc a little inside the ring, lit faintly from above.
    const auto capRadius = ringRadius - ringThickness * 0.5f - juce::jmax (2.5f, size * 0.07f);
    const auto cap = juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre);
    const auto capColour = NanoTheme::argb (t.controlKnobBg);
    g.setGradientFill (juce::ColourGradient (capColour.brighter (theme.isLight() ? 0.02f : 0.12f), centre.x, cap.getY(),
                                             capColour.darker (theme.isLight() ? 0.04f : 0.1f), centre.x, cap.getBottom(), false));
    g.fillEllipse (cap);
    g.setColour (NanoTheme::argb (t.controlBorder).withMultipliedAlpha (0.8f));
    g.drawEllipse (cap.reduced (0.5f), 1.0f);

    // The pointer: from near the middle to near the edge of the cap.
    const auto direction = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour (NanoTheme::argb (t.controlIndicator).withMultipliedAlpha (enabledAlpha));
    g.drawLine ({ centre + direction * (capRadius * 0.3f), centre + direction * (capRadius * 0.82f) }, juce::jmax (1.5f, size * 0.035f));
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
    const float thickness = 4.0f;
    const auto enabledAlpha = slider.isEnabled() ? 1.0f : 0.5f;
    const auto track = horizontal ? juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - thickness * 0.5f, bounds.getWidth(), thickness)
                                  : juce::Rectangle<float> (bounds.getCentreX() - thickness * 0.5f, bounds.getY(), thickness, bounds.getHeight());

    g.setColour (NanoTheme::argb (theme.t().knobRingTrack).brighter (0.1f).withMultipliedAlpha (enabledAlpha));
    g.fillRoundedRectangle (track, thickness * 0.5f);

    auto filled = horizontal ? track.withRight (sliderPos) : track.withTop (sliderPos);
    g.setColour (theme.accentFill().withMultipliedAlpha (enabledAlpha));
    g.fillRoundedRectangle (filled, thickness * 0.5f);

    // The thumb: a plain disc with a hairline edge, not a second block of accent.
    const float thumb = juce::jmin (16.0f, (horizontal ? bounds.getHeight() : bounds.getWidth()) - 4.0f);
    const auto thumbCentre = horizontal ? juce::Point<float> (sliderPos, bounds.getCentreY())
                                        : juce::Point<float> (bounds.getCentreX(), sliderPos);
    const auto thumbArea = juce::Rectangle<float> (thumb, thumb).withCentre (thumbCentre);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillEllipse (thumbArea.translated (0.0f, 1.0f));
    g.setColour (theme.isLight() ? juce::Colours::white : NanoTheme::argb (theme.t().controlIndicator));
    g.fillEllipse (thumbArea);
    g.setColour (NanoTheme::argb (theme.t().controlBorder));
    g.drawEllipse (thumbArea.reduced (0.5f), 1.0f);
}

juce::Label* NanoLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (font (NanoTheme::textLabel, FontWeight::medium));
    label->setJustificationType (juce::Justification::centredRight);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::textColourId, theme.textSecondary());
    return label;
}

void NanoLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                            bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = button.getToggleState();
    auto colour = on ? button.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;

    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.5f);
    else if (down)
        colour = on ? colour.darker (0.12f) : colour.overlaidWith (theme.pressedFill());
    else if (highlighted)
        colour = on ? colour.brighter (0.06f) : colour.overlaidWith (theme.hoverFill());

    const auto radius = (float) NanoTheme::controlRadius;
    g.setColour (colour);
    g.fillRoundedRectangle (bounds, radius);

    if (! on && colour.getAlpha() > 0)
    {
        g.setColour (theme.border());
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
    }
}

juce::Font NanoLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (juce::jlimit (12.0f, NanoTheme::textBody, (float) buttonHeight * 0.4f), FontWeight::medium);
}

juce::Font NanoLookAndFeel::getLabelFont (juce::Label& label)
{
    // Keep the weight a view gave the label; only a font without Inter gets it here.
    const auto labelFont = label.getFont();

    for (const auto& typeface : typefaces)
        if (typeface != nullptr && labelFont.getTypefacePtr() == typeface)
            return labelFont;

    return font (labelFont.getHeightInPoints(), weightOf (labelFont));
}

juce::Font NanoLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return font (juce::jlimit (12.0f, NanoTheme::textBody, (float) box.getHeight() * 0.42f));
}

juce::Font NanoLookAndFeel::getPopupMenuFont()
{
    return font (NanoTheme::textBody);
}

juce::Font NanoLookAndFeel::getAlertWindowTitleFont()
{
    return font (NanoTheme::textHeading, FontWeight::semibold);
}

juce::Font NanoLookAndFeel::getAlertWindowMessageFont()
{
    return font (NanoTheme::textBody);
}

juce::Font NanoLookAndFeel::getAlertWindowFont()
{
    return font (NanoTheme::textLabel);
}

void NanoLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    const auto radius = (float) NanoTheme::controlRadius;

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, radius);

    if (box.isMouseOver (true))
    {
        g.setColour (theme.hoverFill());
        g.fillRoundedRectangle (bounds, radius);
    }

    g.setColour (box.hasKeyboardFocus (true) ? theme.accent().withAlpha (0.7f) : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    // A small chevron, not the stock heavy arrow.
    const auto arrow = juce::Rectangle<float> ((float) width - 26.0f, 0.0f, 14.0f, (float) height).withSizeKeepingCentre (9.0f, 5.0f);
    juce::Path chevron;
    chevron.startNewSubPath (arrow.getX(), arrow.getY());
    chevron.lineTo (arrow.getCentreX(), arrow.getBottom());
    chevron.lineTo (arrow.getRight(), arrow.getY());
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void NanoLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 34, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void NanoLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), (float) NanoTheme::controlRadius);
}

void NanoLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;

    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
    g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), (float) NanoTheme::controlRadius,
                            focused ? 1.5f : 1.0f);
}

void NanoLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // A switch, as the web UI draws toggles: track and thumb, then the label.
    const auto bounds = button.getLocalBounds().toFloat();
    const float trackHeight = juce::jmin (20.0f, bounds.getHeight() - 8.0f);
    const float trackWidth = trackHeight * 1.75f;
    const auto track = juce::Rectangle<float> (bounds.getX() + 2.0f, bounds.getCentreY() - trackHeight * 0.5f, trackWidth, trackHeight);
    const bool on = button.getToggleState();
    const auto enabledAlpha = button.isEnabled() ? 1.0f : 0.5f;

    auto trackColour = on ? theme.accentFill() : NanoTheme::argb (theme.t().toggleBg);

    if (highlighted)
        trackColour = trackColour.brighter (0.06f);

    g.setColour (trackColour.withMultipliedAlpha (enabledAlpha));
    g.fillRoundedRectangle (track, trackHeight * 0.5f);

    const float thumb = trackHeight - 4.0f;
    const float thumbX = on ? track.getRight() - thumb - 2.0f : track.getX() + 2.0f;
    g.setColour (juce::Colours::white.withAlpha (0.96f * enabledAlpha));
    g.fillEllipse (thumbX, track.getY() + 2.0f, thumb, thumb);

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (enabledAlpha));
        g.setFont (font (juce::jmin (NanoTheme::textBody, bounds.getHeight() * 0.42f)));
        g.drawFittedText (button.getButtonText(), bounds.withTrimmedLeft (trackWidth + 10.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1);
    }
}

void NanoLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                                     bool vertical, int thumbStart, int thumbSize, bool over, bool down)
{
    juce::ignoreUnused (x, y);
    const auto thumb = vertical ? juce::Rectangle<int> (0, thumbStart, width, thumbSize)
                                : juce::Rectangle<int> (thumbStart, 0, thumbSize, height);
    const auto slim = vertical ? thumb.toFloat().reduced (1.5f, 1.0f) : thumb.toFloat().reduced (1.0f, 1.5f);
    g.setColour (bar.findColour (juce::ScrollBar::thumbColourId).withMultipliedAlpha (over || down ? 1.0f : 0.6f));
    g.fillRoundedRectangle (slim, juce::jmin (slim.getWidth(), slim.getHeight()) * 0.5f);
}
} // namespace soundshed::nano
