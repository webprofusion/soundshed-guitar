#include "nativeui/views/ChainView.h"

#include "nativeui/views/NodeCard.h"
#include "uiclient/ChainLayout.h"

#include <limits>

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

// A wrapped chain page: the space between lines, and the room beside them for the route
// from one line's end round to the next line's start to turn in.
constexpr float kLineGap = 28.0f;
constexpr float kWrapGutter = 16.0f;
constexpr float kRouteReach = 10.0f;
constexpr float kRouteRadius = 8.0f;

// Nano's own, beside nativeUi.scale; the web UI keeps its choice in its part of uiSettings.
constexpr const char* kChainWrapSetting = "nativeUi.chainWrap";
} // namespace

ChainView::ChainView (NanoContext& contextIn, ShellActions& actionsIn, Mode modeIn)
    : context (contextIn), actions (actionsIn), mode (modeIn)
{
    setComponentID (mode == Mode::Strip ? "chain-strip" : "chain-page");
    setTitle (mode == Mode::Strip ? "Signal chain" : "Signal chain editor");
    presetSubscription = context.client.Subscribe (Topic::ActivePreset, [this] { refresh(); });
    catalogSubscription = context.client.Subscribe (Topic::Catalog, [this] { refresh(); });
    resourcesSubscription = context.client.Subscribe (Topic::Resources, [this] { repaint(); });

    // The strip is one row by design: it sits above the effect's controls.
    if (mode == Mode::Full)
    {
        wrap = wrapSetting();
        sessionSubscription = context.client.Subscribe (Topic::Session, [this] {
            if (wrapSetting() != wrap)
            {
                wrap = ! wrap;
                refresh();
            }
        });
    }
}

ChainView::~ChainView() = default;

bool ChainView::wrapSetting() const
{
    const auto& settings = context.state().appSettings;
    const auto it = settings.find (kChainWrapSetting);
    return it != settings.end() && it->is_boolean() && it->get<bool>();
}

void ChainView::refresh()
{
    laidOut = false;

    if (onContentChanged)
        onContentChanged();

    repaint();
}

juce::Point<int> ChainView::contentSize (juce::Point<int> available)
{
    // Only a wrapping page lays out to the width; the rest ignore a width change.
    if (! wrapsToWidth())
        available.x = laidOutFor.x;

    if (! laidOut || available != laidOutFor)
    {
        laidOut = true;
        laidOutFor = available;

        if (mode == Mode::Strip)
            layoutStrip (available.y);
        else
            layoutFull (available);
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

void ChainView::layoutFull (juce::Point<int> available)
{
    items.clear();
    wires.clear();
    routes.clear();
    const auto& state = context.state();

    if (! state.activePreset)
    {
        laidOutSize = { 0, available.y };
        return;
    }

    // The chain as segments, the way the web UI wraps it: the input, then each connector with
    // what it leads into (a node, or a parallel block up to its join), then the output. Each
    // is laid out on its own from x = 0 along a centre line at y = 0; lines then take whole
    // segments, so a line never ends on a connector.
    struct Segment
    {
        std::vector<Item> items;
        std::vector<Wire> wires;
        float width = 0.0f;
        float halfHeight = kCardHeight * 0.5f;
    };

    const auto layout = BuildChainLayout (state.activePreset->graph);
    std::vector<Segment> segments;
    float x = 0.0f;

    const auto cardAt = [&] (Item::Kind kind, const std::string& id, float y) {
        segments.back().items.push_back ({ kind, id, {}, {}, { x, y - kCardHeight * 0.5f, kCardWidth, kCardHeight } });
    };

    const auto connectorAfter = [&] (const std::string& afterId, float y) {
        segments.back().items.push_back ({ Item::Kind::Add, {}, afterId, "plus",
                                           { x + (kConnector - 24.0f) * 0.5f, y - 12.0f, 24.0f, 24.0f } });
        segments.back().wires.push_back ({ { x - 2.0f, y }, { x + kConnector + 2.0f, y } });
        x += kConnector;
    };

    const auto beginSegment = [&] {
        segments.emplace_back();
        x = 0.0f;
    };

    beginSegment();
    cardAt (Item::Kind::Boundary, kInputNodeId, 0.0f);
    x += kCardWidth;
    segments.back().width = x;
    std::string last = kInputNodeId;

    for (const auto& item : layout.items)
    {
        beginSegment();
        connectorAfter (last, 0.0f);

        if (item.kind == ChainItem::Kind::Node)
        {
            cardAt (Item::Kind::Node, item.nodeId, 0.0f);
            x += kCardWidth;
            segments.back().width = x;
            last = item.nodeId;
            continue;
        }

        // Parallel: the branches stacked, each with its own "+" at its start.
        const float blockX = x;
        float blockWidth = 0.0f;
        const float spread = 0.5f * ((float) (item.branches.size() - 1) * (kCardHeight + kBranchGap));

        for (std::size_t b = 0; b < item.branches.size(); ++b)
        {
            const float y = (float) b * (kCardHeight + kBranchGap) - spread;
            x = blockX;
            std::string branchLast = item.splitNodeId;

            for (const auto& id : item.branches[b].nodeIds)
            {
                if (branchLast != item.splitNodeId)
                    connectorAfter (branchLast, y);
                else
                    segments.back().wires.push_back ({ { x - kConnector * 0.5f, 0.0f }, { x, y } });

                cardAt (Item::Kind::Node, id, y);
                x += kCardWidth;
                branchLast = id;
            }

            segments.back().wires.push_back ({ { x, y }, { x + kConnector * 0.5f, 0.0f } });
            blockWidth = std::max (blockWidth, x - blockX);
        }

        x = blockX + blockWidth + kConnector * 0.5f;
        segments.back().width = x;
        segments.back().halfHeight = spread + kCardHeight * 0.5f;
        last = item.splitNodeId;
    }

    beginSegment();
    connectorAfter (last, 0.0f);
    cardAt (Item::Kind::Boundary, kOutputNodeId, 0.0f);
    x += kCardWidth;
    segments.back().width = x;

    // Fill each line with as many whole segments as fit. Unwrapped, that is all of them.
    struct Line
    {
        std::size_t first = 0, count = 0;
        float width = 0.0f, halfHeight = 0.0f, top = 0.0f;
    };

    const float gutter = wrap ? kWrapGutter : 0.0f;
    const float lineRoom = wrap ? (float) available.x - 2.0f * (kMargin + gutter) : std::numeric_limits<float>::max();
    std::vector<Line> lines;

    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        if (lines.empty() || lines.back().width + segments[i].width > lineRoom)
            lines.push_back ({ i, 0, 0.0f, 0.0f, 0.0f });

        auto& line = lines.back();
        ++line.count;
        line.width += segments[i].width;
        line.halfHeight = std::max (line.halfHeight, segments[i].halfHeight);
    }

    float widest = 0.0f;
    float y = kMargin;

    for (auto& line : lines)
    {
        line.top = y;
        y += 2.0f * line.halfHeight + kLineGap;
        widest = std::max (widest, line.width);
    }

    const float contentWidth = widest + 2.0f * (kMargin + gutter);
    const float contentHeight = y - kLineGap + kMargin;
    laidOutSize = { (int) std::ceil (contentWidth), juce::jmax (available.y, (int) std::ceil (contentHeight)) };

    // Centre a short chain vertically on a tall page, and a wrapped one across the page.
    const float left = kMargin + gutter + (wrap ? std::max (0.0f, std::floor (((float) available.x - contentWidth) * 0.5f)) : 0.0f);
    const float top = std::max (0.0f, std::floor (((float) available.y - contentHeight) * 0.5f));

    for (const auto& line : lines)
    {
        const juce::Point<float> origin { left, top + line.top + line.halfHeight };
        float lineX = 0.0f;

        for (std::size_t i = line.first; i < line.first + line.count; ++i)
        {
            const auto offset = origin.translated (lineX, 0.0f);

            for (auto item : segments[i].items)
            {
                item.bounds += offset;
                items.push_back (std::move (item));
            }

            for (const auto& wire : segments[i].wires)
                wires.push_back ({ wire.first + offset, wire.second + offset });

            lineX += segments[i].width;
        }
    }

    // From the end of each line out, down, back under it, down and into the next line's start.
    for (std::size_t i = 0; i + 1 < lines.size(); ++i)
    {
        const auto& from = lines[i];
        const auto& to = lines[i + 1];
        const float y1 = top + from.top + from.halfHeight;
        const float y2 = top + to.top + to.halfHeight;
        const float gap = top + from.top + 2.0f * from.halfHeight + kLineGap * 0.5f;
        const float x1 = left + from.width;

        juce::Path turn;
        turn.startNewSubPath (x1, y1);
        turn.lineTo (x1 + kRouteReach, y1);
        turn.lineTo (x1 + kRouteReach, gap);
        turn.lineTo (left - kRouteReach, gap);
        turn.lineTo (left - kRouteReach, y2);
        turn.lineTo (left, y2);
        routes.push_back (turn.createPathWithRoundedCorners (kRouteRadius));
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
    // Lays the items out if nothing has since a refresh. The size the owner last asked about,
    // not this component's: a chain wider than its page is bigger than the page it fits to.
    juce::ignoreUnused (contentSize (laidOutFor.y >= 0 ? laidOutFor : juce::Point<int> (getWidth(), getHeight())));
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

    for (const auto& route : routes)
        g.strokePath (route, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

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

    // A wrapping page keeps clear of the vertical scroll bar its extra lines can bring, so
    // that bar never pushes the lines it fits into a horizontal scroll as well.
    const int width = chain.wrapsToWidth() ? juce::jmax (0, view.getWidth() - viewport.getScrollBarThickness()) : view.getWidth();
    const auto content = chain.contentSize ({ width, view.getHeight() });
    chain.setSize (juce::jmax (content.x, width), juce::jmax (content.y, view.getHeight()));
    chain.revealSelected();
}
} // namespace soundshed::nano
