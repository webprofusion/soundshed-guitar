// The Tones page: Soundshed's tone sharing, as the web UI's Tones tab
// (ui-components/panels/sharing-panel.html, css/tone-sharing.css, css/compact/panels.css): the
// account and the sections (Presets, Packs, Installed, My Content) in the side list, with Create
// Pack, the Cards / List switch and a help card (chips across the top at the compact size); a
// search with a tag filter; then the tones, as cards or as rows - the latest packs in a strip that
// scrolls sideways over the community presets - each with Preview, Install and Share, a pack with
// Open for its presets. Installed lists what this profile has installed, each with a way to remove
// it; My Content, the account's own presets and packs. A tone a link opened the page on sits over
// the rest. Previews, a pack's own view and links are TonePreview.cpp's.
//
// The requests, the account, previewing and installing are the shared ToneService's
// (core/src/uiclient/ToneService.h); both fetch the Tone3000 models a preset names from Soundshed's
// relay first. Publishing, and making packs, are Soundshed Guitar's: Publish and Create Pack say so
// (SectionNav.cpp).

#include "ui/DialogChrome.h"
#include "ui/Shell.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <ctime>

namespace nanoq::ui
{
namespace tones = guitarfx::uiclient::tones;

namespace
{
constexpr float kCardGap = 10.0f;
constexpr float kPresetCardMin = 196.0f; // the grid's minmax(168px, 1fr), at the compact density
constexpr float kPresetCardHeight = 156.0f;
constexpr float kPackCardWidth = 172.0f;
constexpr float kPackCardHeight = 206.0f;
constexpr float kActionBar = 38.0f;

/// The card's accent where it has no picture: a hue from its id, as the web UI's.
el::color AccentFor(const std::string& id)
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
std::vector<std::string> Wrap(artist::canvas& cnv, std::string text, const el::font_descr& font, float width,
                              int maxLines)
{
    // A card shows running text: line breaks in it are spaces.
    std::replace_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;

    while (i < text.size() && static_cast<int>(lines.size()) < maxLines)
    {
        auto end = text.find(' ', i);

        if (end == std::string::npos)
        {
            end = text.size();
        }

        const auto word = text.substr(i, end - i);
        const auto candidate = line.empty() ? word : line + " " + word;

        if (!line.empty() && el::measure_text(cnv, candidate, font).x > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = candidate;
        }

        i = end + 1;
    }

    if (static_cast<int>(lines.size()) < maxLines && !line.empty())
    {
        lines.push_back(line);
    }

    if (!lines.empty() && (i < text.size() || el::measure_text(cnv, lines.back(), font).x > width))
    {
        lines.back() = Ellipsize(cnv, lines.back() + (i < text.size() ? "..." : ""), font, width);
    }

    return lines;
}

/// A small rounded label on a card (.tone-sharing-chip, .tag-badge).
float DrawChip(artist::canvas& cnv, const std::string& text, float x, float y, el::color ink, el::color fill)
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

/// A section's heading (.tone-sharing-row-title): small capitals.
Ptr SectionTitle(const Theme& theme, const std::string& text)
{
    auto t = std::make_shared<Text>(theme, Upper(text), 9.5f, Weight::SemiBold);
    t->Colour(theme.TextSecondary());
    return FixedHeight(26, Pad(2, 10, 0, 2, t));
}
} // namespace

std::string Shell::TonesPageSignature() const
{
    const auto& service = mSession.Tones();
    std::string s = std::to_string(static_cast<int>(mTonesTab)) + "|" + mTonesQuery + "|" + mTonesTag + "|" +
                    std::to_string(mTonesPage) + "|" + (mTonesMore ? "m" : "") + (mTonesLoading ? "l" : "") + "|" +
                    mTonesError + "|" + std::to_string(mToneItems.size()) + "/" + std::to_string(mTonePacks.size()) +
                    "/" + std::to_string(mToneHomePacks.size()) + "/" + std::to_string(mToneMine.size()) + "/" +
                    std::to_string(mToneMinePacks.size()) + (mTonesList ? "L" : "") + (mWide ? "W" : "") + "|" +
                    (service.Account() ? service.Account()->Label() : std::string()) +
                    (service.HasSession() ? "s" : "") + "|" + std::to_string(static_cast<int>(ViewSize().x / 32.0f)) +
                    "|" + (mTonesShared ? mTonesShared->id : std::string()) + "|" + service.PreviewItemId() +
                    (service.IsPreviewLoading() ? "l" : "");

    // A card's Install button follows its install.
    for (const auto& entry : tones::InstalledEntries(mState.appSettings))
    {
        s += "|i" + entry.id;
    }

    for (const auto& [id, install] : mState.toneInstalls)
    {
        s += "|t" + id + std::to_string(static_cast<int>(install.status));
    }

    for (const auto* list : {&mToneItems, &mTonePacks, &mToneHomePacks, &mToneMine, &mToneMinePacks})
    {
        for (const auto& tone : *list)
        {
            if (service.IsDownloading(tones::EntryIdFor(tone)))
            {
                s += "|d" + tone.id;
            }
        }
    }

    return s;
}

std::shared_ptr<artist::image> Shell::RemoteImage(const std::string& url)
{
    if (url.empty())
    {
        return nullptr;
    }

    return mRemoteImages.Find(url, mSession.Tones().Image(url));
}

void Shell::LoadTones(bool more)
{
    auto& service = mSession.Tones();
    const int generation = ++mTonesGeneration;
    mTonesLoading = true;
    mTonesLoaded = true;
    mTonesError.clear();

    if (!more)
    {
        mTonesPage = 1;
    }

    const auto failed = [this, generation](const tones::Response& response) {
        if (generation != mTonesGeneration)
        {
            return false;
        }

        mTonesLoading = false;

        if (!response.ok)
        {
            mTonesError = response.error.empty() ? std::string("Tone sharing could not be reached") : response.error;
            Mark(kPage);
            return true;
        }

        return false;
    };

    switch (mTonesTab)
    {
    case TonesTab::Presets: {
        const bool home = !more && mTonesQuery.empty() && mTonesTag.empty();

        if (!more)
        {
            mToneItems.clear();
            mToneHomePacks.clear();
        }

        if (home)
        {
            // The featured page's pack rows become the row of latest packs over the presets.
            service.Get("/home", [this, generation](const tones::Response& response) {
                if (generation != mTonesGeneration || !response.ok)
                {
                    return;
                }

                mToneHomePacks.clear();

                for (const auto& row : tones::ParseHome(response.data))
                {
                    for (const auto& tone : row.tones)
                    {
                        if (tone.IsPack() && std::none_of(mToneHomePacks.begin(), mToneHomePacks.end(),
                                                          [&tone](const tones::Tone& t) { return t.id == tone.id; }))
                        {
                            mToneHomePacks.push_back(tone);
                        }
                    }
                }

                Mark(kPage);
            });
        }

        const int page = more ? mTonesPage + 1 : 1;
        service.Get(tones::ItemsPath(page, mTonesQuery, mTonesTag),
                    [this, generation, page, failed](const tones::Response& response) {
                        if (failed(response) || generation != mTonesGeneration)
                        {
                            return;
                        }

                        const auto items = tones::ParseItems(response.data);
                        mToneItems.insert(mToneItems.end(), items.begin(), items.end());
                        mTonesPage = page;
                        mTonesMore = static_cast<int>(items.size()) >= tones::kItemsPageSize;
                        Mark(kPage);
                    });
        break;
    }
    case TonesTab::Packs: {
        const int page = std::max(1, mTonesPage);
        service.Get(tones::PacksPath(page), [this, generation, failed](const tones::Response& response) {
            if (failed(response) || generation != mTonesGeneration)
            {
                return;
            }

            mTonePacks = tones::ParsePacks(response.data);
            mTonesMore = static_cast<int>(mTonePacks.size()) >= tones::kPacksPageSize;
            Mark(kPage);
        });
        break;
    }
    case TonesTab::Installed:
        mTonesLoading = false; // read from the settings, not the service
        break;
    case TonesTab::Mine: {
        mToneMine.clear();
        mToneMinePacks.clear();
        mTonesMore = false;

        if (!service.HasSession())
        {
            mTonesLoading = false;
            break;
        }

        // Both lists, as the web UI's loadMine: the page is done when both have answered.
        auto pending = std::make_shared<int>(2);
        const auto answered = [this, generation, pending, failed](const tones::Response& response,
                                                                  std::vector<tones::Tone>& into, bool packs) {
            if (generation != mTonesGeneration)
            {
                return;
            }

            if (response.ok)
            {
                into = packs ? tones::ParsePacks(response.data) : tones::ParseItems(response.data);
            }
            else
            {
                failed(response);
            }

            mTonesLoading = --*pending > 0 && mTonesError.empty();
            Mark(kPage);
        };
        service.Get(tones::kMyItemsPath, [answered, this](const tones::Response& r) { answered(r, mToneMine, false); });
        service.Get(tones::kMyPacksPath,
                    [answered, this](const tones::Response& r) { answered(r, mToneMinePacks, true); });
        break;
    }
    }
}

Ptr Shell::BuildTonesPage()
{
    auto& service = mSession.Tones();

    if (!mTonesLoaded && !mTonesLoading)
    {
        service.RefreshAccount();
        LoadTones(false);
    }

    // The side list takes 245 px in the full layout.
    const float stageWidth = StageWidth() - (mWide ? 245.0f : 0.0f);
    const float contentWidth = stageWidth - 2.0f * (mWide ? 14.0f : 10.0f) - 14.0f; // margins, scroll bar
    auto image = [this](const std::string& url) { return RemoteImage(url); };

    // ── The sections: the account, the tabs, Create Pack, Cards / List and the help card ──
    const auto& account = service.Account();
    SectionNav nav;
    nav.account = account ? account->Label() : std::string(service.HasSession() ? "Soundshed" : "Sign In");
    nav.onAccount = [this](el::rect anchor) { OpenToneAccount(anchor); };

    for (const auto& [label, which] :
         {std::pair{"Presets", TonesTab::Presets}, std::pair{"Packs", TonesTab::Packs},
          std::pair{"Installed", TonesTab::Installed}, std::pair{"My Content", TonesTab::Mine}})
    {
        nav.items.push_back({label, mTonesTab == which, [this, which] {
                                 if (mTonesTab == which)
                                 {
                                     return;
                                 }

                                 mTonesTab = which;
                                 mTonesPage = 1;
                                 LoadTones(false);
                                 Mark(kPage);
                             }});
    }

    const auto createPack = [this](float width) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        b->Size(width, 26).Label("+ Create Pack", 12.0f);
        b->Ink(mTheme.TextSecondary());
        b->onClick = [this] { OpenPublishInfo(true); };
        return b;
    };
    const auto layout = [this](float segment) {
        auto s = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Cards", "List"}, segment);
        s->AccessName("View");
        s->Select(mTonesList ? 1 : 0);
        s->onSelect = [this](int index) {
            mTonesList = index == 1;
            mEffectSignature.clear();
            Mark(kPage);
        };
        return s;
    };
    nav.tools = {FixedHeight(26, createPack(208.0f)), layout(104.0f)};
    nav.compactTools = {FixedWidth(110, Middle(createPack(110.0f))), FixedWidth(112, Middle(layout(56.0f)))};
    nav.help = "Share a preset in 3 steps: load it in Soundshed Guitar, click Publish there, then use Share on "
               "its card here after approval.";

    std::vector<Ptr> body;

    // ── The search: words, a tag, and Clear (Presets only, as the web UI) ──
    if (mTonesTab == TonesTab::Presets)
    {
        if (!mTonesSearchBox)
        {
            auto [element, box] = el::input_box("Name, description, creator, or tag", Font(Weight::Regular), 1.0f);
            mTonesSearchBox = box;
            mTonesSearchElement = el::share(std::move(element));
            mTonesSearchBox->on_enter = [this](std::string_view text) {
                mTonesQuery = std::string(text);
                LoadTones(false);
                Mark(kPage);
                return true;
            };
        }

        auto tag = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        tag->Size(120, 30).Label(mTonesTag.empty() ? std::string("All tags") : mTonesTag, 11.0f).Caret(true);
        tag->onClick = [this, t = tag.get()] {
            std::vector<MenuItem> items{{"All tags", mTonesTag.empty(), false, [this] {
                                             mTonesTag.clear();
                                             LoadTones(false);
                                             Mark(kPage);
                                         }}};

            for (const auto& name : tones::StandardTags())
            {
                items.push_back({name, mTonesTag == name, false, [this, name] {
                                     mTonesTag = name;
                                     LoadTones(false);
                                     Mark(kPage);
                                 }});
            }

            OpenDropdown(t->Bounds(), std::move(items), 160.0f);
        };
        auto clear = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        clear->Size(56, 30).Label("Clear", 11.0f).Disabled(mTonesQuery.empty() && mTonesTag.empty());
        clear->onClick = [this] {
            mTonesQuery.clear();
            mTonesTag.clear();

            if (mTonesSearchBox)
            {
                mTonesSearchBox->set_text("");
            }

            LoadTones(false);
            Mark(kPage);
        };
        body.push_back(FixedHeight(30, Row({mTonesSearchElement, HGap(8), tag, HGap(6), clear})));
        body.push_back(VGap(4));
    }

    // A card's actions: Open (a pack) or Preview (Stop while it plays), Install (Installed once it
    // is), Share.
    auto actionsFor = [this, &service](const tones::Tone& tone) {
        std::vector<ToneCard::Action> actions;

        if (tone.IsPack())
        {
            actions.push_back({"ui-eye", false, true, {}, [this, tone] { OpenPackDetail(tone); }});
        }
        else if (service.PreviewItemId() == tone.id && service.IsPreviewLoading())
        {
            actions.push_back({"", false, false, "Loading...", {}});
        }
        else if (service.PreviewItemId() == tone.id)
        {
            actions.push_back({"ui-stop", true, true, {}, [this] { mSession.Tones().StopPreview(); }});
        }
        else
        {
            actions.push_back({"ui-play", false, true, {}, [this, tone] { mSession.Tones().Preview(tone); }});
        }

        const auto entryId = tones::EntryIdFor(tone);
        const bool installed = tones::FindInstalled(mState.appSettings, entryId).has_value();
        const bool busy = service.IsDownloading(entryId) || (mState.toneInstalls.count(entryId) > 0 &&
                                                             mState.toneInstalls.at(entryId).status ==
                                                                 guitarfx::uiclient::ToneInstall::Status::Installing);

        if (busy)
        {
            actions.push_back({"", false, false, "Installing...", {}});
        }
        else if (installed)
        {
            actions.push_back({"", false, false, "Installed", {}});
        }
        else
        {
            actions.push_back({"ui-download", true, true, {}, [this, tone] { mSession.Tones().Install(tone); }});
        }

        actions.push_back({"ui-share", false, true, {}, [this, tone] { ShareTone(tone); }});
        return actions;
    };

    auto grid = [&](const std::vector<tones::Tone>& list, float minWidth, float height) {
        // The List layout: a row each, the width of the page.
        if (mTonesList)
        {
            std::vector<Ptr> rows;

            for (const auto& tone : list)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, tone, image, contentWidth, 32.0f);
                card->row = true;
                card->actions = actionsFor(tone);

                if (!rows.empty())
                {
                    rows.push_back(VGap(5));
                }

                rows.push_back(card);
            }

            return Col(rows);
        }

        const int columns = std::max(1, static_cast<int>((contentWidth + kCardGap) / (minWidth + kCardGap)));
        const float width = (contentWidth - static_cast<float>(columns - 1) * kCardGap) / static_cast<float>(columns);
        std::vector<Ptr> rows;

        for (std::size_t i = 0; i < list.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> row;

            for (std::size_t j = i; j < std::min(list.size(), i + static_cast<std::size_t>(columns)); ++j)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, list[j], image, width, height);
                card->actions = actionsFor(list[j]);

                if (!row.empty())
                {
                    row.push_back(HGap(kCardGap));
                }

                row.push_back(card);
            }

            row.push_back(Stretch());

            if (!rows.empty())
            {
                rows.push_back(VGap(kCardGap));
            }

            rows.push_back(FixedHeight(height, Row(row)));
        }

        return Col(rows);
    };

    auto message = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, text, 12.0f);
        t->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        return FixedHeight(60, t);
    };

    if (!mTonesError.empty())
    {
        body.push_back(message(mTonesError));
    }

    switch (mTonesTab)
    {
    case TonesTab::Presets: {
        // The tone a link opened the page on, over the rest until it is put away.
        if (mTonesShared)
        {
            auto close = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            close->Size(80, 24).Label("Show all", 10.5f);
            close->onClick = [this] {
                mTonesShared.reset();
                Mark(kPage);
            };
            body.push_back(
                FixedHeight(30, Row({SectionTitle(mTheme, "Shared with you"), Stretch(), Pad(0, 4, 0, 2, close)})));
            body.push_back(grid({*mTonesShared}, kPresetCardMin, kPresetCardHeight));
        }

        if (!mToneHomePacks.empty())
        {
            body.push_back(SectionTitle(mTheme, "Latest packs"));
            std::vector<Ptr> strip;

            // As rows, the strip is a line of wide bars.
            const float packWidth = mTonesList ? 320.0f : kPackCardWidth;
            const float packHeight = mTonesList ? 32.0f : kPackCardHeight;

            for (const auto& pack : mToneHomePacks)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, pack, image, packWidth, packHeight);
                card->row = mTonesList;
                card->actions = actionsFor(pack);

                if (!strip.empty())
                {
                    strip.push_back(HGap(kCardGap));
                }

                strip.push_back(card);
            }

            const float stripWidth = static_cast<float>(mToneHomePacks.size()) * (packWidth + kCardGap);
            body.push_back(FixedHeight(packHeight + 14.0f, HScroll(FixedWidth(stripWidth, Row(strip)))));
        }

        body.push_back(
            SectionTitle(mTheme, mTonesQuery.empty() && mTonesTag.empty() ? "Community presets" : "Search results"));

        if (mToneItems.empty())
        {
            body.push_back(message(mTonesLoading ? "Loading..." : "No presets match."));
        }
        else
        {
            body.push_back(grid(mToneItems, kPresetCardMin, kPresetCardHeight));
        }

        if (mTonesMore || mTonesLoading)
        {
            auto loadMore = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            loadMore->Size(110, 30).Label(mTonesLoading ? "Loading..." : "Load More", 11.0f).Disabled(mTonesLoading);
            loadMore->onClick = [this] {
                LoadTones(true);
                Mark(kPage);
            };
            body.push_back(VGap(10));
            body.push_back(FixedHeight(30, Row({Stretch(), loadMore, Stretch()})));
        }

        break;
    }
    case TonesTab::Packs: {
        body.push_back(SectionTitle(mTheme, "Latest packs"));

        if (mTonePacks.empty())
        {
            body.push_back(message(mTonesLoading ? "Loading..." : "No packs yet."));
        }
        else
        {
            body.push_back(grid(mTonePacks, kPackCardWidth, kPackCardHeight));
        }

        auto step = [this](const char* label, int delta, bool enabled) {
            auto b = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            b->Size(80, 28).Label(label, 11.0f).Disabled(!enabled);
            b->onClick = [this, delta] {
                mTonesPage = std::max(1, mTonesPage + delta);
                LoadTones(true);
                Mark(kPage);
            };
            return b;
        };
        auto pageText = std::make_shared<Text>(
            mTheme, "Page " + std::to_string(mTonesPage) + (mTonesMore ? "" : " \xC2\xB7 end of results"), 11.0f);
        pageText->Colour(mTheme.TextMuted()).Fixed(true);
        body.push_back(VGap(12));
        body.push_back(FixedHeight(28, Row({Stretch(), step("Previous", -1, mTonesPage > 1), HGap(8),
                                            step("Next", 1, mTonesMore), HGap(12), pageText, Stretch()})));
        break;
    }
    case TonesTab::Installed: {
        const auto entries = tones::InstalledEntries(mState.appSettings);

        if (entries.empty())
        {
            body.push_back(message("Nothing installed from tone sharing yet."));
            break;
        }

        std::vector<tones::Tone> list;
        std::vector<std::vector<std::string>> details;

        for (const auto& entry : entries)
        {
            tones::Tone tone;
            tone.id = entry.id;
            tone.title = entry.title;
            list.push_back(tone);
            std::string when = entry.importedAt.substr(0, 10);
            const auto source = entry.source == "toneSharingApi" ? std::string("Tone Sharing") : entry.source;
            const auto presets = entry.presetIds.size();
            details.push_back({source + (when.empty() ? "" : " \xC2\xB7 " + when),
                               std::to_string(presets) + (presets == 1 ? " preset" : " presets") + " \xC2\xB7 " +
                                   std::to_string(entry.resourceCount) +
                                   (entry.resourceCount == 1 ? " resource" : " resources")});
        }

        const int columns = std::max(1, static_cast<int>((contentWidth + kCardGap) / (kPackCardWidth + kCardGap)));
        const float width = (contentWidth - static_cast<float>(columns - 1) * kCardGap) / static_cast<float>(columns);
        std::vector<Ptr> rows;

        for (std::size_t i = 0; i < list.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> row;

            for (std::size_t j = i; j < std::min(list.size(), i + static_cast<std::size_t>(columns)); ++j)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, list[j], nullptr, width, 128.0f);
                card->lines = details[j];
                const auto id = list[j].id, title = list[j].title;
                card->actions.push_back({"ui-trash", false, true, {}, [this, id, title] {
                                             OpenConfirm(
                                                 "Remove \"" + title + "\"",
                                                 "Its presets are deleted, and the models and IRs nothing else uses.",
                                                 "Remove", [this, id] { mSession.Tones().Remove(id); });
                                         }});

                if (!row.empty())
                {
                    row.push_back(HGap(kCardGap));
                }

                row.push_back(card);
            }

            row.push_back(Stretch());

            if (!rows.empty())
            {
                rows.push_back(VGap(kCardGap));
            }

            rows.push_back(FixedHeight(128.0f, Row(row)));
        }

        body.push_back(VGap(8));
        body.push_back(Col(rows));
        break;
    }
    case TonesTab::Mine: {
        if (!service.HasSession())
        {
            body.push_back(message("Sign in to view your content."));
            break;
        }

        // What the account has published, as the web UI's two rows. Editing and deleting them
        // are Soundshed Guitar's.
        const auto section = [&](const char* title, const std::vector<tones::Tone>& list, bool packs) {
            body.push_back(SectionTitle(mTheme, title));

            if (list.empty())
            {
                body.push_back(message(mTonesLoading ? "Loading..." : "Nothing published yet."));
            }
            else
            {
                body.push_back(packs ? grid(list, kPackCardWidth, kPackCardHeight)
                                     : grid(list, kPresetCardMin, kPresetCardHeight));
            }
        };
        section("My presets on Tone Sharing", mToneMine, false);
        section("My packs on Tone Sharing", mToneMinePacks, true);
        auto publish = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Accent);
        publish->Size(120, 30).IconSize(13).Label("How to publish", 11.0f);
        publish->onClick = [this] { OpenPublishInfo(); };
        body.push_back(VGap(12));
        body.push_back(FixedHeight(30, Row({Stretch(), publish, Stretch()})));
        break;
    }
    }

    body.push_back(VGap(16));
    auto content = VScroll(Pad(mWide ? 14.0f : 10.0f, mWide ? 18.0f : 4.0f, 14, 0, Col(body)));
    return Over(WithSectionNav(std::move(nav), content), std::make_shared<StageBackdrop>(mTheme));
}

void Shell::OpenToneAccount(el::rect anchor)
{
    auto& service = mSession.Tones();

    if (service.Account() || service.HasSession())
    {
        const auto& account = service.Account();
        std::vector<MenuItem> items;

        if (account)
        {
            items.push_back({account->Label(), false, true, {}});
        }

        items.push_back({"Sign out", false, false, [this] {
                             OpenConfirm("Sign out of tone sharing",
                                         "This signs Soundshed Guitar out too: they share the account.", "Sign out",
                                         [this] { mSession.Tones().SignOut(); });
                         }});
        OpenDropdown(anchor, std::move(items), 200.0f);
        return;
    }

    // Signing in: an email, then the code the service emails to it.
    OpenPrompt("Sign in to tone sharing", "", "Email me a code", [this](const std::string& email) {
        mSession.Tones().RequestCode(email, [this, email](const guitarfx::uiclient::tones::Response& response) {
            if (!response.ok)
            {
                ShowToast("Could not send a code", response.error, true);
                return;
            }

            OpenPrompt("Enter the code sent to " + email, "", "Sign in", [this, email](const std::string& code) {
                mSession.Tones().VerifyCode(email, code, [this](const guitarfx::uiclient::tones::Response& answer) {
                    if (!answer.ok)
                    {
                        ShowToast("Could not sign in", answer.error, true);
                    }

                    Mark(kPage);
                });
            });
        });
    });
}

void Shell::ShareTone(const tones::Tone& tone)
{
    el::set_clipboard({{"text/plain", tones::ShareLink(tone)}});
    ShowToast("Link copied", tone.title, false);
}
} // namespace nanoq::ui
