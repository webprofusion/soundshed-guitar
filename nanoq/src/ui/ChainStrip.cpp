// The chain strip: the active preset's signal chain as a row of cards, parallel branches
// stacked. A tap selects a card, a tap on its light bypasses it, a long press or right click
// opens its menu (Shell::OpenNodeMenu).

#include "ui/Shell.h"

#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>

namespace nanoq::ui
{
namespace
{
using guitarfx::uiclient::ChainItem;

constexpr float kCardWidth = 150.0f;
constexpr float kCardHeight = 48.0f;
constexpr float kLightWidth = 30.0f;

/// One effect in the strip. The light at the left bypasses it; the rest of the card selects it.
class NodeChip final : public el::element
{
public:
    NodeChip(const Theme& theme, std::string name, std::string detail, bool enabled, bool selected, el::color tint)
        : mTheme(theme), mName(std::move(name)), mDetail(std::move(detail)), mEnabled(enabled), mSelected(selected), mTint(tint)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kCardWidth, kCardHeight}, {kCardWidth, kCardHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(1.0f, 1.0f);

        cnv.fill_style(mSelected ? mTheme.Elevated() : mTheme.Card());
        cnv.fill_round_rect(b, metrics::kRadius);
        cnv.stroke_style(mSelected ? mTheme.Accent() : mTheme.Border());
        cnv.line_width(mSelected ? 2.0f : 1.0f);
        cnv.stroke_round_rect(b, metrics::kRadius);

        // The effect's category colour, as a short bar at the left edge.
        cnv.fill_style(mTint.opacity(mEnabled ? 0.9f : 0.35f));
        cnv.fill_round_rect(el::rect{b.left + 1.5f, b.top + 9.0f, b.left + 4.5f, b.bottom - 9.0f}, 1.5f);

        // The light: green when the effect is on.
        const el::point c{b.left + kLightWidth * 0.5f, b.top + b.height() * 0.5f};
        cnv.fill_style(mEnabled ? mTheme.Success() : mTheme.TextMuted().opacity(0.35f));
        cnv.add_circle(cycfi::artist::circle(c, mHoverLight ? 6.5f : 5.5f));
        cnv.fill();

        auto text = b;
        text.left += kLightWidth;
        text.right -= 8.0f;

        const auto nameFont = Font(Weight::Medium).size(13.0f);
        const auto detailFont = Font(Weight::Regular).size(11.0f);
        const float nameY = b.top + (mDetail.empty() ? b.height() * 0.5f : b.height() * 0.38f);

        auto state = cnv.new_state();
        cnv.text_align(cycfi::artist::canvas::left | cycfi::artist::canvas::middle);
        cnv.font(nameFont);
        cnv.fill_style(mEnabled ? mTheme.Text() : mTheme.TextMuted());
        cnv.fill_text(Fit(cnv, mName, nameFont, text.width()), el::point{text.left, nameY});

        if (!mDetail.empty())
        {
            cnv.font(detailFont);
            cnv.fill_style(mTheme.TextMuted());
            cnv.fill_text(Fit(cnv, mDetail, detailFont, text.width()), el::point{text.left, b.top + b.height() * 0.74f});
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right)
        {
            if (btn.down && onMenu)
                onMenu();
            return true;
        }
        if (btn.state != el::mouse_button::left)
            return false;

        if (btn.down)
        {
            mDown = true;
            mDownOnLight = btn.pos.x < ctx.bounds.left + kLightWidth;
            return true;
        }

        const bool wasDown = mDown;
        mDown = false;
        if (wasDown && ctx.bounds.includes(btn.pos))
        {
            if (mDownOnLight && onToggle)
                onToggle();
            else if (onSelect)
                onSelect();
        }
        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool light = status != el::cursor_tracking::leaving && p.x < ctx.bounds.left + kLightWidth;
        if (light != mHoverLight)
        {
            mHoverLight = light;
            ctx.view.refresh(ctx);
        }
        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

    std::function<void()> onSelect, onToggle, onMenu;

private:
    static std::string Fit(cycfi::artist::canvas& cnv, const std::string& text, const el::font_descr& font, float width)
    {
        if (el::measure_text(cnv, text, font).x <= width)
            return text;
        std::string cut = text;
        while (!cut.empty())
        {
            do
                cut.pop_back();
            while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
            if (el::measure_text(cnv, cut + "\xE2\x80\xA6", font).x <= width)
                return cut + "\xE2\x80\xA6";
        }
        return "\xE2\x80\xA6";
    }

    const Theme& mTheme;
    std::string mName, mDetail;
    bool mEnabled, mSelected;
    el::color mTint;
    bool mDown = false, mDownOnLight = false, mHoverLight = false;
};
} // namespace

std::string Shell::ChainSignature() const
{
    if (!mState.activePreset)
        return {};

    std::string s = mSelectedNode + "|" + mState.activePresetId + "|" + mState.activeSceneId;
    for (const auto& node : mState.activePreset->graph.nodes)
    {
        s += "|" + node.id + ":" + node.type + ":" + (node.enabled ? "1" : "0") + ":" +
             guitarfx::uiclient::NodeDisplayName(mState, node);
    }
    for (const auto& edge : mState.activePreset->graph.edges)
        s += "|" + edge.from + ">" + edge.to + "#" + std::to_string(edge.fromPort);
    return s;
}

Ptr Shell::BuildChain()
{
    using namespace guitarfx::uiclient;

    if (!mState.activePreset)
        return VGap(0);

    const auto& graph = mState.activePreset->graph;
    const auto layout = BuildChainLayout(graph);

    auto makeChip = [this, &graph](const std::string& nodeId) -> Ptr {
        const auto* node = graph.FindNode(nodeId);
        if (node == nullptr)
            return HGap(1);

        const auto* type = mState.FindEffectType(node->type);
        auto name = NodeDisplayName(mState, *node);
        std::string detail = type != nullptr ? type->name : std::string();
        if (detail == name)
            detail.clear();

        const auto tint = Theme::Argb(mPresentation.CategoryColour(NodeCategory(mState, *node)));
        auto chip = std::make_shared<NodeChip>(mTheme, name, detail, node->enabled, nodeId == mSelectedNode, tint);
        chip->onSelect = [this, nodeId] { SelectNode(nodeId); };
        chip->onMenu = [this, nodeId] { OpenNodeMenu(nodeId); };
        chip->onToggle = [this, nodeId] {
            if (const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr)
                mSession.Commands().SetNodeBypassed(nodeId, n->enabled);
        };
        return chip;
    };

    auto caption = [this](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 11.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre).Fixed(true);
        return FixedWidth(34, t);
    };

    std::vector<Ptr> row{HGap(10), caption("IN")};
    size_t tallest = 1;

    for (const auto& item : layout.items)
    {
        row.push_back(HGap(6));
        if (item.kind == ChainItem::Kind::Node)
        {
            row.push_back(makeChip(item.nodeId));
            continue;
        }

        // Parallel branches stacked, each as a short row of its own.
        std::vector<Ptr> lanes;
        for (const auto& branch : item.branches)
        {
            std::vector<Ptr> lane;
            for (const auto& id : branch.nodeIds)
            {
                if (!lane.empty())
                    lane.push_back(HGap(6));
                lane.push_back(makeChip(id));
            }
            if (!lanes.empty())
                lanes.push_back(VGap(4));
            lanes.push_back(Row(lane));
        }
        tallest = std::max(tallest, item.branches.size());
        row.push_back(Col(lanes));
    }

    // The last card: add an effect at the end of the chain.
    {
        auto add = std::make_shared<Button>(mTheme, "+", [this] { OpenEffectPicker(LastNodeBeforeOutput()); }, Button::Style::Plain);
        add->MinWidth(48).TextSize(20);
        row.push_back(HGap(6));
        row.push_back(FixedHeight(kCardHeight, add));
    }
    row.push_back(HGap(6));
    row.push_back(caption("OUT"));
    row.push_back(HGap(10));

    const float height = 12.0f + static_cast<float>(tallest) * (kCardHeight + 4.0f) + 12.0f;
    return FixedHeight(height, Fill(mTheme.Panel(), HScroll(PadXY(0, 8, Row(row)))));
}
} // namespace nanoq::ui
