#include "nativeui/widgets/IconCache.h"

namespace soundshed::nano
{
IconCache::IconCache (juce::File iconsFolder) : folder (std::move (iconsFolder))
{
}

juce::Drawable* IconCache::drawableFor (const juce::String& name)
{
    if (const auto it = drawables.find (name); it != drawables.end())
        return it->second.get();

    std::unique_ptr<juce::Drawable> drawable;
    const auto file = folder.getChildFile (name + ".svg");

    // Some icons stroke with "currentColor", which JUCE's SVG parser leaves transparent. Only
    // the alpha channel is used (the colour comes from draw()), so any solid colour will do.
    if (file.existsAsFile())
        if (auto xml = juce::parseXML (file.loadFileAsString().replace ("currentColor", "#000000")))
            drawable = juce::Drawable::createFromSVG (*xml);

    auto* raw = drawable.get();
    drawables[name] = std::move (drawable);
    return raw;
}

bool IconCache::canLoad (const juce::String& name)
{
    return drawableFor (name) != nullptr;
}

void IconCache::draw (juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area, juce::Colour colour)
{
    if (name.isEmpty() || area.isEmpty())
        return;

    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int pixels = juce::jmax (1, juce::roundToInt (juce::jmin (area.getWidth(), area.getHeight()) * scale));
    const auto key = name + "@" + juce::String (pixels);
    auto it = rendered.find (key);

    if (it == rendered.end())
    {
        auto* drawable = drawableFor (name);

        if (drawable == nullptr)
            return;

        juce::Image image (juce::Image::ARGB, pixels, pixels, true);
        {
            juce::Graphics imageGraphics (image);
            drawable->drawWithin (imageGraphics, juce::Rectangle<float> ((float) pixels, (float) pixels),
                                  juce::RectanglePlacement::centred, 1.0f);
        }

        it = rendered.emplace (key, image).first;
    }

    const auto side = juce::jmin (area.getWidth(), area.getHeight());
    const auto target = juce::Rectangle<float> (side, side).withCentre (area.getCentre());
    g.setColour (colour);
    g.drawImage (it->second, target, juce::RectanglePlacement::stretchToFit, true);
}
} // namespace soundshed::nano
