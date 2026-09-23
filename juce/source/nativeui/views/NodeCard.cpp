#include "nativeui/views/NodeCard.h"

#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"

namespace soundshed::nano
{
using namespace guitarfx::uiclient;

juce::Rectangle<float> nodeCardBypassArea (juce::Rectangle<float> bounds, bool compact)
{
    const float size = compact ? 18.0f : 26.0f;
    return { bounds.getRight() - size - 2.0f, bounds.getY() + 2.0f, size, size };
}

const LevelReading* nodeLevel (const NanoContext& context, const std::string& nodeId)
{
    const auto& state = context.state();
    const auto key = state.activePresetId + "::" + nodeId;
    const auto it = state.telemetry.nodes.find (key);
    return it == state.telemetry.nodes.end() || it->second.channelCount == 0 ? nullptr : &it->second;
}

void paintNodeCard (juce::Graphics& g, NanoContext& context, const guitarfx::GraphNode& node,
                    juce::Rectangle<float> bounds, const NodeCardLook& look)
{
    const auto& theme = context.theme;
    const auto& state = context.state();
    const auto category = NodeCategory (state, node);
    const auto categoryColour = juce::Colour (context.presentation.CategoryColour (category));
    const bool boundary = IsBoundaryNode (node);
    const bool bypassed = ! node.enabled && ! boundary;
    const float radius = look.compact ? 8.0f : 10.0f;

    g.setColour (theme.card());
    g.fillRoundedRectangle (bounds, radius);

    // The category's colour as a band along the top, as the web UI's node classes tint cards.
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (bounds, radius);
        g.reduceClipRegion (clip);
        g.setColour (categoryColour.withAlpha (bypassed ? 0.25f : 0.85f));
        g.fillRect (bounds.withHeight (look.compact ? 3.0f : 4.0f));
    }

    g.setColour (look.selected ? theme.accent() : theme.border());
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, look.selected ? 2.0f : 1.0f);

    const auto contentAlpha = bypassed ? 0.45f : 1.0f;
    auto area = bounds.reduced (look.compact ? 4.0f : 8.0f, look.compact ? 5.0f : 8.0f);
    const auto icon = boundary ? juce::String (node.id == kInputNodeId || node.type == guitarfx::kNodeTypeInput ? "guitar" : "output")
                               : juce::String (context.presentation.IconFor (node.type, category));
    const auto name = juce::String::fromUTF8 (NodeDisplayName (state, node).c_str());

    if (look.compact)
    {
        const float iconSize = juce::jmin (22.0f, area.getHeight() * 0.5f);
        context.icons->draw (g, icon, area.removeFromTop (iconSize + 2.0f).withSizeKeepingCentre (iconSize, iconSize),
                             categoryColour.interpolatedWith (theme.text(), 0.35f).withMultipliedAlpha (contentAlpha));
        g.setColour (theme.text().withMultipliedAlpha (contentAlpha));
        g.setFont (context.font (11.5f));
        g.drawFittedText (name, area.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
    else
    {
        const float iconSize = 26.0f;
        auto top = area.removeFromTop (iconSize);
        context.icons->draw (g, icon, top.removeFromLeft (iconSize),
                             categoryColour.interpolatedWith (theme.text(), 0.35f).withMultipliedAlpha (contentAlpha));

        const auto badge = NodeArchitectureBadge (state, node);

        if (! badge.empty())
        {
            g.setFont (context.font (11.0f));
            const auto badgeArea = top.removeFromLeft (30.0f).reduced (4.0f, 5.0f);
            g.setColour (theme.textMuted().withAlpha (0.25f));
            g.fillRoundedRectangle (badgeArea, 3.0f);
            g.setColour (theme.textSecondary());
            g.drawText (juce::String (badge), badgeArea, juce::Justification::centred);
        }

        area.removeFromTop (4.0f);
        g.setColour (theme.text().withMultipliedAlpha (contentAlpha));
        g.setFont (context.font (14.0f));
        g.drawFittedText (name, area.removeFromTop (area.getHeight() * 0.6f).toNearestInt(),
                          juce::Justification::topLeft, 2, 0.8f);

        const auto* type = state.FindEffectType (node.type);
        const auto typeName = boundary ? juce::String() : juce::String::fromUTF8 (type != nullptr ? type->name.c_str() : node.type.c_str());

        g.setColour (theme.textMuted().withMultipliedAlpha (contentAlpha));
        g.setFont (context.font (11.5f));
        g.drawFittedText (bypassed ? "OFF" : typeName, area.toNearestInt(), juce::Justification::bottomLeft, 1, 0.8f);
    }

    if (! boundary)
    {
        // The bypass LED: lit while the effect runs.
        const auto led = nodeCardBypassArea (bounds, look.compact).reduced (look.compact ? 5.0f : 7.0f);
        g.setColour (bypassed ? theme.textMuted().withAlpha (0.4f) : theme.success());
        g.fillEllipse (led);
    }

    if (look.clipping)
    {
        g.setColour (theme.error());
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withPosition (bounds.getX() + 4.0f, bounds.getBottom() - 11.0f));
    }

    if (look.missingResource)
    {
        g.setColour (theme.warning());
        g.setFont (context.font (13.0f));
        g.drawText ("!", bounds.withTrimmedRight (bounds.getWidth() - 18.0f).withTrimmedTop (bounds.getHeight() - 20.0f),
                    juce::Justification::centred);
    }
}
} // namespace soundshed::nano
