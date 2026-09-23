#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/debug/NamedTargets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace soundshed::nano
{
/// The active preset's signal chain, drawn from uiclient::BuildChainLayout (the web UI's walk
/// of the graph, parallel lanes and all).
///
/// Two layouts share the drawing and the gestures:
///  - Strip: one row of small chips above the effect controls, so the chain stays in view
///    while an effect is edited; a tap selects.
///  - Full: the chain page, with bigger cards, parallel branches stacked, the input and
///    output at the ends and a "+" between every pair to add an effect there.
///
/// Gestures: tap a card to select it (on the chain page that also opens its controls); tap a
/// card's LED to bypass it; long-press (or right-click) a card for its menu.
class ChainView final : public juce::Component,
                        public NamedTargets,
                        private juce::Timer
{
public:
    enum class Mode
    {
        Strip,
        Full
    };

    ChainView (NanoContext& context, ShellActions& actions, Mode mode);
    ~ChainView() override;

    /// Re-lays out from the client's active preset, and tells the owner the size changed.
    void refresh();

    /// Called after a refresh changed what there is to lay out; the owner re-sizes this.
    std::function<void()> onContentChanged;

    /// Re-reads the per-node clip lights (20 Hz), repainting only when one changed.
    void updateMeters (double nowSeconds);

    /// The size the content wants; the owner puts it in a viewport.
    [[nodiscard]] juce::Point<int> contentSize (int availableHeight);

    /// Scrolls the owning viewport so the selected node is in view.
    void revealSelected();

    [[nodiscard]] std::vector<std::pair<juce::String, juce::Rectangle<int>>> namedTargets() const override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

private:
    struct Item
    {
        enum class Kind
        {
            Node,
            Boundary, // input or output: drawn, and selectable for their trim
            Glyph,    // the split/join marks of a parallel block
            Add       // "+": add an effect after `afterNodeId`
        };

        Kind kind = Kind::Node;
        std::string nodeId;
        std::string afterNodeId;
        std::string icon;
        juce::Rectangle<float> bounds;
    };

    void layoutStrip (int height);
    void layoutFull (int height);
    void timerCallback() override;
    [[nodiscard]] const Item* itemAt (juce::Point<float> position) const;
    [[nodiscard]] bool nodeMissingResource (const guitarfx::GraphNode& node) const;

    NanoContext& context;
    ShellActions& actions;
    Mode mode;
    std::vector<Item> items;
    std::vector<std::pair<juce::Point<float>, juce::Point<float>>> wires;
    std::vector<std::string> clipping;
    std::map<std::string, double> clipUntil;
    int laidOutHeight = -1;
    juce::Point<int> laidOutSize;

    // A copy, not a pointer: an engine message can re-lay out the items mid-press.
    std::optional<Item> pressed;
    bool longPressFired = false;

    guitarfx::uiclient::Subscription presetSubscription;
    guitarfx::uiclient::Subscription catalogSubscription;
    guitarfx::uiclient::Subscription resourcesSubscription;
};
/// The chain in a viewport: horizontally scrolling for the strip, both ways for the page.
class ChainPanel final : public juce::Component
{
public:
    ChainPanel (NanoContext& context, ShellActions& actions, ChainView::Mode mode);

    [[nodiscard]] ChainView& getChain() noexcept { return chain; }
    void resized() override;

private:
    juce::Viewport viewport;
    ChainView chain;
};
} // namespace soundshed::nano
