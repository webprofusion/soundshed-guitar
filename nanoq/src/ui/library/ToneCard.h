#pragma once

// One tone on the Tones page (TonesPage.cpp), as a card or a row.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <ctime>

namespace nanoq::ui::tone_cards
{
namespace tones = guitarfx::uiclient::tones;

inline constexpr float kCardGap = 10.0f;
inline constexpr float kPresetCardMin = 196.0f; // the grid's minmax(168px, 1fr), at the compact density
inline constexpr float kPresetCardHeight = 156.0f;
inline constexpr float kPackCardWidth = 172.0f;
inline constexpr float kPackCardHeight = 206.0f;
inline constexpr float kActionBar = 38.0f;

/// The card's accent where it has no picture: a hue from its id, as the web UI's.
inline el::color AccentFor(const std::string& id)
{
    const auto hash = std::hash<std::string>{}(id);
    const float hue = static_cast<float>(hash % 360) / 360.0f;
    const auto channel = [hue](float offset) {
        float h = hue + offset;
        h -= static_cast<float>(static_cast<int>(h));
        const float x = std::abs(h * 6.0f - 3.0f) - 1.0f;
        return std::clamp(x, 0.0f, 1.0f) * 0.55f + 0.2f;
    };
    return el::color{channel(0.0f), channel(2.0f / 3.0f), channel(1.0f / 3.0f), 1.0f};
}

/// Up to `maxLines` lines of `text` in `width`, the last cut with an ellipsis.
inline std::vector<std::string> Wrap(artist::canvas& cnv, std::string text, const el::font_descr& font, float width,
                                     int maxLines)
{
    // A card shows running text: line breaks in it are spaces.
    std::replace_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    return WrapLines(cnv, text, font, width, maxLines);
}

/// A small rounded label on a card (.tone-sharing-chip, .tag-badge).
inline float DrawChip(artist::canvas& cnv, const std::string& text, float x, float y, el::color ink, el::color fill)
{
    const auto font = Font(Weight::SemiBold).size(9.0f);
    const float w = el::measure_text(cnv, text, font).x + 12.0f;
    const el::rect r{x, y, x + w, y + 15.0f};
    cnv.fill_style(fill);
    cnv.fill_round_rect(r, 7.5f);
    FillText(cnv, text, font, ink, r, HAlign::Centre);
    return w;
}

/// One tone as a card: its picture or accent, title, chips, description, tags and creator over
/// a bar of actions. Pictures are fetched as the card is drawn, and drawn once they arrive. With
/// focus Left and Right pick an action (lit as under the pointer), and Enter or Space runs it.
class ToneCard final : public el::element, public Focusable, public TapTarget
{
  public:
    struct Action
    {
        std::string icon;
        bool primary = false; // the accent's filled download button
        bool enabled = true;
        std::string label; // drawn instead of an icon ("Installed")
        std::function<void()> onClick;
    };

    ToneCard(const Theme& theme, Icons& icons, tones::Tone tone,
             std::function<std::shared_ptr<artist::image>(const std::string&)> image, float width, float height)
        : mTheme(theme), mIcons(icons), mTone(std::move(tone)), mImage(std::move(image)), mWidth(width), mHeight(height)
    {
    }

    std::vector<Action> actions;
    std::vector<std::string> lines; // extra lines (Installed's details) in place of chips and tags
    bool row = false;               // the List layout's row (.tone-sharing-feed.is-list) rather than a card

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;
        info.name = NameOr(mTone.title);
        const int at = KeyAction();

        if (at >= 0)
        {
            const auto& a = actions[static_cast<std::size_t>(at)];
            info.value = a.label.empty() ? IconButton::IconMeaning(a.icon) : a.label;
        }

        return info;
    }

    bool Activate(const el::rect&) override
    {
        const int at = KeyAction();

        if (at < 0 || !actions[static_cast<std::size_t>(at)].onClick)
        {
            return false;
        }

        const auto run = actions[static_cast<std::size_t>(at)].onClick; // it may rebuild the list
        run();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(KeyAction() >= 0);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        mHoverAction = -1;
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if ((k.action == el::key_action::press || k.action == el::key_action::repeat) &&
            (k.key == el::key_code::left || k.key == el::key_code::right))
        {
            const int step = k.key == el::key_code::right ? 1 : -1;

            for (int i = KeyAction() + step; i >= 0 && i < static_cast<int>(actions.size()); i += step)
            {
                if (actions[static_cast<std::size_t>(i)].enabled && actions[static_cast<std::size_t>(i)].onClick)
                {
                    mKeyAction = i;
                    mHoverAction = i;
                    ctx.view.refresh(ctx);
                    break;
                }
            }

            return true;
        }

        return IsPressKey(k) && Activate(ctx.bounds);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mWidth, mHeight}, {mWidth, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        if (row)
        {
            DrawRow(ctx);
            return;
        }

        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        constexpr float kRadius = 10.0f;
        const auto accent = AccentFor(mTone.id);
        const auto picture = mImage ? mImage(mTone.thumbnailUrl) : nullptr;
        const el::rect face{b.left, b.top, b.right, b.bottom - kActionBar};

        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, kRadius);
            cnv.clip();
            cnv.fill_style(mTheme.Pick(el::rgba(30, 30, 38, 255), mTheme.Card()));
            cnv.fill_rect(b);

            if (picture)
            {
                DrawImageCovering(cnv, *picture, face);
                artist::canvas::linear_gradient shade{face.top_left(), face.bottom_left()};
                // The light theme fades the picture into the card instead (.tone-sharing-pack-hero).
                shade.add_color_stop(0.0f, mTheme.Pick(el::rgba(12, 12, 16, 200), el::rgba(235, 238, 245, 240)));
                shade.add_color_stop(1.0f, mTheme.Pick(el::rgba(12, 12, 16, 110), el::rgba(235, 238, 245, 205)));
                cnv.fill_style(shade);
                cnv.fill_rect(face);
            }
            else
            {
                artist::canvas::linear_gradient wash{face.top_left(), face.bottom_left()};
                wash.add_color_stop(0.0f, accent.opacity(0.22f));
                wash.add_color_stop(1.0f, accent.opacity(0.0f));
                cnv.fill_style(wash);
                cnv.fill_rect(face);
            }

            cnv.fill_style(mTheme.Pick(el::rgba(20, 20, 26, 255), mTheme.Card()));
            cnv.fill_rect(el::rect{b.left, face.bottom, b.right, b.bottom});
        }
        cnv.stroke_style(mTheme.Pick(mHover ? el::rgba(255, 255, 255, 40) : el::rgba(255, 255, 255, 18),
                                     mHover ? mTheme.BorderStrong() : mTheme.Border()));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, kRadius);

        // Title (two lines at most), then what kind it is.
        const float x = b.left + 9.0f, w = b.width() - 18.0f;
        float y = b.top + 9.0f;
        const auto titleFont = Font(Weight::SemiBold).size(11.0f);

        for (const auto& line : Wrap(cnv, mTone.title, titleFont, w, 2))
        {
            FillText(cnv, line, titleFont, mTheme.Ink(), el::rect{x, y, x + w, y + 14.0f}, HAlign::Left);
            y += 14.0f;
        }

        y += 3.0f;

        const auto chipFill = mTheme.Pick(el::rgba(255, 255, 255, 22), mTheme.Panel());

        if (!lines.empty())
        {
            const auto font = Font(Weight::Regular).size(10.0f);

            for (const auto& line : lines)
            {
                FillText(cnv, Ellipsize(cnv, line, font, w), font, mTheme.TextSecondary(),
                         el::rect{x, y, x + w, y + 13.0f}, HAlign::Left);
                y += 15.0f;
            }
        }
        else
        {
            float cx = x;

            if (mTone.IsPack())
            {
                cx += DrawChip(cnv, "pack", cx, y, mTheme.TextSecondary(), chipFill) + 4.0f;
            }
            else if (!mTone.type.empty() && mTone.type != "preset")
            {
                cx += DrawChip(cnv, mTone.type, cx, y, mTheme.TextSecondary(), chipFill) + 4.0f;
            }

            if (mTone.downloads >= 0 && !mTone.IsPack())
            {
                DrawChip(cnv, tones::FormatCount(mTone.downloads) + (mTone.downloads == 1 ? " download" : " downloads"),
                         cx, y, mTheme.TextSecondary(), chipFill);
            }

            y += 21.0f;

            if (!mTone.description.empty())
            {
                const auto font = Font(Weight::Regular).size(10.0f);

                for (const auto& line : Wrap(cnv, mTone.description, font, w, mTone.IsPack() ? 3 : 1))
                {
                    FillText(cnv, line, font, mTheme.TextSecondary(), el::rect{x, y, x + w, y + 13.0f}, HAlign::Left);
                    y += 13.0f;
                }

                y += 5.0f;
            }

            if (!mTone.IsPack() && !mTone.tags.empty())
            {
                float tx = x;

                for (const auto& tag : mTone.tags)
                {
                    const float cw = el::measure_text(cnv, tag, Font(Weight::SemiBold).size(9.0f)).x + 12.0f;

                    if (tx + cw > x + w)
                    {
                        break;
                    }

                    tx += DrawChip(cnv, tag, tx, y, mTheme.TextSecondary(),
                                   mTheme.Pick(el::rgba(255, 255, 255, 14), mTheme.Panel())) +
                          4.0f;
                }
            }
        }

        // The creator, at the foot of the face: an avatar and the name.
        if (!mTone.creatorName.empty())
        {
            const float cy = face.bottom - 14.0f;
            const el::point c{x + 8.0f, cy};
            const auto avatar = mImage ? mImage(mTone.creatorAvatarUrl) : nullptr;

            if (avatar)
            {
                auto state = cnv.new_state();
                cnv.add_circle(artist::circle(c, 8.0f));
                cnv.clip();
                DrawImageCovering(cnv, *avatar, el::rect{c.x - 8.0f, c.y - 8.0f, c.x + 8.0f, c.y + 8.0f});
            }
            else
            {
                cnv.fill_style(mTheme.Pick(el::rgba(255, 255, 255, 30), mTheme.Text().opacity(0.12f)));
                cnv.add_circle(artist::circle(c, 8.0f));
                cnv.fill();
                FillText(cnv, mTone.creatorName.substr(0, 1), Font(Weight::SemiBold).size(9.0f), mTheme.Ink(),
                         el::rect{c.x - 8.0f, c.y - 8.0f, c.x + 8.0f, c.y + 8.0f}, HAlign::Centre);
            }

            const auto font = Font(Weight::SemiBold).size(10.0f);
            FillText(cnv, Ellipsize(cnv, mTone.creatorName, font, w - 22.0f), font, mTheme.Ink(),
                     el::rect{x + 21.0f, cy - 7.0f, x + w, cy + 7.0f}, HAlign::Left);
        }

        DrawActions(cnv, b);
    }

    /// One line: the title and its chip at the left, the creator and the actions at the right,
    /// over the pack's picture, faded.
    void DrawRow(const el::context& ctx)
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        constexpr float kRadius = 6.0f;
        const auto picture = mImage ? mImage(mTone.thumbnailUrl) : nullptr;

        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, kRadius);
            cnv.clip();
            cnv.fill_style(mTheme.Pick(el::rgba(30, 30, 38, 255), mTheme.Card()));
            cnv.fill_rect(b);

            if (picture)
            {
                DrawImageCovering(cnv, *picture, b);
                cnv.fill_style(mTheme.Pick(el::rgba(20, 20, 26, 205), el::rgba(235, 238, 245, 225)));
                cnv.fill_rect(b);
            }
        }

        cnv.stroke_style(mTheme.Pick(mHover ? el::rgba(255, 255, 255, 40) : el::rgba(255, 255, 255, 14),
                                     mHover ? mTheme.BorderStrong() : mTheme.Border()));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, kRadius);

        const float cy = b.top + b.height() * 0.5f;
        const float actionsLeft = actions.empty() ? b.right - 8.0f : ActionRect(b, 0).left - 10.0f;
        float creatorLeft = actionsLeft;

        if (!mTone.creatorName.empty())
        {
            const auto font = Font(Weight::SemiBold).size(10.5f);
            const float w = std::min(MeasureTextWidth(mTone.creatorName, font), 160.0f);
            creatorLeft = actionsLeft - w;
            FillText(cnv, Ellipsize(cnv, mTone.creatorName, font, w), font, mTheme.Ink(),
                     el::rect{creatorLeft, cy - 8.0f, actionsLeft, cy + 8.0f}, HAlign::Right);
            creatorLeft -= 12.0f;
        }

        const auto titleFont = Font(Weight::SemiBold).size(12.5f);
        const float x = b.left + 10.0f;
        const auto title = Ellipsize(cnv, mTone.title, titleFont, std::max(40.0f, (creatorLeft - x) * 0.7f));
        const float titleWidth = MeasureTextWidth(title, titleFont);
        FillText(cnv, title, titleFont, mTheme.Ink(), el::rect{x, cy - 9.0f, x + titleWidth + 2.0f, cy + 9.0f},
                 HAlign::Left);
        const auto chipFill = mTheme.Pick(el::rgba(255, 255, 255, 22), mTheme.Panel());
        const float chipX = x + titleWidth + 10.0f;

        if (mTone.IsPack())
        {
            DrawChip(cnv, "pack", chipX, cy - 8.0f, mTheme.TextSecondary(), chipFill);
        }
        else if (mTone.downloads >= 0 && chipX + 90.0f < creatorLeft)
        {
            DrawChip(cnv, tones::FormatCount(mTone.downloads) + (mTone.downloads == 1 ? " download" : " downloads"),
                     chipX, cy - 8.0f, mTheme.TextSecondary(), chipFill);
        }

        DrawActions(cnv, b);
    }

    void DrawActions(artist::canvas& cnv, const el::rect& b)
    {
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            const auto r = ActionRect(b, i);
            const auto& a = actions[i];
            const bool hover = static_cast<int>(i) == mHoverAction && a.enabled;
            el::color fill = a.primary ? mTheme.Accent().level(hover ? 1.08f : 0.95f)
                                       : mTheme.Pick(el::rgba(255, 255, 255, hover ? 34 : 18),
                                                     mTheme.Text().opacity(hover ? 0.12f : 0.06f));

            if (!a.enabled)
            {
                fill = mTheme.Pick(el::rgba(255, 255, 255, 10), mTheme.Text().opacity(0.04f));
            }

            cnv.fill_style(fill);
            cnv.fill_round_rect(r, 6.0f);
            const auto ink = a.primary && a.enabled ? el::rgba(255, 255, 255, 255)
                                                    : (a.enabled ? mTheme.TextSecondary() : mTheme.TextMuted());

            if (!a.label.empty())
            {
                FillText(cnv, a.label, Font(Weight::SemiBold).size(9.5f), ink, r, HAlign::Centre);
            }
            else
            {
                mIcons.Draw(cnv, a.icon, r.inset(6.5f, 6.5f), ink);
            }
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        const int hit = HitAction(ctx.bounds, btn.pos);

        if (!btn.down && hit >= 0 && actions[static_cast<std::size_t>(hit)].enabled &&
            actions[static_cast<std::size_t>(hit)].onClick)
        {
            actions[static_cast<std::size_t>(hit)].onClick();
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const int hit = inside ? HitAction(ctx.bounds, p) : -1;

        if (inside != mHover || hit != mHoverAction)
        {
            mHover = inside;
            mHoverAction = hit;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    /// The action Enter runs: the one picked, else the filled one, else the first that does
    /// anything; -1 for none.
    [[nodiscard]] int KeyAction() const
    {
        const auto usable = [this](int i) {
            return i >= 0 && i < static_cast<int>(actions.size()) && actions[static_cast<std::size_t>(i)].enabled &&
                   actions[static_cast<std::size_t>(i)].onClick;
        };

        if (usable(mKeyAction))
        {
            return mKeyAction;
        }

        int first = -1;

        for (int i = 0; i < static_cast<int>(actions.size()); ++i)
        {
            if (usable(i))
            {
                if (actions[static_cast<std::size_t>(i)].primary)
                {
                    return i;
                }

                first = first < 0 ? i : first;
            }
        }

        return first;
    }

    el::rect ActionRect(const el::rect& b, std::size_t i) const
    {
        const auto width = [this](std::size_t j) { return actions[j].label.empty() ? 26.0f : 70.0f; };

        // A row's sit at its right, the last against the edge.
        if (row)
        {
            float right = b.right - 4.0f;

            for (std::size_t j = actions.size() - 1; j > i; --j)
            {
                right -= width(j) + 4.0f;
            }

            const float cy = b.top + b.height() * 0.5f;
            return el::rect{right - width(i), cy - 12.0f, right, cy + 12.0f};
        }

        // Square icon buttons from the left; a labelled one is wider.
        float x = b.left + 8.0f;

        for (std::size_t j = 0; j < i; ++j)
        {
            x += (actions[j].label.empty() ? 26.0f : 70.0f) + 5.0f;
        }

        const float w = actions[i].label.empty() ? 26.0f : 70.0f;
        const float y = b.bottom - kActionBar + 6.0f;
        return el::rect{x, y, x + w, y + 26.0f};
    }

    int HitAction(const el::rect& bounds, el::point p) const
    {
        const auto b = bounds.inset(0.5f, 0.5f);

        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            if (ActionRect(b, i).includes(p))
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    const Theme& mTheme;
    Icons& mIcons;
    tones::Tone mTone;
    std::function<std::shared_ptr<artist::image>(const std::string&)> mImage;
    float mWidth, mHeight;
    bool mHover = false;
    int mHoverAction = -1;
    int mKeyAction = -1; // the action Enter runs: the one picked with the arrows
};
} // namespace nanoq::ui::tone_cards
