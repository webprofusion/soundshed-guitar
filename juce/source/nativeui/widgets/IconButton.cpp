#include "nativeui/widgets/IconButton.h"

namespace soundshed::nano
{
namespace
{
constexpr int kLongPressMs = 550;
} // namespace

IconButton::IconButton (NanoContext& contextIn, const juce::String& componentId, const juce::String& iconIn,
                        const juce::String& textIn)
    : juce::Button (componentId), context (contextIn), icon (iconIn), text (textIn)
{
    setComponentID (componentId);
    setTooltip (textIn);
}

void IconButton::setIcon (const juce::String& newIcon)
{
    if (icon != newIcon)
    {
        icon = newIcon;
        repaint();
    }
}

void IconButton::setText (const juce::String& newText)
{
    if (text != newText)
    {
        text = newText;
        repaint();
    }
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& theme = context.theme;
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    const bool on = getToggleState() && highlightWhenOn;

    juce::Colour fill = flat ? juce::Colours::transparentBlack : theme.card();

    if (on)
        fill = theme.accent();
    else if (down)
        fill = theme.elevated().darker (0.1f);
    else if (highlighted)
        fill = flat ? theme.text().withAlpha (0.08f) : theme.elevated();

    if (fill.getAlpha() > 0)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (bounds, (float) NanoTheme::radius);
    }

    if (! flat && ! on)
    {
        g.setColour (theme.border());
        g.drawRoundedRectangle (bounds, (float) NanoTheme::radius, 1.0f);
    }

    const float alpha = isEnabled() ? 1.0f : 0.4f;
    const auto contentColour = (on ? juce::Colours::white : iconColour.value_or (theme.text())).withMultipliedAlpha (alpha);
    auto content = bounds.reduced (6.0f, 4.0f);

    if (stacked)
    {
        const float iconSize = juce::jmin (24.0f, content.getHeight() * 0.5f);
        context.icons->draw (g, icon, content.removeFromTop (content.getHeight() * 0.62f).withTrimmedTop (2.0f)
                                          .withSizeKeepingCentre (iconSize, iconSize), contentColour);
        g.setColour (contentColour);
        g.setFont (context.font (11.5f));
        g.drawFittedText (text, content.toNearestInt(), juce::Justification::centredTop, 1, 0.8f);
        return;
    }

    if (icon.isNotEmpty())
    {
        const float iconSize = juce::jmin (22.0f, content.getHeight());
        auto iconArea = text.isEmpty() ? content : content.removeFromLeft (iconSize + 4.0f);
        context.icons->draw (g, icon, iconArea.withSizeKeepingCentre (iconSize, iconSize), contentColour);
    }

    if (text.isNotEmpty())
    {
        g.setColour (contentColour);
        g.setFont (context.font (juce::jlimit (12.0f, 16.0f, bounds.getHeight() * 0.36f)));
        g.drawFittedText (text, content.toNearestInt(), icon.isEmpty() ? juce::Justification::centred
                                                                       : juce::Justification::centredLeft, 1, 0.8f);
    }

    if (badge.isNotEmpty())
    {
        g.setFont (context.font (11.0f));
        const float badgeWidth = juce::jmax (16.0f, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), badge) + 8.0f);
        const auto badgeArea = juce::Rectangle<float> (badgeWidth, 16.0f).withPosition (bounds.getRight() - badgeWidth, bounds.getY());
        g.setColour (theme.accent());
        g.fillRoundedRectangle (badgeArea, 8.0f);
        g.setColour (juce::Colours::white);
        g.drawText (badge, badgeArea, juce::Justification::centred);
    }
}

void IconButton::mouseDown (const juce::MouseEvent& e)
{
    longPressFired = false;

    if (e.mods.isPopupMenu() && onLongPress)
    {
        longPressFired = true;
        onLongPress();
        return;
    }

    if (onLongPress)
        startTimer (kLongPressMs);

    juce::Button::mouseDown (e);
}

void IconButton::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 8)
        stopTimer();

    juce::Button::mouseDrag (e);
}

void IconButton::mouseUp (const juce::MouseEvent& e)
{
    stopTimer();

    // A long press is its own action: the release that ends it must not click as well.
    if (longPressFired || e.mods.isPopupMenu())
    {
        longPressFired = false;
        setState (juce::Button::buttonNormal);
        return;
    }

    // A press held long enough ends as a long press even if the timer was starved.
    if (onLongPress && e.getLengthOfMousePress() >= kLongPressMs && e.getDistanceFromDragStart() < 8)
    {
        setState (juce::Button::buttonNormal);
        onLongPress();
        return;
    }

    juce::Button::mouseUp (e);
}

void IconButton::timerCallback()
{
    stopTimer();

    if (isMouseButtonDown() && onLongPress)
    {
        longPressFired = true;
        onLongPress();
    }
}
} // namespace soundshed::nano
