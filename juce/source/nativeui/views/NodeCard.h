#pragma once

#include "nativeui/NanoContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
struct NodeCardLook
{
    bool selected = false;
    bool compact = false;        // the mini strip: icon over a short name
    bool clipping = false;       // the node clipped recently
    bool missingResource = false;
};

/// Draws one chain node as the web UI's node card does: the category's colour and icon, the
/// node's name (its model or IR when it plays one), and dimmed with "OFF" when bypassed.
void paintNodeCard (juce::Graphics& g, NanoContext& context, const guitarfx::GraphNode& node,
                    juce::Rectangle<float> bounds, const NodeCardLook& look);

/// The corner of a card that toggles bypass when tapped.
[[nodiscard]] juce::Rectangle<float> nodeCardBypassArea (juce::Rectangle<float> bounds, bool compact);

/// The node's level reading from the meter feed, or null when it did not run.
[[nodiscard]] const guitarfx::uiclient::LevelReading* nodeLevel (const NanoContext& context, const std::string& nodeId);
} // namespace soundshed::nano
