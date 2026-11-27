#pragma once

// A node's card in the chain strip (ChainStrip.cpp), and the hit map the strip's cards and cables
// note their bounds in. It paints from the theme; the strip decides where it goes.

#include "ui/Drawn.h"
#include "ui/widgets/ControlSkin.h"
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
// The cable back under a line: with each row's 10 px either side of its cards, wrapped rows sit
// 28 apart as the web UI's (wrap.css row-gap).
constexpr float kRouteHeight = 8.0f;
constexpr float kSidePad = 12.0f;
constexpr float kPowerSize = 16.0f;
// The chain dragged short (nodes-compact.css): each card a 40 px tile with only its icon, in a
// 48 px row.
constexpr float kIconCardSize = 40.0f;
constexpr float kIconRowHeight = 48.0f;

/// The card's name, its detail and its model's badge: 10, 8 and 8 px, or with touch sizes 11, 10
/// and 10.
inline float CardNameSize()
{
    return metrics::Readable(10.0f);
}

inline float CardDetailSize()
{
    return metrics::Readable(8.0f, 10.0f);
}

inline float CardBadgeSize()
{
    return metrics::Readable(8.0f, 10.0f);
}

/// A card's width for its text, estimated from its length so the wrap can be worked out before
/// anything is measured: the 10 px capitals, spaced .06em, run about 7.2 px a letter and the 8 px
/// detail about 4.6 (in proportion at the touch sizes), inside 12 px of padding each side
/// (min-width 72, the text at most 160).
inline float CardWidthFor(const std::string& name, const std::string& detail)
{
    const auto chars = [](const std::string& s) {
        return static_cast<float>(
            std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    const float nameLetter = metrics::Touch() ? 8.0f : 7.2f, detailLetter = metrics::Touch() ? 5.8f : 4.6f;
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
        NoteDrawn(ctx, PaintArea(ctx.bounds));
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
        else if (mHover && !mDragging && !metrics::Touch())
        {
            // Hovered, it lifts a pixel (.signal-node:hover).
            cnv.translate(0.0f, -1.0f);
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

        // A theme that draws its own modules (Studio 1973's cream caps): the cap as its web UI's,
        // standing its wall proud of the tray, its border the category's unless the theme sets
        // its own. Otherwise the shared look below.
        const auto& skin = mTheme.Controls();
        const NodeSkin& nodeSkin = mLook.selected ? skin.nodes.selected : skin.nodes.plain;
        const bool skinned = nodeSkin.face.drawn;

        if (skinned)
        {
            // The edge as nodes.css mixes it: the category 72% into the cap's face (88% under the
            // pointer), full when chosen, with the chosen halo in the category's colour too.
            const std::uint32_t faceArgb = skin.backdrops.nodePlain.colour;
            const el::color capFace = (faceArgb >> 24) != 0 ? Theme::Argb(faceArgb) : face;
            const el::color category = mLook.missing    ? el::rgba(232, 76, 76, 153)
                                       : mLook.selected ? tint
                                                        : Mix(tint, capFace, mHover ? 0.12f : 0.28f);
            const bool own = !nodeSkin.fixedBorder || mLook.missing;
            const std::uint32_t border = own ? Theme::ArgbOf(ghost(category)) : 0u;
            const std::uint32_t ring =
                own ? Theme::ArgbOf(tint.opacity(Theme::Argb(nodeSkin.face.box.ringColour).alpha)) : 0u;
            const el::rect cap{r.left, r.top, r.right, r.bottom + nodeSkin.face.wall};
            DrawKey(cnv, mTheme, cap, nodeSkin.face,
                    mLook.selected ? skin.backdrops.nodeSelected : skin.backdrops.nodePlain, mHover && !mLook.selected,
                    false, border, ring);

            if (!on)
            {
                // Bypassed: faded toward the tray, as the web UI's opacity.
                cnv.fill_style(mTheme.Bar().opacity(1.0f - fade));
                cnv.fill_round_rect(cap, nodeSkin.face.box.radius);
            }
        }
        else
        {
            // The web UI's raised card (--node-raised): a drop shadow, a 3 px wall of the face
            // darkened 38% over a 1 px edge, then the face with its sheen. No category wash.
            const el::rect wall{r.left, r.top + 4.0f, r.right, r.bottom + 4.0f};
            {
                auto state = cnv.new_state();
                cnv.shadow_style(el::point{0.0f, 4.0f}, 12.0f, el::rgba(0, 0, 0, on ? 70 : 30));
                cnv.fill_style(ghost(el::rgba(0, 0, 0, 77)));
                cnv.fill_round_rect(wall, kRadius);
            }
            cnv.fill_style(ghost(Mix(el::rgba(0, 0, 0, 255), face, 0.38f)));
            cnv.fill_round_rect(el::rect{r.left, r.top + 3.0f, r.right, r.bottom + 3.0f}, kRadius);
            cnv.fill_style(ghost(face));
            cnv.fill_round_rect(r, kRadius);
            {
                auto state = cnv.new_state();
                artist::canvas::linear_gradient sheen{r.top_left(), r.bottom_left()};
                sheen.add_color_stop(0.0f, el::rgba(255, 255, 255, static_cast<std::uint8_t>(15 * fade)));
                sheen.add_color_stop(0.42f, el::rgba(255, 255, 255, 0));
                sheen.add_color_stop(1.0f, el::rgba(0, 0, 0, 10));
                cnv.fill_style(sheen);
                cnv.fill_round_rect(r, kRadius);
            }

            // The border: the category colour 72% into the face, 88% under the pointer, full when
            // chosen with a 2 px halo of it outside; red when its file is missing. Dashed when off.
            const el::color border = mLook.missing    ? el::rgba(232, 76, 76, 153)
                                     : mLook.selected ? tint
                                                      : Mix(tint, face, mHover ? 0.88f : 0.72f);
            cnv.stroke_style(ghost(border));
            cnv.line_width(1.0f);

            if (on)
            {
                cnv.stroke_round_rect(r.inset(0.5f, 0.5f), kRadius - 0.5f);
            }
            else
            {
                StrokeDashedRoundRect(cnv, r.inset(0.5f, 0.5f), kRadius - 0.5f, 4.0f, 3.0f);
            }

            if (mLook.selected || mLook.missing)
            {
                const float halo = mLook.selected ? 2.0f : 1.0f;
                cnv.stroke_style(mLook.selected ? tint.opacity(0.24f) : el::rgba(232, 76, 76, 89));
                cnv.line_width(halo);
                cnv.stroke_round_rect(r.inset(-halo * 0.5f, -halo * 0.5f), kRadius + halo * 0.5f);
            }
        }

        // A dragged card would land before this one (.signal-node.drag-over): the drop colour's
        // edge and a 12 px dot at its left; a card held or dragged is edged in the accent.
        if (mHits && !mNodeId.empty() && mHits->hoverNode == mNodeId)
        {
            const el::color drop = el::rgba(72, 224, 120, 255);
            cnv.stroke_style(drop);
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(r.inset(0.5f, 0.5f), kRadius - 0.5f);
            const el::point dot{r.left - 4.0f, (r.top + r.bottom) * 0.5f};
            cnv.fill_style(drop.opacity(0.24f));
            cnv.add_circle(artist::circle(dot, 9.0f));
            cnv.fill();
            cnv.fill_style(drop);
            cnv.add_circle(artist::circle(dot, 6.0f));
            cnv.fill();
        }
        else if (lifted || mDragging)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(r.inset(0.5f, 0.5f), kRadius - 0.5f);
        }

        // The highlight along the top edge (a skinned cap has its own).
        if (!skinned)
        {
            cnv.stroke_style(el::rgba(255, 255, 255, 255).opacity(0.1f * fade));
            cnv.line_width(1.0f);
            cnv.begin_path();
            cnv.move_to(r.left + kRadius, r.top + 1.5f);
            cnv.line_to(r.right - kRadius, r.top + 1.5f);
            cnv.stroke();
        }

        // Icon-only: the icon fills the tile, or the artwork covers the whole of it inside the
        // border, with no name, effect or badge (nodes-compact.css).
        if (mLook.iconOnly)
        {
            const auto c = artist::center_point(r);

            if (mLook.thumbnail)
            {
                const float edge = mLook.selected ? 2.0f : 1.0f;
                const el::rect picture = r.inset(edge, edge);
                auto state = cnv.new_state();
                cnv.begin_path();
                cnv.add_round_rect(picture, kRadius - edge);
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

            // The picture: the artwork alone over the top edge, or the icon in a round badge of the
            // category colour.
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
                y += 24.0f + 4.0f;
            }
            else
            {
                const el::point c{cx, y + 15.0f};
                auto state = cnv.new_state();

                if (skinned)
                {
                    // The theme's well for the icon (Studio 1973's dark jewel), under the lit badge.
                    DrawControlBox(cnv, el::rect{c.x - 15.0f, c.y - 15.0f, c.x + 15.0f, c.y + 15.0f}, nodeSkin.icon,
                                   1.0f, 15.0f);
                }

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
            // A skinned module's name in its theme's lettering (Studio 1973's Fraunces).
            const el::color ink = ghost(skinned ? Theme::Argb(nodeSkin.name.colour) : mTheme.Text());
            const auto nameFont =
                skinned ? ControlFont(nodeSkin.name, CardNameSize()) : Font(Weight::Medium).size(CardNameSize());
            const auto detailFont = Font(Weight::Medium).size(CardDetailSize());
            const float textWidth = std::min(160.0f, r.width() - 16.0f);
            const float nameRow = metrics::Touch() ? 13.0f : 12.0f;
            // .node-name: spaced .06em.
            constexpr float kNameTracking = 0.06f;
            const auto upperName = Upper(mLook.name);
            const float nameTracking =
                kNameTracking * CardNameSize() * static_cast<float>(std::min<std::size_t>(upperName.size(), 32));
            FillTrackedText(cnv, Ellipsize(cnv, upperName, nameFont, std::max(0.0f, textWidth - nameTracking)),
                            nameFont, ink, el::rect{r.left + 6.0f, y, r.right - 6.0f, y + nameRow}, HAlign::Centre,
                            kNameTracking);
            y += nameRow + 2.0f;

            if (!mLook.detail.empty())
            {
                FillText(cnv, Ellipsize(cnv, mLook.detail, detailFont, textWidth), detailFont,
                         ghost(skinned ? Theme::Argb(nodeSkin.detail) : Mix(tint, mTheme.TextMuted(), 0.26f)),
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

        // The signal light (.node-clip-indicator): 6 px, 5 px in from the bottom right; a lamp
        // with a highlight when there is signal (green) or clipping (red), grey at .6 with none.
        // It stays lit on a card that is off, faded with it.
        if (mLight != Light::None)
        {
            const el::point c{r.right - 8.0f, r.bottom - 8.0f};
            auto state = cnv.new_state();

            if (mLight == Light::Idle)
            {
                cnv.fill_style(ghost(mTheme.TextMuted().opacity(0.6f)));
            }
            else
            {
                const bool clip = mLight == Light::Clip;
                const el::color lamp = clip ? el::rgba(255, 92, 92, 255) : el::rgba(72, 224, 120, 255);
                cnv.shadow_style(el::point{0.0f, 0.0f}, 6.0f, lamp.opacity((clip ? 0.8f : 0.6f) * fade));
                const el::point spot{c.x - 0.9f, c.y - 1.2f};
                artist::canvas::radial_gradient glass{spot, 0.0f, spot, 4.2f};
                glass.add_color_stop(
                    0.0f, (clip ? el::rgba(255, 208, 208, 255) : el::rgba(216, 255, 228, 255)).opacity(fade));
                glass.add_color_stop(0.45f, lamp.opacity(fade));
                glass.add_color_stop(1.0f,
                                     (clip ? el::rgba(192, 40, 40, 255) : el::rgba(32, 160, 80, 255)).opacity(fade));
                cnv.fill_style(glass);
            }

            cnv.add_circle(artist::circle(c, 3.0f));
            cnv.fill();
        }

        // A small boxed tag (.node-bypass-badge, .node-missing-badge) with its right edge at `right`.
        const auto tag = [&](const char* text, float size, float right, float top, el::color ink, el::color fill,
                             el::color edge, float padX, float padY) {
            const auto font = Font(Weight::SemiBold).size(size);
            const float w = el::measure_text(cnv, text, font).x + 2.0f * padX;
            const el::rect box{right - w, top, right, top + size + 2.0f * padY + 1.0f};
            cnv.fill_style(fill);
            cnv.fill_round_rect(box, 3.0f);

            if (edge.alpha > 0.0f)
            {
                cnv.stroke_style(edge);
                cnv.line_width(1.0f);
                cnv.stroke_round_rect(box.inset(0.5f, 0.5f), 3.0f);
            }

            FillText(cnv, text, font, ink, box, HAlign::Centre);
        };

        // Off: a red OFF at the top right, under the power (.node-bypass-badge).
        if (!on && !mLook.iconOnly && !boundary)
        {
            tag("OFF", 7.0f, r.right - 2.0f, r.top + 20.0f, el::rgba(255, 102, 102, 255), el::rgba(255, 102, 102, 51),
                el::rgba(0, 0, 0, 0), 4.0f, 2.0f);
        }

        // Its file is missing: a red boxed warning sign at the top right, or on a tile at the
        // bottom left (.node-missing-badge).
        if (mLook.missing)
        {
            const float right = mLook.iconOnly ? r.left + 18.0f : r.right - 2.0f;
            const float top = mLook.iconOnly ? r.bottom - 16.0f : r.top + 18.0f + (on ? 0.0f : 14.0f);
            tag("\xE2\x9A\xA0", 10.0f, right, top, el::rgba(255, 138, 138, 255), el::rgba(232, 76, 76, 51),
                el::rgba(232, 76, 76, 102), 4.0f, 1.0f);
        }

        // The corners (.signal-node-delete, .signal-node-bypass): remove at the top left and the
        // power at the top right, shown while the card is hovered or chosen; the power also
        // while the card is off. With touch sizes a hover is only a tap on its way, as the web
        // UI's (hover: none) rule has it, so only the chosen card shows them.
        const auto corner = [&](el::rect at, el::color fill, el::color ring) {
            const auto c = artist::center_point(at);
            cnv.fill_style(fill);
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
            // Red under the pointer (.signal-node-delete:hover).
            const auto x = RemoveRect(r);
            corner(x, mHoverRemove ? el::rgba(255, 80, 80, 64) : el::rgba(0, 0, 0, 64),
                   mHoverRemove ? el::rgba(255, 80, 80, 89) : el::rgba(255, 255, 255, 38));
            const auto c = artist::center_point(x);
            cnv.stroke_style(mHoverRemove ? el::rgba(255, 255, 255, 242) : el::rgba(255, 255, 255, 205));
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
            // Green under the pointer, red while off (.signal-node-bypass), a 10 px glyph.
            const auto p = PowerRect(r);
            const el::color fill = !on           ? el::rgba(255, 80, 80, 51)
                                   : mHoverPower ? el::rgba(116, 228, 158, 51)
                                                 : el::rgba(0, 0, 0, 64);
            const el::color ring = !on           ? el::rgba(255, 80, 80, 89)
                                   : mHoverPower ? el::rgba(116, 228, 158, 102)
                                                 : el::rgba(255, 255, 255, 38);
            corner(p, fill, ring);
            mIcons.Draw(cnv, "output", p.inset(3.0f, 3.0f),
                        !on           ? el::rgba(255, 170, 170, 250)
                        : mHoverPower ? el::rgba(196, 255, 216, 250)
                                      : el::rgba(160, 236, 186, 242));
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
            ctx.view.refresh(ctx, PaintArea(ctx.bounds));
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

    /// How far a tile's corner buttons stand out past the tile.
    static constexpr float kTileCornerProud = 5.0f;

    /// All of what the card draws, for a redraw to cover: its bounds, and round a tile the
    /// corner buttons that stand out past them (and a pixel for their edge). A redraw of the
    /// bounds alone drew a hovered tile's buttons cut off at its sides.
    [[nodiscard]] el::rect PaintArea(const el::rect& bounds) const
    {
        return mLook.iconOnly ? bounds.inset(-(kTileCornerProud + 1.0f), -(kTileCornerProud + 1.0f)) : bounds;
    }

    [[nodiscard]] el::rect PowerRect(const el::rect& card) const
    {
        // On a tile it sits over the corner, clear of the icon.
        if (mLook.iconOnly)
        {
            return el::rect{card.right + kTileCornerProud - kPowerSize, card.top - kTileCornerProud,
                            card.right + kTileCornerProud, card.top - kTileCornerProud + kPowerSize};
        }

        return el::rect{card.right - 2.0f - kPowerSize, card.top + 2.0f, card.right - 2.0f,
                        card.top + 2.0f + kPowerSize};
    }

    [[nodiscard]] el::rect RemoveRect(const el::rect& card) const
    {
        if (mLook.iconOnly)
        {
            return el::rect{card.left - kTileCornerProud, card.top - kTileCornerProud,
                            card.left - kTileCornerProud + kPowerSize, card.top - kTileCornerProud + kPowerSize};
        }

        return el::rect{card.left + 2.0f, card.top + 2.0f, card.left + 2.0f + kPowerSize, card.top + 2.0f + kPowerSize};
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
