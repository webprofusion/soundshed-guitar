#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>
#include <vector>

namespace soundshed::nano
{
/// A component that draws several tappable things itself, rather than as child components
/// (the chain's cards, "+" buttons and bypass lights), names them so the debug server can
/// list and press them like any other control: "node:amp_0", "add:amp_0", "bypass:amp_0".
class NamedTargets
{
public:
    virtual ~NamedTargets() = default;

    /// Each target's name and its bounds in the component's own coordinates.
    [[nodiscard]] virtual std::vector<std::pair<juce::String, juce::Rectangle<int>>> namedTargets() const = 0;
};
} // namespace soundshed::nano
