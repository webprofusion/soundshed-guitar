#pragma once

#include "nativeui/NanoContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace soundshed::nano
{
/// A button showing one of the web UI's icons, a text, or both; with a long press (or a
/// right click) as a second action, since a touch screen has no right button.
class IconButton : public juce::Button,
                   private juce::Timer
{
public:
    IconButton (NanoContext& context, const juce::String& componentId, const juce::String& icon = {},
                const juce::String& text = {});

    void setIcon (const juce::String& newIcon);
    void setText (const juce::String& newText);
    [[nodiscard]] const juce::String& getText() const noexcept { return text; }

    /// Whether the on state is drawn at all (a faint neutral fill and brighter content).
    void setHighlightWhenOn (bool shouldHighlight) { highlightWhenOn = shouldHighlight; }

    /// Draws the on state in the accent's tint instead: for the one thing in a group that is
    /// playing (the scene), where the neutral selection would not read from a pedalboard away.
    void setAccentWhenOn (bool shouldUseAccent) { accentWhenOn = shouldUseAccent; repaint(); }

    /// The button that confirms (OK, Save, Delete): filled with the accent, whatever its state.
    void setPrimary (bool shouldBePrimary) { primary = shouldBePrimary; repaint(); }

    /// A plain, frameless button (toolbars) rather than a bordered one.
    void setFlat (bool shouldBeFlat) { flat = shouldBeFlat; repaint(); }

    /// The icon above a small label (navigation), rather than beside it.
    void setStacked (bool shouldStack) { stacked = shouldStack; repaint(); }

    /// Sets the icon's size instead of the theme's for this kind of button.
    void setIconSize (float size) { iconSize = size; repaint(); }

    /// Puts the icon and text at the left, as a row in a list, rather than centred.
    void setAlignLeft (bool shouldAlignLeft) { alignLeft = shouldAlignLeft; repaint(); }

    void setIconColour (std::optional<juce::Colour> colour) { iconColour = colour; repaint(); }
    void setBadge (const juce::String& newBadge) { badge = newBadge; repaint(); }

    std::function<void()> onLongPress;

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

protected:
    NanoContext& context;

private:
    void timerCallback() override;

    juce::String icon;
    juce::String text;
    juce::String badge;
    std::optional<juce::Colour> iconColour;
    std::optional<float> iconSize;
    bool highlightWhenOn = true;
    bool accentWhenOn = false;
    bool primary = false;
    bool alignLeft = false;
    bool flat = false;
    bool stacked = false;
    bool longPressFired = false;
};
} // namespace soundshed::nano
