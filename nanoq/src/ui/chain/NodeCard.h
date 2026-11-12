#pragma once

// A node's card in the chain strip (ChainStrip.cpp), and the hit map the strip's cards and cables
// note their bounds in. It paints from the theme; the strip decides where it goes.

#include "ui/Drawn.h"
#include "ui/widgets/DragScroll.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Layout.h"
#include "ui/widgets/LongPress.h"
#include "ui/widgets/TouchHold.h"
#include "ui/widgets/Widgets.h"

#include <algorithm>
#include <chrono>
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

/// The card's name, its detail and its model's badge: 10, 9 and 8 px, or with touch sizes 11, 10
/// and 10.
inline float CardNameSize()
{
    return metrics::Readable(10.0f);
}

inline float CardDetailSize()
{
    return metrics::Readable(9.0f, 10.0f);
}

inline float CardBadgeSize()
{
    return metrics::Readable(8.0f, 10.0f);
}

/// A card's width for its text, estimated from its length so the wrap can be worked out before
/// anything is measured: the 10 px capitals run about 6.6 px a letter and the 9 px detail about
/// 5.2 (in proportion at the touch sizes), inside 12 px of padding each side (min-width 72, the
/// text at most 160).
inline float CardWidthFor(const std::string& name, const std::string& detail)
{
    const auto chars = [](const std::string& s) {
        return static_cast<float>(
            std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    const float nameLetter = metrics::Touch() ? 7.3f : 6.6f, detailLetter = metrics::Touch() ? 5.8f : 5.2f;
    const float wanted = std::min(160.0f, std::max(chars(name) * nameLetter, chars(detail) * detailLetter)) + 24.0f;
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
class NodeCard final : public el::element, public Drawn, public Focusable, public TapTarget
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
        return RunFromCopy(onSelect);
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
            return RunFromCopy(onMenu);
        }

        if (k.action != el::key_action::press)
        {
            return false;
        }

        const bool command = (k.modifiers & (el::mod_control | el::mod_action)) != 0;

        if ((k.key == key_code::enter || k.key == key_code::kp_enter) && command)
        {
            return RunFromCopy(onReplace);
        }

        if (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super))
        {
            return false;
        }

        switch (k.key)
        {
        case key_code::enter:
        case key_code::kp_enter:
            return RunFromCopy(onSelect);
        case key_code::space:
            return mLook.canBypass && RunFromCopy(onToggle);
        case key_code::_delete:
            return mLook.canRemove && RunFromCopy(onRemove);
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

    /// With touch sizes a finger rests on the card before it moves it (TouchHold.h): a swipe
    /// sooner scrolls the chain. A mouse moves it at once, and the chain scrolls from its gaps.
    [[nodiscard]] bool HoldToDrag() const override
    {
        return metrics::Touch() && static_cast<bool>(onDrag);
    }

    [[nodiscard]] bool GivesWay() const override
    {
        return HoldToDrag();
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

        // Picked up by a finger's hold and not yet moved: it lifts, so the finger knows.
        const bool lifted = Lifted();

        if (lifted)
        {
            const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
            cnv.translate(cx, cy);
            cnv.scale(1.06f, 1.06f);
            cnv.translate(-cx, -cy);
        }

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

        // A dragged card would land after this one, or a held one is picked up: an accent ring.
        if (lifted || (mHits && !mNodeId.empty() && mHits->hoverNode == mNodeId))
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
            const auto nameFont = Font(Weight::Medium).size(CardNameSize());
            const auto detailFont = Font(Weight::Medium).size(CardDetailSize());
            const float textWidth = std::min(160.0f, r.width() - 16.0f);
            const float nameRow = metrics::Touch() ? 13.0f : 12.0f;
            FillText(cnv, Ellipsize(cnv, Upper(mLook.name), nameFont, textWidth), nameFont, ink,
                     el::rect{r.left + 6.0f, y, r.right - 6.0f, y + nameRow}, HAlign::Centre);
            y += nameRow + 2.0f;

            if (!mLook.detail.empty())
            {
                FillText(cnv, Ellipsize(cnv, mLook.detail, detailFont, textWidth), detailFont,
                         ghost(Mix(tint, mTheme.TextMuted(), 0.26f)),
                         el::rect{r.left + 6.0f, y, r.right - 6.0f, y + 11.0f}, HAlign::Centre);
                y += 13.0f;
            }

            if (!mLook.badge.empty())
            {
                const auto badgeFont = Font(Weight::SemiBold).size(CardBadgeSize());
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
        // while the card is off. With touch sizes a hover is only a tap on its way, as the web
        // UI's (hover: none) rule has it, so only the chosen card shows them.
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
        const bool showCorners = mLook.selected || (mHover && !metrics::Touch());

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
            // A corner takes the press only where it is drawn: unseen, a tap near it selected
            // nothing and turned the effect off (the power), or removed it.
            mDown = true;
            mDownOnPower =
                mLook.canBypass && (CornersLive() || !mLook.enabled) && CornerHit(PowerRect(r)).includes(btn.pos);
            mDownOnRemove =
                !mDownOnPower && mLook.canRemove && CornersLive() && CornerHit(RemoveRect(r)).includes(btn.pos);
            mDownAt = btn.pos;
            mDragging = false;
            mHoldMissed = false;

            if (!mDownOnPower && !mDownOnRemove && HoldToDrag())
            {
                TouchHold::Press(btn.pos.x, btn.pos.y);
            }

            // Held, a finger's press opens the card's menu, as a right click does (LongPress.h).
            if (!mDownOnPower && !mDownOnRemove && onMenu)
            {
                LongPress::Press(btn.pos, onMenu);
            }

            return true;
        }

        const bool wasDown = mDown;
        mDown = false;
        mHoldMissed = false;

        if (LongPress::Fired())
        {
            mDragging = false;
            mDragOffset = {0.0f, 0.0f};
            ctx.view.refresh();
            return true;
        }

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

        // With touch sizes only a held card moves: one moved sooner was a swipe of the chain, and
        // stays put for the rest of the press even once the hold would have been up.
        if (!mDragging && HoldToDrag() && (mHoldMissed || !TouchHold::Held()))
        {
            mHoldMissed = true;
            return;
        }

        // Moved, it is a drag and not a hold.
        LongPress::Cancel();
        mDragging = true;
        mDragOffset = offset;
        onDrag(mDownAt, btn.pos, false);
        ctx.view.refresh();
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const auto r = CardRect(ctx.bounds, CardHeight());
        const bool power = inside && mLook.canBypass && CornerHit(PowerRect(r)).includes(p);
        const bool remove = inside && mLook.canRemove && CornerHit(RemoveRect(r)).includes(p);

        if (inside && !mHover)
        {
            mHoverSince = std::chrono::steady_clock::now();
        }

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
    /// Whether the corners take a press: drawn on the chosen card, and on a hovered one once the
    /// pointer has rested there long enough for them to be seen. A tap reaches the card as a
    /// hover and a press together (Windows' touch, the DevDriver), and has seen nothing.
    [[nodiscard]] bool CornersLive() const
    {
        if (mLook.selected)
        {
            return true;
        }

        constexpr std::chrono::milliseconds kSeen{120};
        return mHover && !metrics::Touch() && std::chrono::steady_clock::now() - mHoverSince >= kSeen;
    }

    /// A corner's hit area: 4 px round it, or with touch sizes a 36 px square on its centre.
    [[nodiscard]] static el::rect CornerHit(const el::rect& corner)
    {
        if (metrics::Touch())
        {
            const auto c = artist::center_point(corner);
            return el::rect{c.x - 18.0f, c.y - 18.0f, c.x + 18.0f, c.y + 18.0f};
        }

        return corner.inset(-4.0f, -4.0f);
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
    bool mHoldMissed = false; // moved before a finger's hold took: not a drag this press
    std::chrono::steady_clock::time_point mHoverSince;

    [[nodiscard]] bool Lifted() const
    {
        return mDown && !mDragging && !mHoldMissed && HoldToDrag() && TouchHold::Held();
    }
};
} // namespace nanoq::ui
