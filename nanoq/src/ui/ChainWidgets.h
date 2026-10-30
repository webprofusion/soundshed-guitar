#pragma once

// The pieces the chain strip (ChainStrip.cpp) is drawn from: a node's card, the cable between two
// cards with its +, the rails where a parallel block splits and joins, and the cable that runs
// back under a wrapped line. Each is an Elements element that paints from the theme; the strip
// decides where they go.

#include "ui/Drawn.h"
#include "ui/Graphics.h"
#include "ui/Layout.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace chain
{
/// Where the strip's cards and cables were last drawn, for dropping a dragged card: each card
/// and cable notes its own bounds as it draws, in the view's coordinates. One per build of the
/// strip. It also says what a drag is over, which those elements draw as a highlight.
struct HitMap
{
    struct Card
    {
        std::string nodeId;
        el::rect bounds;
    };

    struct Cable
    {
        guitarfx::GraphEdge edge;
        el::rect bounds;
    };

    std::vector<Card> cards;
    std::vector<Cable> cables;
    std::string hoverNode;
    std::optional<guitarfx::GraphEdge> hoverEdge;

    void NoteCard(const std::string& nodeId, const el::rect& bounds)
    {
        for (auto& c : cards)
        {
            if (c.nodeId == nodeId)
            {
                c.bounds = bounds;
                return;
            }
        }

        cards.push_back({nodeId, bounds});
    }

    void NoteCable(const guitarfx::GraphEdge& edge, const el::rect& bounds)
    {
        for (auto& c : cables)
        {
            if (c.edge == edge)
            {
                c.bounds = bounds;
                return;
            }
        }

        cables.push_back({edge, bounds});
    }

    [[nodiscard]] const Card* FindCard(const std::string& nodeId) const
    {
        for (const auto& c : cards)
        {
            if (c.nodeId == nodeId)
            {
                return &c;
            }
        }

        return nullptr;
    }
};

// The web UI's .signal-node: 72 px square at the least, 86 px tall when its artwork sits over
// its top edge; a row is tall enough for either, with the artwork's overhang.
constexpr float kCardHeight = 92.0f; // a row of the chain, and a parallel lane
constexpr float kPlainCardHeight = 72.0f;
constexpr float kThumbCardHeight = 86.0f;
constexpr float kBoundaryWidth = 72.0f;
constexpr float kConnectorWidth = 36.0f;
constexpr float kRailWidth = 30.0f;
constexpr float kLaneGap = 8.0f;
constexpr float kWireEndWidth = 14.0f; // where a line's cable turns down, or the next one's comes in
constexpr float kRouteHeight = 16.0f;  // the cable back under a line
constexpr float kSidePad = 12.0f;
constexpr float kPowerSize = 16.0f;
// The chain dragged short (nodes-compact.css): each card a 40 px tile with only its icon, in a
// 48 px row.
constexpr float kIconCardSize = 40.0f;
constexpr float kIconRowHeight = 48.0f;

/// A card's width for its text, estimated from its length so the wrap can be worked out before
/// anything is measured: the 10 px capitals run about 6.6 px a letter and the 9 px detail about
/// 5.2, inside 12 px of padding each side (min-width 72, the text at most 160).
inline float CardWidthFor(const std::string& name, const std::string& detail)
{
    const auto chars = [](const std::string& s) {
        return static_cast<float>(
            std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    const float wanted = std::min(160.0f, std::max(chars(name) * 6.6f, chars(detail) * 5.2f)) + 24.0f;
    return std::clamp(wanted, 72.0f, 185.0f);
}

/// The cable's colour (--signal-route-main: the secondary text with a fifth of green).
inline el::color CableColour(const Theme& theme)
{
    const auto t = theme.TextSecondary(), g = theme.Success();
    return el::color{t.red * 0.78f + g.red * 0.22f, t.green * 0.78f + g.green * 0.22f, t.blue * 0.78f + g.blue * 0.22f,
                     1.0f};
}

/// `a` mixed into `b` by `amount` (CSS color-mix).
inline el::color Mix(el::color a, el::color b, float amount)
{
    return el::color{a.red * amount + b.red * (1.0f - amount), a.green * amount + b.green * (1.0f - amount),
                     a.blue * amount + b.blue * (1.0f - amount), a.alpha * amount + b.alpha * (1.0f - amount)};
}

/// The card's bounds inside whatever height the row gives it: centred, so every card and cable
/// in a row shares one centre line.
inline el::rect CardRect(const el::rect& bounds, float height = kPlainCardHeight)
{
    const float top = bounds.top + std::max(0.0f, (bounds.height() - height) * 0.5f);
    return el::rect{bounds.left, top, bounds.right, top + height}.inset(0.5f, 0.5f);
}
} // namespace chain

using namespace chain;

/// One node in the chain. With focus: Enter opens it in the editor (Ctrl+Enter swaps it for
/// another effect), Space turns it on or off, Delete removes it, the Menu key opens its menu, and
/// the arrows go along the chain.
class NodeCard final : public el::element, public Drawn, public Focusable
{
  public:
    enum class Light
    {
        None, // the card is off, or it is the splitter
        Idle, // on, nothing passing
        Live, // passing signal
        Clip
    };

    enum class Boundary
    {
        None,
        Input,
        Output
    };

    struct Look
    {
        std::string name;
        std::string detail;
        std::string badge;
        std::string icon;
        std::shared_ptr<artist::image> thumbnail;
        el::color tint;
        float width = 120.0f;
        bool enabled = true;
        bool selected = false;
        bool missing = false;
        bool canBypass = true;
        bool canRemove = false;
        bool iconOnly = false; // a 40 px tile with the icon (or the artwork) and nothing else
        Boundary boundary = Boundary::None;
    };

    NodeCard(const Theme& theme, Icons& icons, Look look) : mTheme(theme), mIcons(icons), mLook(std::move(look))
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;
        info.name = NameOr(mLook.name);
        info.value = mLook.detail;

        if (mLook.missing)
        {
            info.value += info.value.empty() ? "missing" : ", missing";
        }

        if (mLook.canBypass)
        {
            info.toggled = mLook.enabled ? 1 : 0;
        }

        info.selected = mLook.selected;
        return info;
    }

    bool Activate(const el::rect&) override
    {
        return Run(onSelect);
    }

    bool wants_focus() const override
    {
        return FocusWanted(onSelect != nullptr);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context&, el::key_info k) override
    {
        using el::key_code;

        if (IsMenuKey(k))
        {
            return Run(onMenu);
        }

        if (k.action != el::key_action::press)
        {
            return false;
        }

        const bool command = (k.modifiers & (el::mod_control | el::mod_action)) != 0;

        if ((k.key == key_code::enter || k.key == key_code::kp_enter) && command)
        {
            return Run(onReplace);
        }

        if (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super))
        {
            return false;
        }

        switch (k.key)
        {
        case key_code::enter:
        case key_code::kp_enter:
            return Run(onSelect);
        case key_code::space:
            return mLook.canBypass && Run(onToggle);
        case key_code::_delete:
            return mLook.canRemove && Run(onRemove);
        default:
            return false;
        }
    }

    /// Notes the card in `hits` as `nodeId` when drawn, so drops find it; with onDrag it can be
    /// dragged too.
    void Draggable(std::shared_ptr<HitMap> hits, std::string nodeId)
    {
        mHits = std::move(hits);
        mNodeId = std::move(nodeId);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mLook.width, mLook.iconOnly ? kIconRowHeight : kCardHeight}, {mLook.width, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;

        if (mHits && !mNodeId.empty())
        {
            mHits->NoteCard(mNodeId, CardRect(ctx.bounds, CardHeight()));
        }

        // While dragged, the card follows the pointer.
        auto dragState = cnv.new_state();

        if (mDragging)
        {
            cnv.translate(mDragOffset.x, mDragOffset.y);
        }

        const auto r = CardRect(ctx.bounds, CardHeight());
        const auto tint = mLook.tint;
        const bool on = mLook.enabled;
        const bool boundary = mLook.boundary != Boundary::None;
        constexpr float kRadius = 10.0f;
        const el::color face = mTheme.Card();

        // A bypassed card fades and greys (opacity .45, saturate .35): drawn here by mixing each
        // colour toward the stage behind it.
        const float fade = on ? 1.0f : 0.45f;
        const auto ghost = [&](el::color c) {
            return on ? c : Mix(Mix(c, el::color{0.5f, 0.5f, 0.55f, c.alpha}, 0.35f), mTheme.Bar(), fade);
        };

        // The shadow under it, and for the selected card a glow in its colour.
        {
            auto state = cnv.new_state();

            if (mLook.selected)
            {
                cnv.shadow_style(el::point{0.0f, 0.0f}, 22.0f, tint.opacity(0.24f));
            }
            else
            {
                cnv.shadow_style(el::point{0.0f, 3.0f}, 8.0f, el::rgba(0, 0, 0, on ? 90 : 40));
            }

            cnv.fill_style(ghost(face));
            cnv.fill_round_rect(r, kRadius);
        }
        // A wash of the category colour from the top, and a shade at the foot.
        {
            auto state = cnv.new_state();
            const el::point top{r.left + r.width() * 0.5f, r.top};
            artist::canvas::radial_gradient wash{top, 0.0f, top, std::max(r.width() * 0.6f, r.height() * 0.7f)};
            wash.add_color_stop(0.0f, tint.opacity(0.16f * fade));
            wash.add_color_stop(1.0f, tint.opacity(0.0f));
            cnv.fill_style(wash);
            cnv.fill_round_rect(r, kRadius);

            artist::canvas::linear_gradient shade{r.top_left(), r.bottom_left()};
            shade.add_color_stop(0.55f, el::rgba(0, 0, 0, 0));
            shade.add_color_stop(1.0f, el::rgba(0, 0, 0, 26));
            cnv.fill_style(shade);
            cnv.fill_round_rect(r, kRadius);
        }

        // The border: the category colour mixed into the border colour, full strength with an inner
        // ring when selected; red when its file is missing.
        const el::color border = mLook.missing    ? el::rgba(232, 76, 76, 153)
                                 : mLook.selected ? tint
                                 : boundary       ? Mix(tint, mTheme.Border(), 0.6f)
                                                  : Mix(tint, mTheme.Border(), 0.42f);
        cnv.stroke_style(ghost(border));
        cnv.line_width(mLook.selected ? 2.0f : 1.0f);
        cnv.stroke_round_rect(mLook.selected ? r.inset(0.5f, 0.5f) : r, kRadius);

        if (mLook.selected)
        {
            cnv.stroke_style(tint.opacity(0.36f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(r.inset(-1.0f, -1.0f), kRadius + 1.0f);
        }

        // A dragged card would land after this one: an accent ring round it.
        if (mHits && !mNodeId.empty() && mHits->hoverNode == mNodeId)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(2.0f);
            cnv.stroke_round_rect(r.inset(-3.0f, -3.0f), kRadius + 3.0f);
        }

        // The highlight along the top edge.
        cnv.stroke_style(Mix(el::rgba(255, 255, 255, 255), tint, 0.08f).opacity(0.18f * fade));
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(r.left + kRadius, r.top + 1.5f);
        cnv.line_to(r.right - kRadius, r.top + 1.5f);
        cnv.stroke();

        // The underglow: a strip of the category colour along the foot, wider when selected.
        if (on)
        {
            auto state = cnv.new_state();
            const float inset = r.width() * (mLook.selected ? 0.10f : 0.22f);
            const el::rect strip{r.left + inset, r.bottom - 1.0f, r.right - inset, r.bottom + 1.0f};
            artist::canvas::linear_gradient glow{strip.top_left(), strip.top_right()};
            const float strength = mLook.selected ? 1.0f : (mLight == Light::Live || mHover ? 0.9f : 0.5f);
            glow.add_color_stop(0.0f, tint.opacity(0.0f));
            glow.add_color_stop(0.5f, tint.opacity(strength));
            glow.add_color_stop(1.0f, tint.opacity(0.0f));
            cnv.shadow_style(el::point{0.0f, 0.0f}, 10.0f, tint.opacity(0.24f * strength));
            cnv.fill_style(glow);
            cnv.fill_round_rect(strip, 1.0f);
        }

        // Icon-only: the icon fills the tile, or the artwork does in a 26 px square, with no
        // name, effect or badge (nodes-compact.css).
        if (mLook.iconOnly)
        {
            const auto c = artist::center_point(r);

            if (mLook.thumbnail)
            {
                const el::rect picture{c.x - 13.0f, c.y - 13.0f, c.x + 13.0f, c.y + 13.0f};
                auto state = cnv.new_state();
                cnv.begin_path();
                cnv.add_round_rect(picture, 4.0f);
                cnv.clip();
                DrawImageCovering(cnv, *mLook.thumbnail, picture);

                if (!on)
                {
                    cnv.fill_style(mTheme.Bar().opacity(0.6f));
                    cnv.fill_rect(picture);
                }
            }
            else
            {
                mIcons.Draw(cnv, mLook.icon, el::rect{c.x - 8.0f, c.y - 8.0f, c.x + 8.0f, c.y + 8.0f}, ghost(tint));
            }
        }
        else
        {
            const float cx = r.left + r.width() * 0.5f;
            float y = r.top + 8.0f;

            // The picture: the artwork over the top edge with the icon in a dark disc on it, or the
            // icon in a round badge of the category colour.
            if (mLook.thumbnail)
            {
                const auto& image = *mLook.thumbnail;
                const auto size = image.size();
                const float h = 32.0f;
                const float w = size.y > 0.0f ? std::clamp(h * size.x / size.y, 24.0f, 64.0f) : h;
                const el::rect picture{cx - w * 0.5f, r.top - 8.0f, cx + w * 0.5f, r.top - 8.0f + h};
                {
                    auto state = cnv.new_state();
                    cnv.begin_path();
                    cnv.add_round_rect(picture, 6.0f);
                    cnv.clip();
                    DrawImageCovering(cnv, image, picture);
                    cnv.fill_style(mTheme.Bar().opacity(on ? 0.15f : 0.6f));
                    cnv.fill_rect(picture);
                }
                cnv.stroke_style(el::rgba(255, 255, 255, 31));
                cnv.line_width(1.0f);
                cnv.stroke_round_rect(picture.inset(0.5f, 0.5f), 6.0f);
                const auto c = artist::center_point(picture);
                cnv.fill_style(el::rgba(9, 11, 17, 102));
                cnv.add_circle(artist::circle(c, 11.0f));
                cnv.fill();
                mIcons.Draw(cnv, mLook.icon, el::rect{c.x - 7.0f, c.y - 7.0f, c.x + 7.0f, c.y + 7.0f},
                            ghost(tint).opacity(0.85f));
                y += 24.0f + 4.0f;
            }
            else
            {
                const el::point c{cx, y + 15.0f};
                auto state = cnv.new_state();
                artist::canvas::radial_gradient badge{el::point{c.x, c.y - 3.5f}, 0.0f, el::point{c.x, c.y - 3.5f},
                                                      15.0f * 1.1f};
                badge.add_color_stop(0.0f, tint.opacity(0.28f * fade));
                badge.add_color_stop(0.72f, tint.opacity(0.07f * fade));
                cnv.fill_style(badge);
                cnv.add_circle(artist::circle(c, 15.0f));
                cnv.fill();
                cnv.stroke_style(tint.opacity(0.24f * fade));
                cnv.line_width(1.0f);
                cnv.add_circle(artist::circle(c, 14.5f));
                cnv.stroke();
                mIcons.Draw(cnv, mLook.icon, el::rect{c.x - 8.0f, c.y - 8.0f, c.x + 8.0f, c.y + 8.0f}, ghost(tint));
                y += 30.0f + 4.0f;
            }

            // The name, in capitals as the web UI's are, the effect under it, and the model's badge.
            const el::color ink = ghost(mTheme.Text());
            const auto nameFont = Font(Weight::Medium).size(10.0f);
            const auto detailFont = Font(Weight::Medium).size(9.0f);
            const float textWidth = std::min(160.0f, r.width() - 16.0f);
            FillText(cnv, Ellipsize(cnv, Upper(mLook.name), nameFont, textWidth), nameFont, ink,
                     el::rect{r.left + 6.0f, y, r.right - 6.0f, y + 12.0f}, HAlign::Centre);
            y += 14.0f;

            if (!mLook.detail.empty())
            {
                FillText(cnv, Ellipsize(cnv, mLook.detail, detailFont, textWidth), detailFont,
                         ghost(Mix(tint, mTheme.TextMuted(), 0.26f)),
                         el::rect{r.left + 6.0f, y, r.right - 6.0f, y + 11.0f}, HAlign::Centre);
                y += 13.0f;
            }

            if (!mLook.badge.empty())
            {
                const auto badgeFont = Font(Weight::SemiBold).size(8.0f);
                const float w = el::measure_text(cnv, mLook.badge, badgeFont).x + 12.0f;
                const el::rect pill{cx - w * 0.5f, y, cx + w * 0.5f, y + 13.0f};
                cnv.fill_style(el::rgba(56, 118, 98, 71));
                cnv.fill_round_rect(pill, 6.5f);
                cnv.stroke_style(el::rgba(128, 230, 188, 92));
                cnv.line_width(1.0f);
                cnv.stroke_round_rect(pill.inset(0.5f, 0.5f), 6.5f);
                FillText(cnv, mLook.badge, badgeFont, el::rgba(186, 255, 226, 255), pill, HAlign::Centre);
            }
        }

        // The signal light, bottom right.
        if (on && mLight != Light::None)
        {
            const el::color colour = mLight == Light::Clip ? mTheme.Error() : el::rgba(72, 224, 120, 255);
            const el::point c{r.right - 9.0f, r.bottom - 9.0f};
            auto state = cnv.new_state();

            if (mLight != Light::Idle)
            {
                cnv.shadow_style(el::point{0.0f, 0.0f}, 6.0f, colour.opacity(0.8f));
            }

            cnv.fill_style(mLight == Light::Idle ? colour.opacity(0.35f) : colour);
            cnv.add_circle(artist::circle(c, 3.0f));
            cnv.fill();
        }

        if (mLook.missing && !mLook.iconOnly)
        {
            const auto font = Font(Weight::SemiBold).size(9.0f);
            FillText(cnv, "\xE2\x9A\xA0", font, el::rgba(255, 102, 102, 255),
                     el::rect{r.left + 4.0f, r.bottom - 15.0f, r.left + 16.0f, r.bottom - 3.0f}, HAlign::Centre);
        }

        // The corners (.signal-node-delete, .signal-node-bypass): remove at the top left and the
        // power at the top right, shown while the card is hovered or chosen; the power also
        // while the card is off.
        const auto corner = [&](el::rect at, bool hot, el::color ring) {
            const auto c = artist::center_point(at);
            cnv.fill_style(hot ? mTheme.Elevated() : el::rgba(0, 0, 0, 64));
            cnv.add_circle(artist::circle(c, kPowerSize * 0.5f));
            cnv.fill();
            cnv.stroke_style(ring);
            cnv.line_width(1.0f);
            cnv.add_circle(artist::circle(c, kPowerSize * 0.5f - 0.5f));
            cnv.stroke();
        };
        const bool showCorners = mHover || mLook.selected;

        if (mLook.canRemove && showCorners)
        {
            const auto x = RemoveRect(r);
            corner(x, mHoverRemove, el::rgba(255, 255, 255, 38));
            const auto c = artist::center_point(x);
            cnv.stroke_style(el::rgba(255, 255, 255, 205));
            cnv.line_width(1.4f);
            cnv.begin_path();
            cnv.move_to(c.x - 3.0f, c.y - 3.0f);
            cnv.line_to(c.x + 3.0f, c.y + 3.0f);
            cnv.move_to(c.x + 3.0f, c.y - 3.0f);
            cnv.line_to(c.x - 3.0f, c.y + 3.0f);
            cnv.stroke();
        }

        if (mLook.canBypass && (showCorners || !on))
        {
            const auto p = PowerRect(r);
            corner(p, mHoverPower, on ? el::rgba(255, 255, 255, 38) : el::rgba(255, 80, 80, 90));
            mIcons.Draw(cnv, "output", p.inset(3.0f, 3.0f),
                        on ? el::rgba(160, 236, 186, 242) : el::rgba(255, 170, 170, 250));
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right)
        {
            if (btn.down && onMenu)
            {
                onMenu();
            }

            return true;
        }

        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        const auto r = CardRect(ctx.bounds, CardHeight());

        if (btn.down)
        {
            mDown = true;
            mDownOnPower = mLook.canBypass && PowerRect(r).inset(-4.0f, -4.0f).includes(btn.pos);
            mDownOnRemove = !mDownOnPower && mLook.canRemove && (mHover || mLook.selected) &&
                            RemoveRect(r).inset(-4.0f, -4.0f).includes(btn.pos);
            mDownAt = btn.pos;
            mDragging = false;
            return true;
        }

        const bool wasDown = mDown;
        mDown = false;

        if (mDragging)
        {
            mDragging = false;
            mDragOffset = {0.0f, 0.0f};

            if (onDrag)
            {
                onDrag(mDownAt, btn.pos, true);
            }

            ctx.view.refresh();
            return true;
        }

        if (!wasDown || !r.includes(btn.pos))
        {
            return true;
        }

        if (mDownOnPower)
        {
            if (onToggle)
            {
                onToggle();
            }
        }
        else if (mDownOnRemove)
        {
            if (onRemove)
            {
                onRemove();
            }
        }
        else if (btn.num_clicks >= 2 && onReplace)
        {
            onReplace();
        }
        else if (onSelect)
        {
            onSelect();
        }

        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mDown || mDownOnPower || mDownOnRemove || !onDrag)
        {
            return;
        }

        const el::point offset{btn.pos.x - mDownAt.x, btn.pos.y - mDownAt.y};

        if (!mDragging && std::hypot(offset.x, offset.y) < 6.0f)
        {
            return;
        }

        mDragging = true;
        mDragOffset = offset;
        onDrag(mDownAt, btn.pos, false);
        ctx.view.refresh();
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const auto r = CardRect(ctx.bounds, CardHeight());
        const bool power = inside && mLook.canBypass && PowerRect(r).inset(-4.0f, -4.0f).includes(p);
        const bool remove = inside && mLook.canRemove && RemoveRect(r).inset(-4.0f, -4.0f).includes(p);

        if (inside != mHover || power != mHoverPower || remove != mHoverRemove)
        {
            mHover = inside;
            mHoverPower = power;
            mHoverRemove = remove;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

    /// Returns true if the light changed, so the card is worth redrawing.
    bool SetLight(Light light)
    {
        if (light == mLight)
        {
            return false;
        }

        mLight = light;
        return true;
    }

    [[nodiscard]] bool Enabled() const
    {
        return mLook.enabled;
    }

    [[nodiscard]] Boundary BoundaryKind() const
    {
        return mLook.boundary;
    }

    std::function<void()> onSelect, onToggle, onMenu, onReplace, onRemove;
    /// A drag from `from` that is now at `to` (view coordinates); `done` when it is let go.
    std::function<void(el::point from, el::point to, bool done)> onDrag;

  private:
    /// Runs one of the callbacks from a copy: it may rebuild the chain this card sits in.
    static bool Run(const std::function<void()>& f)
    {
        if (!f)
        {
            return false;
        }

        const auto run = f;
        run();
        return true;
    }

    [[nodiscard]] float CardHeight() const
    {
        if (mLook.iconOnly)
        {
            return kIconCardSize;
        }

        return mLook.thumbnail ? kThumbCardHeight : kPlainCardHeight;
    }

    [[nodiscard]] el::rect PowerRect(const el::rect& card) const
    {
        // On a tile it sits over the corner, clear of the icon.
        if (mLook.iconOnly)
        {
            return el::rect{card.right + 5.0f - kPowerSize, card.top - 5.0f, card.right + 5.0f,
                            card.top - 5.0f + kPowerSize};
        }

        return el::rect{card.right - 4.0f - kPowerSize, card.top + 4.0f, card.right - 4.0f,
                        card.top + 4.0f + kPowerSize};
    }

    [[nodiscard]] el::rect RemoveRect(const el::rect& card) const
    {
        if (mLook.iconOnly)
        {
            return el::rect{card.left - 5.0f, card.top - 5.0f, card.left - 5.0f + kPowerSize,
                            card.top - 5.0f + kPowerSize};
        }

        return el::rect{card.left + 4.0f, card.top + 4.0f, card.left + 4.0f + kPowerSize, card.top + 4.0f + kPowerSize};
    }

    const Theme& mTheme;
    Icons& mIcons;
    Look mLook;
    Light mLight = Light::Idle;
    bool mDown = false, mDownOnPower = false, mDownOnRemove = false, mHover = false, mHoverPower = false,
         mHoverRemove = false;
    std::shared_ptr<HitMap> mHits;
    std::string mNodeId;
    el::point mDownAt{0.0f, 0.0f};
    el::point mDragOffset{0.0f, 0.0f};
    bool mDragging = false;
};

/// A cable between two things in a row. With `onAdd` it carries a + in its middle that inserts
/// an effect there; `stretch` lets it run on to fill a short branch.
class Connector final : public el::element, public Drawn, public Focusable
{
  public:
    Connector(const Theme& theme, std::function<void()> onAdd, bool stretch = false)
        : mTheme(theme), mOnAdd(std::move(onAdd)), mStretch(stretch)
    {
    }

    // Its + is a button: add an effect here.
    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.name = NameOr("Add effect here");
        return info;
    }

    bool Activate(const el::rect&) override
    {
        if (!mOnAdd)
        {
            return false;
        }

        const auto add = mOnAdd;
        add();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mOnAdd != nullptr);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        return IsPressKey(k) && Activate(ctx.bounds);
    }

    /// The graph edge this cable draws, so a dragged card can be dropped on it.
    void Edge(std::shared_ptr<HitMap> hits, guitarfx::GraphEdge edge)
    {
        mHits = std::move(hits);
        mEdge = std::move(edge);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kConnectorWidth, mRowHeight}, {mStretch ? el::full_extent : kConnectorWidth, el::full_extent}};
    }

    /// The row it sits in: the cards' (kCardHeight), or the tiles' (kIconRowHeight).
    void RowHeight(float height)
    {
        mRowHeight = height;
    }

    /// The node whose signal this cable carries (its edge's source), or "" for a drawing-only one.
    [[nodiscard]] std::string Source() const
    {
        return mEdge ? mEdge->from : std::string();
    }

    /// Lights the cable with the signal (uiclient's SignalFlow): green, with pulses running down
    /// it, `phase` (0..1) along their 18 px spacing. Returns true when it needs a redraw.
    bool SetFlow(bool live, float phase)
    {
        const bool changed = live != mLive || (live && phase != mPhase);
        mLive = live;
        mPhase = phase;
        return changed;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float y = b.top + b.height() * 0.5f;

        if (mHits && mEdge)
        {
            mHits->NoteCable(*mEdge, b);
        }

        const bool dropHere = mHits && mEdge && mHits->hoverEdge && *mHits->hoverEdge == *mEdge;

        // A live cable (.signal-live): the hot route colour with a glow, and bright pulses
        // running toward what it feeds.
        if (mLive && !dropHere)
        {
            const auto hot = el::rgba(72, 224, 120, 255);
            auto state = cnv.new_state();
            cnv.line_cap(artist::canvas::round);
            cnv.stroke_style(hot.opacity(0.25f));
            cnv.line_width(7.0f);
            cnv.begin_path();
            cnv.move_to(b.left + 1.5f, y);
            cnv.line_to(b.right - 1.5f, y);
            cnv.stroke();
            cnv.stroke_style(hot);
            cnv.line_width(3.0f);
            cnv.begin_path();
            cnv.move_to(b.left + 1.5f, y);
            cnv.line_to(b.right - 1.5f, y);
            cnv.stroke();
            cnv.add_rect(el::rect{b.left + 1.5f, y - 2.0f, b.right - 1.5f, y + 2.0f});
            cnv.clip();
            constexpr float kSpacing = 18.0f;

            for (float x = b.left - kSpacing + mPhase * kSpacing; x < b.right; x += kSpacing)
            {
                artist::canvas::linear_gradient pulse{el::point{x, y}, el::point{x + kSpacing, y}};
                pulse.add_color_stop(0.0f, hot.opacity(0.0f));
                pulse.add_color_stop(0.5f, el::rgba(220, 255, 230, 230));
                pulse.add_color_stop(1.0f, hot.opacity(0.0f));
                cnv.fill_style(pulse);
                cnv.fill_rect(el::rect{x, y - 2.0f, x + kSpacing, y + 2.0f});
            }
        }

        // The cable (.signal-connector: 3 px, the route colour at 55%) with an arrowhead into
        // what it feeds, in the full colour; the accent where a dragged card would land.
        const auto route = dropHere ? mTheme.Accent() : mLive ? el::rgba(72, 224, 120, 255) : CableColour(mTheme);

        if (!mLive || dropHere)
        {
            cnv.stroke_style(route.opacity(0.55f));
            cnv.line_width(3.0f);
            cnv.line_cap(artist::canvas::round);
            cnv.begin_path();
            cnv.move_to(b.left + 1.5f, y);
            cnv.line_to(b.right - 1.5f, y);
            cnv.stroke();
        }

        cnv.stroke_style(route);
        cnv.line_width(1.5f);
        cnv.begin_path();
        cnv.move_to(b.right - 4.5f, y - 3.5f);
        cnv.line_to(b.right - 1.0f, y);
        cnv.line_to(b.right - 4.5f, y + 3.5f);
        cnv.stroke();

        if (!mOnAdd)
        {
            return;
        }

        const el::point c = PlusCentre(b);

        if (mHover)
        {
            // The add button (.signal-add-btn): 24 px, raised, a bold +.
            auto state = cnv.new_state();
            artist::canvas::linear_gradient fill{el::point{c.x, c.y - 11.0f}, el::point{c.x, c.y + 11.0f}};
            fill.add_color_stop(0.0f, mTheme.Pick(el::rgba(58, 58, 68, 255), mTheme.Elevated()));
            fill.add_color_stop(1.0f, mTheme.Pick(el::rgba(32, 32, 42, 255), mTheme.Card()));
            cnv.fill_style(fill);
            cnv.add_circle(artist::circle(c, 11.0f));
            cnv.fill();
            cnv.stroke_style(mTheme.Pick(el::rgba(74, 74, 88, 255), mTheme.BorderStrong()));
            cnv.line_width(2.0f);
            cnv.add_circle(artist::circle(c, 11.0f));
            cnv.stroke();
            cnv.stroke_style(mTheme.TextSecondary());
            cnv.line_width(2.0f);
            cnv.begin_path();
            cnv.move_to(c.x - 4.5f, c.y);
            cnv.line_to(c.x + 4.5f, c.y);
            cnv.move_to(c.x, c.y - 4.5f);
            cnv.line_to(c.x, c.y + 4.5f);
            cnv.stroke();
            return;
        }

        // At rest, a small ringed + on the cable (.signal-connector-wrapper::after).
        cnv.fill_style(Mix(mTheme.Bar(), el::rgba(0, 0, 0, 0), 0.72f));
        cnv.add_circle(artist::circle(c, 5.5f));
        cnv.fill();
        cnv.stroke_style(route.opacity(0.68f * 0.7f));
        cnv.line_width(1.0f);
        cnv.add_circle(artist::circle(c, 4.5f));
        cnv.stroke();
        cnv.stroke_style(route.opacity(0.7f));
        cnv.begin_path();
        cnv.move_to(c.x - 2.5f, c.y);
        cnv.line_to(c.x + 2.5f, c.y);
        cnv.move_to(c.x, c.y - 2.5f);
        cnv.line_to(c.x, c.y + 2.5f);
        cnv.stroke();
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mOnAdd || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!btn.down && HitsPlus(ctx, btn.pos))
        {
            mOnAdd();
        }

        return HitsPlus(ctx, btn.pos);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool hover = mOnAdd && status != el::cursor_tracking::leaving && HitsPlus(ctx, p);

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return static_cast<bool>(mOnAdd);
    }

  private:
    bool mLive = false;
    float mPhase = 0.0f;

    /// Where the + sits: two fifths along the cable (--signal-add-anchor-x: 40%).
    static el::point PlusCentre(const el::rect& b)
    {
        return el::point{b.left + kConnectorWidth * 0.4f, b.top + b.height() * 0.5f};
    }

    bool HitsPlus(const el::context& ctx, el::point p) const
    {
        const auto c = PlusCentre(ctx.bounds);
        return std::abs(p.x - c.x) <= 14.0f && std::abs(p.y - c.y) <= 14.0f;
    }

    const Theme& mTheme;
    std::function<void()> mOnAdd;
    bool mStretch;
    bool mHover = false;
    std::shared_ptr<HitMap> mHits;
    std::optional<guitarfx::GraphEdge> mEdge;
    float mRowHeight = kCardHeight;
};

/// Where a parallel block splits (or joins): the cable from the main line meets a rail that
/// reaches each branch, with the split (join) icon on it. The split's icon selects the splitter,
/// which is where its Branches control is.
class Rail final : public el::element, public Focusable
{
  public:
    Rail(const Theme& theme, Icons& icons, int lanes, bool join, bool selected, std::function<void()> onSelect)
        : mTheme(theme), mIcons(icons), mLanes(lanes), mJoin(join), mSelected(selected), mOnSelect(std::move(onSelect))
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;
        info.name = NameOr(mJoin ? "Mix" : "Split");
        info.value = std::to_string(mLanes) + " lanes";
        info.selected = mSelected;
        return info;
    }

    bool Activate(const el::rect&) override
    {
        if (!mOnSelect)
        {
            return false;
        }

        const auto select = mOnSelect;
        select();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mOnSelect != nullptr);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        return IsPressKey(k) && Activate(ctx.bounds);
    }

    /// Each lane's height: the cards' (kCardHeight), or the tiles' (kIconRowHeight).
    void RowHeight(float height)
    {
        mRowHeight = height;
    }

    /// Lanes of their own heights, top first: a lane holding a nested split is as tall as its
    /// branches. Unset, every lane is RowHeight.
    void LaneHeights(std::vector<float> heights)
    {
        mLaneHeights = std::move(heights);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = Height();
        return {{kRailWidth, h}, {kRailWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float top = b.top + std::max(0.0f, (b.height() - Height()) * 0.5f);
        const float mid = b.top + b.height() * 0.5f;
        const float x = mJoin ? b.left + 8.0f : b.right - 8.0f; // the rail
        const float outside = mJoin ? b.right : b.left;
        const float inside = mJoin ? b.left : b.right;

        cnv.stroke_style(CableColour(mTheme).opacity(0.55f));
        cnv.line_width(3.0f);
        cnv.line_join(artist::canvas::round_join);
        cnv.begin_path();
        cnv.move_to(outside, mid);
        cnv.line_to(x, mid);
        // Each lane's middle, where its cable runs.
        std::vector<float> middles;
        float laneTop = top;

        for (int i = 0; i < mLanes; ++i)
        {
            const float h = LaneHeight(i);
            middles.push_back(laneTop + h * 0.5f);
            laneTop += h + kLaneGap;
        }

        if (!middles.empty())
        {
            cnv.move_to(x, middles.front());
            cnv.line_to(x, middles.back());
        }

        for (const float y : middles)
        {
            cnv.move_to(x, y);
            cnv.line_to(inside, y);
        }

        cnv.stroke();

        const el::point c{x, mid};
        cnv.fill_style(mSelected ? mTheme.Elevated() : mTheme.Card());
        cnv.add_circle(artist::circle(c, 11.0f));
        cnv.fill();
        cnv.stroke_style(mSelected ? mTheme.Accent() : (mHover && mOnSelect ? mTheme.Text() : mTheme.BorderStrong()));
        cnv.line_width(mSelected ? 2.0f : 1.0f);
        cnv.add_circle(artist::circle(c, 11.0f));
        cnv.stroke();
        mIcons.Draw(cnv, mJoin ? "parallel-join" : "parallel-split",
                    el::rect{c.x - 7.0f, c.y - 7.0f, c.x + 7.0f, c.y + 7.0f}, mTheme.TextSecondary());
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mOnSelect || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!btn.down)
        {
            mOnSelect();
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point, el::cursor_tracking status) override
    {
        const bool hover = status != el::cursor_tracking::leaving;

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return static_cast<bool>(mOnSelect);
    }

  private:
    [[nodiscard]] float LaneHeight(int lane) const
    {
        return lane < static_cast<int>(mLaneHeights.size()) ? mLaneHeights[static_cast<std::size_t>(lane)] : mRowHeight;
    }

    [[nodiscard]] float Height() const
    {
        float height = static_cast<float>(std::max(0, mLanes - 1)) * kLaneGap;

        for (int i = 0; i < mLanes; ++i)
        {
            height += LaneHeight(i);
        }

        return height;
    }

    const Theme& mTheme;
    Icons& mIcons;
    int mLanes;
    float mRowHeight = kCardHeight;
    std::vector<float> mLaneHeights;
    bool mJoin, mSelected;
    std::function<void()> mOnSelect;
    bool mHover = false;
};

/// The end of a wrapped line's cable: in from the left and down (`out`), or in from above and
/// on to the right (the next line's start).
class WireEnd final : public el::element
{
  public:
    WireEnd(const Theme& theme, bool out) : mTheme(theme), mOut(out)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kWireEndWidth, 0.0f}, {kWireEndWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float mid = b.top + b.height() * 0.5f;
        const float x = b.left + kWireEndWidth * 0.5f;
        cnv.stroke_style(CableColour(mTheme).opacity(0.55f));
        cnv.line_width(3.0f);
        cnv.begin_path();

        if (mOut)
        {
            cnv.move_to(b.left, mid);
            cnv.arc_to(el::point{x, mid}, el::point{x, b.bottom}, 6.0f);
            cnv.line_to(x, b.bottom);
        }
        else
        {
            cnv.move_to(x, b.top);
            cnv.arc_to(el::point{x, mid}, el::point{b.right, mid}, 6.0f);
            cnv.line_to(b.right, mid);
        }

        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    bool mOut;
};

/// The cable back under a line: down from where the line ended, across, and down into the next.
class ReturnRoute final : public el::element
{
  public:
    ReturnRoute(const Theme& theme, float fromX, float toX) : mTheme(theme), mFrom(fromX), mTo(toX)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, kRouteHeight}, {el::full_extent, kRouteHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float mid = b.top + b.height() * 0.5f;
        const float from = b.left + mFrom, to = b.left + mTo;
        cnv.stroke_style(CableColour(mTheme).opacity(0.55f));
        cnv.line_width(3.0f);
        cnv.begin_path();
        cnv.move_to(from, b.top);
        cnv.arc_to(el::point{from, mid}, el::point{to, mid}, 6.0f);
        cnv.arc_to(el::point{to, mid}, el::point{to, b.bottom}, 6.0f);
        cnv.line_to(to, b.bottom);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    float mFrom, mTo;
};
} // namespace nanoq::ui
