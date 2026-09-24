#include "nativeui/views/ChainView.h"

#include "nativeui/views/NodeCard.h"
#include "uiclient/ChainLayout.h"

namespace soundshed::nano
{
using namespace guitarfx::uiclient;

namespace
{
constexpr int kLongPressMs = 550;
constexpr double kClipHoldSeconds = 1.5;

// Strip chips and full-page cards, in logical pixels.
constexpr float kChipWidth = 82.0f;
constexpr float kChipGap = 6.0f;
constexpr float kGlyphWidth = 22.0f;
constexpr float kCardWidth = 132.0f;
constexpr float kCardHeight = 92.0f;
constexpr float kConnector = 34.0f;
constexpr float kBranchGap = 14.0f;
constexpr float kMargin = 12.0f;
} // namespace

ChainView::ChainView (NanoContext& contextIn, ShellActions& actionsIn, Mode modeIn)
    : context (contextIn), actions (actionsIn), mode (modeIn)
{
    setComponentID (mode == Mode::Strip ? "chain-strip" : "chain-page");
    setTitle (mode == Mode::Strip ? "Signal chain" : "Signal chain editor");
    presetSubscription = context.client.Subscribe (Topic::ActivePreset, [this] { refresh(); });
    catalogSubscription = context.client.Subscribe (Topic::Catalog, [this] { refresh(); });
    resourcesSubscription = context.client.Subscribe (Topic::Resources, [this] { repaint(); });
}

ChainView::~ChainView() = default;

void ChainView::refresh()
{
    laidOutHeight = -1;

    if (onContentChanged)
        onContentChanged();

    repaint();
}

juce::Point<int> ChainView::contentSize (int availableHeight)
{
    if (laidOutHeight != availableHeight)
    {
        laidOutHeight = availableHeight;

        if (mode == Mode::Strip)
            layoutStrip (availableHeight);
        else
            layoutFull (availableHeight);
    }

    return laidOutSize;
}

void ChainView::layoutStrip (int height)
{
    items.clear();
    wires.clear();
    const auto& state = context.state();

    if (! state.activePreset)
    {
        laidOutSize = { 0, height };
        return;
    }

    const auto layout = BuildChainLayout (state.activePreset->graph);
    const float h = (float) height - 8.0f;
    float x = 6.0f;
    std::string last = kInputNodeId;

    const auto addNode = [&] (const std::string& id) {
        items.push_back ({ Item::Kind::Node, id, {}, {}, { x, 4.0f, kChipWidth, h } });
        x += kChipWidth + kChipGap;
        last = id;
    };

    for (const auto& item : layout.items)
    {
        if (item.kind == ChainItem::Kind::Node)
        {
            addNode (item.nodeId);
            continue;
        }

        // A parallel block reads as its branches in a row, bracketed by the split and join marks.
        items.push_back ({ Item::Kind::Glyph, item.splitNodeId, {}, "parallel-split", { x, 4.0f, kGlyphWidth, h } });
        x += kGlyphWidth + kChipGap;

        for (const auto& branch : item.branches)
            for (const auto& id : branch.nodeIds)
                addNode (id);

        items.push_back ({ Item::Kind::Glyph, item.joinNodeId, {}, "parallel-join", { x, 4.0f, kGlyphWidth, h } });
        x += kGlyphWidth + kChipGap;
    }

    // "+" adds after the selected effect, or at the end.
    items.push_back ({ Item::Kind::Add, {}, last, "plus", { x + 4.0f, 4.0f + h * 0.5f - 16.0f, 32.0f, 32.0f } });
    x += 42.0f;
    laidOutSize = { (int) std::ceil (x), height };
}

void ChainView::layoutFull (int height)
{
    items.clear();
    wires.clear();
    const auto& state = context.state();

    if (! state.activePreset)
    {
        laidOutSize = { 0, height };
        return;
    }

    const auto layout = BuildChainLayout (state.activePreset->graph);
    std::size_t lanes = 1;

    for (const auto& item : layout.items)
        if (item.kind == ChainItem::Kind::Parallel)
            lanes = std::max (lanes, item.branches.size());

    const float rowY = kMargin + 0.5f * ((float) (lanes - 1) * (kCardHeight + kBranchGap));
    float x = kMargin;

    const auto cardAt = [&] (Item::Kind kind, const std::string& id, float y) {
        items.push_back ({ kind, id, {}, {}, { x, y, kCardWidth, kCardHeight } });
    };

    const auto connectorAfter = [&] (const std::string& afterId, float y) {
        items.push_back ({ Item::Kind::Add, {}, afterId, "plus",
                           { x + (kConnector - 24.0f) * 0.5f, y + (kCardHeight - 24.0f) * 0.5f, 24.0f, 24.0f } });
        wires.push_back ({ { x - 2.0f, y + kCardHeight * 0.5f }, { x + kConnector + 2.0f, y + kCardHeight * 0.5f } });
        x += kConnector;
    };

    cardAt (Item::Kind::Boundary, kInputNodeId, rowY);
    x += kCardWidth;
    std::string last = kInputNodeId;

    for (const auto& item : layout.items)
    {
        if (item.kind == ChainItem::Kind::Node)
        {
            connectorAfter (last, rowY);
            cardAt (Item::Kind::Node, item.nodeId, rowY);
            x += kCardWidth;
            last = item.nodeId;
            continue;
        }

        // Parallel: the branches stacked, each with its own "+" at its start.
        connectorAfter (last, rowY);
        const float blockX = x;
        float blockWidth = 0.0f;
        const float firstY = rowY - 0.5f * ((float) (item.branches.size() - 1) * (kCardHeight + kBranchGap));

        for (std::size_t b = 0; b < item.branches.size(); ++b)
        {
            const float y = firstY + (float) b * (kCardHeight + kBranchGap);
            x = blockX;
            std::string branchLast = item.splitNodeId;

            for (const auto& id : item.branches[b].nodeIds)
            {
                if (branchLast != item.splitNodeId)
                    connectorAfter (branchLast, y);
                else
                    wires.push_back ({ { x - kConnector * 0.5f, rowY + kCardHeight * 0.5f }, { x, y + kCardHeight * 0.5f } });

                cardAt (Item::Kind::Node, id, y);
                x += kCardWidth;
                branchLast = id;
            }

            wires.push_back ({ { x, y + kCardHeight * 0.5f }, { x + kConnector * 0.5f, rowY + kCardHeight * 0.5f } });
            blockWidth = std::max (blockWidth, x - blockX);
        }

        x = blockX + blockWidth + kConnector * 0.5f;
        last = item.splitNodeId;
    }

    connectorAfter (last, rowY);
    cardAt (Item::Kind::Boundary, kOutputNodeId, rowY);
    x += kCardWidth + kMargin;

    const float contentHeight = rowY * 2.0f + kCardHeight;
    laidOutSize = { (int) std::ceil (x), juce::jmax (height, (int) std::ceil (contentHeight)) };

    // Centre a short chain vertically on a tall page.
    if (contentHeight < (float) height)
    {
        const float offset = std::floor (((float) height - contentHeight) * 0.5f);

        for (auto& item : items)
            item.bounds.translate (0.0f, offset);

        for (auto& wire : wires)
        {
            wire.first.y += offset;
            wire.second.y += offset;
        }
    }
}

bool ChainView::nodeMissingResource (const guitarfx::GraphNode& node) const
{
    const auto& state = context.state();
    const auto* type = state.FindEffectType (node.type);

    if (type == nullptr || ! type->requiresResource || node.resources.empty())
        return false;

    for (const auto& ref : node.resources)
    {
        if (ref.IsLibraryRef())
        {
            const auto* resource = state.FindLibraryResource (ref.resourceType, ref.resourceId);

            if (resource == nullptr || resource->fileMissing)
                return true;
        }
    }

    return false;
}

void ChainView::updateMeters (double nowSeconds)
{
    bool changed = false;

    for (const auto& item : items)
    {
        if (item.kind != Item::Kind::Node)
            continue;

        const auto* level = nodeLevel (context, item.nodeId);
        auto& until = clipUntil[item.nodeId];
        const bool wasClipping = nowSeconds < until;

        if (level != nullptr && level->clipped)
            until = nowSeconds + kClipHoldSeconds;

        changed = changed || wasClipping != (nowSeconds < until);
    }

    clipping.clear();

    for (const auto& [id, until] : clipUntil)
        if (nowSeconds < until)
            clipping.push_back (id);

    if (changed)
        repaint();
}

void ChainView::revealSelected()
{
    auto* viewport = findParentComponentOfClass<juce::Viewport>();
    const auto selected = actions.selectedNode ? actions.selectedNode() : std::string {};

    if (viewport == nullptr || selected.empty())
        return;

    for (const auto& item : items)
    {
        if (item.nodeId == selected && (item.kind == Item::Kind::Node || item.kind == Item::Kind::Boundary))
        {
            const auto view = viewport->getViewArea();
            const auto target = item.bounds.toNearestInt();

            if (! view.contains (target))
                viewport->setViewPosition (juce::jmax (0, target.getCentreX() - view.getWidth() / 2),
                                           juce::jmax (0, target.getCentreY() - view.getHeight() / 2));

            return;
        }
    }
}

std::vector<std::pair<juce::String, juce::Rectangle<int>>> ChainView::namedTargets() const
{
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> targets;

    for (const auto& item : items)
    {
        const auto bounds = item.bounds.toNearestInt();

        switch (item.kind)
        {
            case Item::Kind::Node:
                targets.emplace_back ("node:" + juce::String (item.nodeId), bounds);
                targets.emplace_back ("bypass:" + juce::String (item.nodeId),
                                      nodeCardBypassArea (item.bounds, mode == Mode::Strip).toNearestInt());
                break;
            case Item::Kind::Boundary:
            case Item::Kind::Glyph:
                targets.emplace_back ("node:" + juce::String (item.nodeId), bounds);
                break;
            case Item::Kind::Add:
                targets.emplace_back ("add:" + juce::String (item.afterNodeId), bounds);
                break;
        }
    }

    return targets;
}

void ChainView::paint (juce::Graphics& g)
{
    juce::ignoreUnused (contentSize (getHeight())); // lays the items out if this height is new
    const auto& state = context.state();
    const auto& theme = context.theme;

    if (! state.activePreset)
    {
        g.setColour (theme.textMuted());
        g.setFont (context.font (14.0f));
        g.drawText ("No preset loaded", getLocalBounds(), juce::Justification::centred);
        return;
    }

    g.setColour (theme.textMuted().withAlpha (0.3f));

    for (const auto& wire : wires)
        g.drawLine ({ wire.first, wire.second }, 1.5f);

    const auto selected = actions.selectedNode ? actions.selectedNode() : std::string {};

    for (const auto& item : items)
    {
        switch (item.kind)
        {
            case Item::Kind::Node:
            case Item::Kind::Boundary:
            {
                const auto* node = state.activePreset->graph.FindNode (item.nodeId);

                if (node == nullptr)
                {
                    // An implicit boundary node the graph does not spell out.
                    guitarfx::GraphNode boundary;
                    boundary.id = item.nodeId;
                    boundary.type = item.nodeId == kInputNodeId ? guitarfx::kNodeTypeInput : guitarfx::kNodeTypeOutput;
                    paintNodeCard (g, context, boundary, item.bounds, { false, mode == Mode::Strip, false, false });
                    break;
                }

                NodeCardLook look;
                look.selected = item.nodeId == selected;
                look.compact = mode == Mode::Strip;
                look.clipping = std::find (clipping.begin(), clipping.end(), item.nodeId) != clipping.end();
                look.missingResource = nodeMissingResource (*node);
                paintNodeCard (g, context, *node, item.bounds, look);
                break;
            }

            case Item::Kind::Glyph:
                context.icons->draw (g, item.icon, item.bounds.withSizeKeepingCentre (item.bounds.getWidth(), item.bounds.getWidth()),
                                     theme.textMuted());
                break;

            case Item::Kind::Add:
                g.setColour (theme.background());
                g.fillEllipse (item.bounds);
                g.setColour (theme.textMuted().withAlpha (0.35f));
                g.drawEllipse (item.bounds.reduced (0.5f), 1.0f);
                context.icons->draw (g, "plus", item.bounds.withSizeKeepingCentre (12.0f, 12.0f), theme.textSecondary());
                break;
        }
    }
}

const ChainView::Item* ChainView::itemAt (juce::Point<float> position) const
{
    // Add buttons are small: give them a finger-sized target.
    for (const auto& item : items)
        if (item.kind == Item::Kind::Add && item.bounds.expanded (8.0f).contains (position))
            return &item;

    for (const auto& item : items)
        if (item.kind != Item::Kind::Add && item.bounds.contains (position))
            return &item;

    return nullptr;
}

void ChainView::mouseDown (const juce::MouseEvent& e)
{
    const auto* hit = itemAt (e.position);
    pressed = hit != nullptr ? std::optional<Item> (*hit) : std::nullopt;
    longPressFired = false;

    if (! pressed)
        return;

    if (e.mods.isPopupMenu() && pressed->kind == Item::Kind::Node)
    {
        longPressFired = true;

        if (actions.openNodeMenu)
            actions.openNodeMenu (pressed->nodeId, this);

        return;
    }

    if (pressed->kind == Item::Kind::Node)
        startTimer (kLongPressMs);
}

void ChainView::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 8)
    {
        stopTimer();
        pressed.reset(); // a drag scrolls the viewport; it is not a tap
    }
}

void ChainView::timerCallback()
{
    stopTimer();

    if (pressed && isMouseButtonDown() && actions.openNodeMenu)
    {
        longPressFired = true;
        actions.openNodeMenu (pressed->nodeId, this);
    }
}

void ChainView::mouseUp (const juce::MouseEvent& e)
{
    stopTimer();
    const auto pressedItem = pressed;
    pressed.reset();

    if (! pressedItem || longPressFired)
        return;

    // A press held long enough ends as a long press even if the timer was starved.
    if (pressedItem->kind == Item::Kind::Node && e.getLengthOfMousePress() >= kLongPressMs && e.getDistanceFromDragStart() < 8)
    {
        if (actions.openNodeMenu)
            actions.openNodeMenu (pressedItem->nodeId, this);

        return;
    }

    const auto* item = itemAt (e.position);

    // Released over what was pressed (by position: the items may have been re-laid out).
    if (item == nullptr || item->kind != pressedItem->kind || item->nodeId != pressedItem->nodeId
        || item->afterNodeId != pressedItem->afterNodeId)
        return;

    switch (item->kind)
    {
        case Item::Kind::Add:
            if (actions.openEffectPicker)
            {
                // In the strip, "+" adds after the selected effect when there is one.
                auto after = item->afterNodeId;

                if (mode == Mode::Strip && actions.selectedNode)
                    if (const auto selected = actions.selectedNode(); ! selected.empty() && selected != kOutputNodeId)
                        after = selected;

                actions.openEffectPicker (after);
            }

            break;

        case Item::Kind::Node:
        {
            const auto* node = context.state().activePreset ? context.state().activePreset->graph.FindNode (item->nodeId) : nullptr;

            if (node != nullptr && nodeCardBypassArea (item->bounds, mode == Mode::Strip).expanded (4.0f).contains (e.position))
            {
                context.commands.SetNodeBypassed (node->id, node->enabled);
                break;
            }

            if (actions.selectNode)
                actions.selectNode (item->nodeId, mode == Mode::Full);

            break;
        }

        case Item::Kind::Boundary:
        case Item::Kind::Glyph:
            if (actions.selectNode && ! item->nodeId.empty())
                actions.selectNode (item->nodeId, mode == Mode::Full);

            break;
    }

    repaint();
}
ChainPanel::ChainPanel (NanoContext& context, ShellActions& actions, ChainView::Mode mode)
    : chain (context, actions, mode)
{
    viewport.setViewedComponent (&chain, false);
    viewport.setScrollBarsShown (mode == ChainView::Mode::Full, true, mode == ChainView::Mode::Full, true);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    viewport.setScrollBarThickness (6);
    addAndMakeVisible (viewport);
    chain.onContentChanged = [this] { resized(); };
}

void ChainPanel::resized()
{
    viewport.setBounds (getLocalBounds());
    const auto view = viewport.getMaximumVisibleWidth() > 0 ? viewport.getLocalBounds() : getLocalBounds();
    const auto content = chain.contentSize (view.getHeight());
    chain.setSize (juce::jmax (content.x, view.getWidth()), juce::jmax (content.y, view.getHeight()));
    chain.revealSelected();
}
} // namespace soundshed::nano
