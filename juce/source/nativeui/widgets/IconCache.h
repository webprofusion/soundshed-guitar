#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>

namespace soundshed::nano
{
/// The web UI's icons (core/ui/images/icons/<name>.svg), drawn in any colour.
///
/// The SVGs are single-colour strokes that the web UI tints through a CSS mask. The same
/// here: each is rendered once per pixel size into an alpha image, then filled with the
/// colour asked for. An icon that is missing or that JUCE's SVG parser cannot read draws
/// nothing rather than failing.
class IconCache
{
public:
    explicit IconCache (juce::File iconsFolder);

    void draw (juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area, juce::Colour colour);

    /// Whether the icon exists and parses (phase-0 check of every icon the UI names).
    [[nodiscard]] bool canLoad (const juce::String& name);

private:
    [[nodiscard]] juce::Drawable* drawableFor (const juce::String& name);

    juce::File folder;
    std::map<juce::String, std::unique_ptr<juce::Drawable>> drawables;
    std::map<juce::String, juce::Image> rendered; // "<name>@<size>"
};
} // namespace soundshed::nano
