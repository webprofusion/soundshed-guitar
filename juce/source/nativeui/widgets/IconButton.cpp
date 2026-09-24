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
    const auto radius = (float) (stacked ? NanoTheme::radius : NanoTheme::controlRadius);

    // The on state is a faint neutral fill, or the accent's tint for an accent button. A
    // solid accent block is kept for the one control that confirms (see NanoLookAndFeel).
    juce::Colour fill = primary ? theme.accentFill() : (flat ? juce::Colours::transparentBlack : theme.card());

    if (on && ! primary)
        fill = fill.overlaidWith (accentWhenOn ? theme.accentTint() : theme.selectedFill());

    if (primary && (down || highlighted))
        fill = down ? fill.darker (0.12f) : fill.brighter (0.06f);
    else if (down)
        fill = fill.overlaidWith (theme.pressedFill());
    else if (highlighted)
        fill = fill.overlaidWith (theme.hoverFill());

    if (fill.getAlpha() > 0)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (bounds, radius);
    }

    if (! flat && ! primary)
    {
        g.setColour (on ? (accentWhenOn ? theme.accent().withAlpha (0.5f) : theme.text().withAlpha (0.16f)) : theme.border());
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
    }

    // Content is secondary until it is on or under the pointer: a bar of bright icons and
    // labels all at full strength is what made the toolbars loud.
    const float alpha = isEnabled() ? 1.0f : 0.4f;
    const auto restColour = stacked ? theme.textMuted() : theme.textSecondary();
    const auto textColour = (primary ? theme.onAccent()
                                     : (on ? (accentWhenOn ? theme.accent() : theme.text()) : (highlighted || down ? theme.text() : restColour)))
                                .withMultipliedAlpha (alpha);
    const auto iconTint = iconColour.value_or (textColour).withMultipliedAlpha (iconColour ? alpha : 1.0f);

    if (stacked)
    {
        // The navigation: the icon over a small label.
        auto content = bounds.reduced (4.0f, 6.0f);
        const float size = iconSize.value_or (NanoTheme::iconNav);
        const float labelHeight = 14.0f;
        const float top = content.getY() + juce::jmax (0.0f, (content.getHeight() - size - 4.0f - labelHeight) * 0.5f);
        context.icons->draw (g, icon, juce::Rectangle<float> (size, size).withCentre ({ content.getCentreX(), top + size * 0.5f }), iconTint);

        g.setColour (textColour);
        g.setFont (context.font (NanoTheme::textOverline, on ? FontWeight::semibold : FontWeight::medium));
        g.drawFittedText (text, juce::Rectangle<float> (content.getX(), top + size + 4.0f, content.getWidth(), labelHeight).toNearestInt(),
                          juce::Justification::centred, 1);
        return;
    }

    auto content = bounds.reduced (8.0f, 2.0f);
    const auto font = context.font (bounds.getHeight() >= 38.0f ? NanoTheme::textBody
                                                                : (bounds.getHeight() >= 26.0f ? NanoTheme::textLabel : NanoTheme::textCaption),
                                    FontWeight::medium);

    if (icon.isNotEmpty() && text.isEmpty())
    {
        const float size = juce::jmin (iconSize.value_or (NanoTheme::iconSize), content.getHeight(), bounds.getWidth() - 4.0f);
        context.icons->draw (g, icon, bounds.withSizeKeepingCentre (size, size), iconTint);
    }
    else
    {
        // An icon and its label as one group, the icon a gap before the text.
        const float size = icon.isNotEmpty() ? juce::jmin (iconSize.value_or (NanoTheme::iconSmall), content.getHeight()) : 0.0f;
        const float gapWidth = icon.isNotEmpty() ? 7.0f : 0.0f;
        // A little over the measured width: drawFittedText lays out on whole pixels, and a
        // label exactly its own width can come out a pixel short and end in an ellipsis.
        const float textWidth = std::ceil (juce::GlyphArrangement::getStringWidth (font, text)) + 4.0f;
        const float groupWidth = juce::jmin (content.getWidth(), size + gapWidth + textWidth);
        auto group = alignLeft ? content.withWidth (groupWidth) : content.withSizeKeepingCentre (groupWidth, content.getHeight());

        if (icon.isNotEmpty())
            context.icons->draw (g, icon, group.removeFromLeft (size).withSizeKeepingCentre (size, size), iconTint);

        group.removeFromLeft (gapWidth);
        g.setColour (textColour);
        g.setFont (font);
        g.drawFittedText (text, group.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    if (badge.isNotEmpty())
    {
        g.setFont (context.font (NanoTheme::textOverline, FontWeight::semibold));
        const float badgeWidth = juce::jmax (16.0f, juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), badge) + 8.0f);
        const auto badgeArea = juce::Rectangle<float> (badgeWidth, 16.0f).withPosition (bounds.getRight() - badgeWidth, bounds.getY());
        g.setColour (theme.accentFill());
        g.fillRoundedRectangle (badgeArea, 8.0f);
        g.setColour (theme.onAccent());
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
