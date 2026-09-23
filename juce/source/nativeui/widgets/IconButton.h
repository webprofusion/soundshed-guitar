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

    /// Draws a filled accent background while on, as the web UI's active state.
    void setHighlightWhenOn (bool shouldHighlight) { highlightWhenOn = shouldHighlight; }

    /// A plain, frameless button (toolbars) rather than a bordered one.
    void setFlat (bool shouldBeFlat) { flat = shouldBeFlat; repaint(); }

    /// The icon above a small label (navigation), rather than beside it.
    void setStacked (bool shouldStack) { stacked = shouldStack; repaint(); }

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
    bool highlightWhenOn = true;
    bool flat = false;
    bool stacked = false;
    bool longPressFired = false;
};
} // namespace soundshed::nano
