// Tones > Your Presets, as the web UI's (tonesPanel/localPresets.ts, tones-local-presets.html,
// css/tones.css): the presets on this machine as a grid of cards, with Recents, Favourites, All
// Presets and the folders down the side (each with its count), and a search, a tag and a sort over
// the grid. It is a second view of the preset library, not a second library: the chooser's filter
// rules (uiclient/PresetBrowse.h), favourites and ratings; what it keeps for itself is where it is
// looking (its folder, search, tag and sort), so browsing here does not move the chooser.
//
// A card loads its preset, anywhere on it but the heart and the stars; its Load button is the same
// for the keyboard. The page stays where it is: loading is heard, not a trip to the chain.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "ui/library/ToneCard.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/TonesBrowse.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
constexpr float kFooterHeight = 44.0f;

/// One preset as a card (.tones-local-card): a band in its accent with its category and initials,
/// its name and heart, description, folders and tags, then its stars and Load. The one playing is
/// edged in the accent and its button says Loaded; the one loading is dimmed.
class LocalPresetCard final : public Pressable
{
  public:
    struct Content
    {
        std::string id, name, category, description, folder;
        std::vector<std::string> tags;
        int rating = 0;
        bool favourite = false, active = false, loading = false;
    };

    LocalPresetCard(const Theme& theme, Content content, bool compact, float width, float height)
        : mTheme(theme), mC(std::move(content)), mCompact(compact), mWidth(width), mHeight(height)
    {
    }

    std::function<void(int rating)> onRate;
    std::function<void()> onFavourite;

    /// The band over the body (.tones-local-card-art): 60 px, 40 at the compact size.
    static float ArtHeight(bool compact)
    {
        return compact ? 40.0f : 60.0f;
    }

    /// How tall a card must be for what it has in `width` (the grid's rows are as tall as their
    /// tallest card, as the web's max-content rows).
    static float HeightFor(const Content& c, bool compact, float width)
    {
        const float text = width - 24.0f;
        const auto lines = [text](const std::string& s, const el::font_descr& font, float reserve) {
            const float w = MeasureTextWidth(s, font);
            return std::clamp(static_cast<int>(std::ceil(w / std::max(1.0f, text - reserve))), 1, 2);
        };
        float h = ArtHeight(compact) + 20.0f + static_cast<float>(lines(c.name, TitleFont(), 24.0f)) * 18.0f;

        if (!c.description.empty())
        {
            h += 6.0f + static_cast<float>(lines(c.description, DescriptionFont(), 0.0f)) * 16.0f;
        }

        if (!c.folder.empty())
        {
            h += 6.0f + 14.0f;
        }

        if (!c.tags.empty())
        {
            h += 6.0f + 16.0f;
        }

        return h + kFooterHeight;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.name = NameOr(mC.name.empty() ? std::string("Untitled preset") : mC.name);
        info.value = std::string(mC.loading ? "Loading" : (mC.active ? "Loaded" : "")) +
                     (mC.favourite ? std::string(mC.active || mC.loading ? ", " : "") + "Favourite" : std::string()) +
                     (mC.rating > 0 ? (mC.active || mC.loading || mC.favourite ? ", " : "") +
                                          std::to_string(mC.rating) + " stars"
                                    : std::string());
        info.selected = mC.active;
        return info;
    }

    // 0 to 5 rate it, F favourites it, as the heart and the stars do.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (k.action == el::key_action::press && (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0)
        {
            if (onRate && k.key >= el::key_code::_0 && k.key <= el::key_code::_5)
            {
                onRate(static_cast<int>(k.key) - static_cast<int>(el::key_code::_0));
                return true;
            }

            if (onFavourite && k.key == el::key_code::f)
            {
                onFavourite();
                return true;
            }
        }

        return Pressable::key(ctx, k);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mWidth, mHeight}, {mWidth, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        constexpr float kRadius = 12.0f;
        const auto accent = tone_cards::AccentFor(mC.id);
        const float artBottom = b.top + ArtHeight(mCompact);
        const el::rect footer{b.left, b.bottom - kFooterHeight, b.right, b.bottom};

        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, kRadius);
            cnv.clip();
            cnv.fill_style(mTheme.Card());
            cnv.fill_rect(b);

            // The band: the card's accent washed over the face.
            const el::rect art{b.left, b.top, b.right, artBottom};
            artist::canvas::linear_gradient wash{art.top_left(), art.bottom_left()};
            wash.add_color_stop(0.0f, accent.opacity(0.30f));
            wash.add_color_stop(1.0f, accent.opacity(0.06f));
            cnv.fill_style(wash);
            cnv.fill_rect(art);

            cnv.fill_style(mTheme.Text().opacity(0.03f));
            cnv.fill_rect(footer);
        }

        cnv.fill_style(mTheme.Border().opacity(0.6f));
        cnv.fill_rect(el::rect{b.left, artBottom, b.right, artBottom + 1.0f});
        cnv.fill_rect(el::rect{b.left, footer.top, b.right, footer.top + 1.0f});

        // The category at the band's foot, the initials at its right.
        const auto initialsFont = Font(Weight::SemiBold).size(mCompact ? 18.0f : 26.0f);
        const auto initials = PresetInitials(mC.name.empty() ? std::string("Untitled preset") : mC.name);
        const float initialsWidth = MeasureTextWidth(initials, initialsFont);
        const float bandBase = artBottom - (mCompact ? 6.0f : 10.0f);
        FillText(cnv, initials, initialsFont, mTheme.Ink(),
                 el::rect{b.right - 12.0f - initialsWidth, bandBase - (mCompact ? 20.0f : 28.0f), b.right - 12.0f,
                          bandBase},
                 HAlign::Right);

        if (!mC.category.empty())
        {
            const auto font = Font(Weight::SemiBold).size(10.0f);
            const float room = b.width() - 24.0f - initialsWidth - 10.0f;
            FillTrackedText(cnv, Ellipsize(cnv, Upper(mC.category), font, std::max(0.0f, room)), font,
                            mTheme.Ink().opacity(0.85f), el::rect{b.left + 12.0f, bandBase - 14.0f, b.left + 12.0f + room, bandBase},
                            HAlign::Left, 0.08f);
        }

        // The name (two lines at most) and the heart at its right.
        const float x = b.left + 12.0f, w = b.width() - 24.0f;
        float y = artBottom + 10.0f;
        const auto heart = HeartRect(b);
        const auto titleFont = TitleFont();

        for (const auto& line : tone_cards::Wrap(cnv, mC.name.empty() ? std::string("Untitled preset") : mC.name,
                                                 titleFont, w - 24.0f, 2))
        {
            FillText(cnv, line, titleFont, mTheme.Ink(), el::rect{x, y, x + w - 24.0f, y + 18.0f}, HAlign::Left);
            y += 18.0f;
        }

        DrawHeart(cnv, heart);

        if (!mC.description.empty())
        {
            y += 6.0f;

            for (const auto& line : tone_cards::Wrap(cnv, mC.description, DescriptionFont(), w, 2))
            {
                FillText(cnv, line, DescriptionFont(), mTheme.TextSecondary(), el::rect{x, y, x + w, y + 16.0f},
                         HAlign::Left);
                y += 16.0f;
            }
        }

        if (!mC.folder.empty())
        {
            y += 6.0f;
            const auto font = Font(Weight::Regular).size(11.0f);
            FillText(cnv, Ellipsize(cnv, mC.folder, font, w), font, mTheme.TextMuted(), el::rect{x, y, x + w, y + 14.0f},
                     HAlign::Left);
            y += 14.0f;
        }

        if (!mC.tags.empty())
        {
            y += 6.0f;
            const auto font = Font(Weight::Regular).size(9.5f);
            float tx = x;

            for (const auto& tag : mC.tags)
            {
                const float tw = MeasureTextWidth(tag, font) + 12.0f;

                if (tx + tw > x + w)
                {
                    break;
                }

                const el::rect chip{tx, y, tx + tw, y + 16.0f};
                cnv.fill_style(mTheme.Accent().opacity(0.12f));
                cnv.fill_round_rect(chip, 8.0f);
                FillText(cnv, tag, font, mTheme.TextSecondary(), chip, HAlign::Centre);
                tx += tw + 4.0f;
            }
        }

        // The stars and Load.
        for (int i = 0; i < 5; ++i)
        {
            FillStar(cnv, StarRect(b, i),
                     i < mC.rating ? mTheme.Accent() : mTheme.TextMuted().opacity(mHover ? 0.65f : 0.4f));
        }

        const auto load = LoadRect(b);
        const std::string label = mC.loading ? "Loading\xE2\x80\xA6" : (mC.active ? "Loaded" : "Load");

        if (mC.active)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(load.inset(0.5f, 0.5f), 6.0f);
        }
        else
        {
            cnv.fill_style(mTheme.AccentFill().opacity(mC.loading ? 0.6f : 1.0f));
            cnv.fill_round_rect(load, 6.0f);
        }

        FillText(cnv, label, Font(Weight::SemiBold).size(11.0f), mC.active ? mTheme.Ink() : mTheme.OnAccent(), load,
                 HAlign::Centre);

        // The edge: the accent for the one playing, the card's accent mixed in under the pointer.
        if (mC.active)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(2.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), kRadius);
        }
        else
        {
            cnv.stroke_style(mHover ? accent.opacity(0.7f) : mTheme.Border());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b, kRadius);
        }

        if (mC.loading)
        {
            cnv.fill_style(mTheme.Card().opacity(0.3f));
            cnv.fill_round_rect(b, kRadius);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left)
        {
            const auto b = ctx.bounds.inset(0.5f, 0.5f);

            if (HeartRect(b).inset(-4.0f, -4.0f).includes(btn.pos))
            {
                if (!btn.down && onFavourite)
                {
                    onFavourite();
                }

                return true;
            }

            for (int i = 0; i < 5; ++i)
            {
                if (StarRect(b, i).inset(-2.0f, -8.0f).includes(btn.pos))
                {
                    // Clicking the rating it has clears it, as in the chooser.
                    if (!btn.down && onRate)
                    {
                        onRate(i + 1 == mC.rating ? 0 : i + 1);
                    }

                    return true;
                }
            }
        }

        return Pressable::click(ctx, btn);
    }

  private:
    static el::font_descr TitleFont()
    {
        return Font(Weight::SemiBold).size(14.0f);
    }

    static el::font_descr DescriptionFont()
    {
        return Font(Weight::Regular).size(12.0f);
    }

    [[nodiscard]] el::rect HeartRect(const el::rect& b) const
    {
        const float top = b.top + ArtHeight(mCompact) + 10.0f;
        return el::rect{b.right - 12.0f - 18.0f, top, b.right - 12.0f, top + 18.0f};
    }

    /// The heart (.tones-local-card-fav): filled in the favourite colour when it is one.
    void DrawHeart(el::canvas& cnv, const el::rect& r) const
    {
        const float cx = (r.left + r.right) * 0.5f, s = r.width() * 0.5f;
        const float top = r.top + s * 0.55f;
        cnv.begin_path();
        cnv.move_to(el::point{cx, r.bottom - s * 0.2f});
        cnv.bezier_curve_to(el::point{cx - s * 1.1f, top + s * 0.55f}, el::point{cx - s * 0.75f, r.top - s * 0.05f},
                            el::point{cx, top});
        cnv.bezier_curve_to(el::point{cx + s * 0.75f, r.top - s * 0.05f}, el::point{cx + s * 1.1f, top + s * 0.55f},
                            el::point{cx, r.bottom - s * 0.2f});
        cnv.close_path();

        if (mC.favourite)
        {
            cnv.fill_style(mTheme.PresetFavorite());
            cnv.fill();
        }
        else
        {
            cnv.stroke_style(mHover ? mTheme.PresetFavorite() : mTheme.TextMuted());
            cnv.line_width(1.3f);
            cnv.stroke();
        }
    }

    [[nodiscard]] el::rect StarRect(const el::rect& b, int index) const
    {
        const float left = b.left + 12.0f + static_cast<float>(index) * 16.0f;
        const float mid = b.bottom - kFooterHeight * 0.5f;
        return el::rect{left, mid - 6.5f, left + 13.0f, mid + 6.5f};
    }

    [[nodiscard]] el::rect LoadRect(const el::rect& b) const
    {
        const float mid = b.bottom - kFooterHeight * 0.5f;
        return el::rect{b.right - 12.0f - 76.0f, mid - 13.0f, b.right - 12.0f, mid + 13.0f};
    }

    const Theme& mTheme;
    Content mC;
    bool mCompact;
    float mWidth, mHeight;
};

std::string FolderName(const std::string& id)
{
    if (id == kPresetFolderRecents)
    {
        return "Recents";
    }

    if (id == kPresetFolderFavorites)
    {
        return "Favourites";
    }

    return "All Presets";
}
} // namespace

std::string Shell::TonesLocalSignature() const
{
    // The typed search is left out: it refills the grid alone (RefillTonesLocalGrid).
    std::string s = mLocalFolder + "|" + mLocalTag + "|" + (mLocalByRating ? "r" : "n") + "|" + mState.activePresetId +
                    "|" + mLoadingPresetId + "|" + (mWide ? "W" : "") +
                    std::to_string(static_cast<int>(ViewSize().x / 32.0f)) + "|" + std::to_string(mState.presetList.size());

    for (const auto& p : mState.presetList)
    {
        s += "|" + p.id + ":" + p.name + ":" + std::to_string(p.tags.size());
    }

    for (const auto& id : mState.presetFavorites)
    {
        s += "|f" + id;
    }

    s += "|" + mState.presetRatings.dump();

    for (const auto& id : mState.presetRecents)
    {
        s += "|r" + id;
    }

    for (const auto& [folder, depth] : FlattenPresetFolders(mState.presetFolders))
    {
        s += "|d" + folder->id + ":" + folder->name + ":" + std::to_string(folder->presetIds.size());
    }

    return s;
}

void Shell::RefillTonesLocalGrid()
{
    if (!mLocalGridSlot || mLocalGridWidth <= 0.0f)
    {
        return;
    }

    const bool compact = !mWide;
    const float gap = compact ? 8.0f : 12.0f;
    const float minWidth = compact ? 190.0f : 230.0f;
    const float width = mLocalGridWidth;

    PresetQuery query;
    query.folderId = mLocalFolder;
    const auto inFolder = FilterPresets(mState, query);
    query.text = mLocalQuery;

    if (!mLocalTag.empty())
    {
        query.tags = {mLocalTag};
    }

    const auto presets = SortLocalPresets(mState, FilterPresets(mState, query),
                                          mLocalByRating ? LocalPresetSort::Rating : LocalPresetSort::Name,
                                          mLocalFolder == kPresetFolderRecents);

    if (mLocalSummarySlot)
    {
        auto summary = std::make_shared<Text>(mTheme, DescribePresetCount(presets.size(), inFolder.size()), 11.0f);
        summary->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);
        Fill(*mLocalSummarySlot, summary);
    }

    // When the card was clicked: it is the one loading until it is the one playing.
    if (!mLoadingPresetId.empty() && mLoadingPresetId == mState.activePresetId)
    {
        mLoadingPresetId.clear();
    }

    if (presets.empty())
    {
        std::string message;
        std::vector<Ptr> extra;

        if (!inFolder.empty())
        {
            message = "No presets match this search.";
        }
        else if (mLocalFolder == kPresetFolderFavorites)
        {
            message = "No favourites yet. Use the heart on a preset to keep it here.";
        }
        else if (mLocalFolder == kPresetFolderRecents)
        {
            message = "Presets you load will show up here.";
        }
        else if (mLocalFolder != kPresetFolderAll)
        {
            message = "This folder is empty.";
        }
        else
        {
            message = "No presets on this machine yet.";

            // The way to some, where tone sharing is on (.tones-local-empty's button).
            if (TonesSectionOn(TonesSection::Community))
            {
                auto browse = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
                browse->Size(210, 30).Label("Browse Community Presets", 11.0f);
                browse->onClick = [this] {
                    mTonesSection = TonesSection::Community;
                    Mark(kPage);
                };
                extra.push_back(VGap(10));
                extra.push_back(FixedHeight(30, Row({Stretch(), browse, Stretch()})));
            }
        }

        auto text = std::make_shared<Text>(mTheme, message, 12.0f);
        text->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        std::vector<Ptr> column{VGap(28), FixedHeight(20, text)};
        column.insert(column.end(), extra.begin(), extra.end());
        Fill(*mLocalGridSlot, Col(column));
        return;
    }

    const int columns = std::max(1, static_cast<int>((width + gap) / (minWidth + gap)));
    const float cardWidth = (width - static_cast<float>(columns - 1) * gap) / static_cast<float>(columns);
    std::vector<Ptr> rows;

    for (std::size_t i = 0; i < presets.size(); i += static_cast<std::size_t>(columns))
    {
        std::vector<LocalPresetCard::Content> contents;

        for (std::size_t j = i; j < std::min(presets.size(), i + static_cast<std::size_t>(columns)); ++j)
        {
            const auto& p = presets[j];
            LocalPresetCard::Content c;
            c.id = p.id;
            c.name = p.name;
            c.category = p.category;
            c.description = p.description;
            c.folder = PresetFolderPath(mState.presetFolders, p.id);
            c.tags = p.tags;
            c.rating = PresetRating(mState, p.id);
            c.favourite = IsPresetFavorite(mState, p.id);
            c.active = p.id == mState.activePresetId;
            c.loading = p.id == mLoadingPresetId;
            contents.push_back(std::move(c));
        }

        float height = 0.0f;

        for (const auto& c : contents)
        {
            height = std::max(height, LocalPresetCard::HeightFor(c, compact, cardWidth));
        }

        std::vector<Ptr> row;

        for (auto& c : contents)
        {
            const auto id = c.id, name = c.name;
            const bool favourite = c.favourite;
            auto card = std::make_shared<LocalPresetCard>(mTheme, std::move(c), compact, cardWidth, height);
            card->onClick = [this, id, name] {
                if (id != mState.activePresetId)
                {
                    mLoadingPresetId = id;
                }

                // As the chooser's, it asks first when the chain playing has unsaved changes; the
                // page stays put.
                ConfirmDiscardThen("Load " + (name.empty() ? std::string("this preset") : name),
                                   [this, id] { mSession.Commands().LoadPreset(id); });
                Mark(kPage);
            };
            card->onRate = [this, id](int rating) { mSession.Commands().SetPresetRating(id, rating); };
            card->onFavourite = [this, id, favourite] { mSession.Commands().SetPresetFavorite(id, !favourite); };

            if (!row.empty())
            {
                row.push_back(HGap(gap));
            }

            row.push_back(card);
        }

        row.push_back(Stretch());

        if (!rows.empty())
        {
            rows.push_back(VGap(gap));
        }

        rows.push_back(FixedHeight(height, Row(row)));
    }

    Fill(*mLocalGridSlot, Col(rows));
}

Ptr Shell::BuildTonesLocal()
{
    // A folder deleted while this looked at it falls back to All Presets.
    if (mLocalFolder != kPresetFolderAll && mLocalFolder != kPresetFolderFavorites &&
        mLocalFolder != kPresetFolderRecents && FindPresetFolder(mState.presetFolders, mLocalFolder) == nullptr)
    {
        mLocalFolder = kPresetFolderAll;
    }

    // The side list: Recents, Favourites, All Presets, then the folders, nested ones stepped in,
    // each with how many presets it holds.
    SectionNav nav;
    nav.top = 12.0f;
    nav.scrollItems = true;
    const auto count = [this](const std::string& folderId) {
        PresetQuery query;
        query.folderId = folderId;
        return std::to_string(FilterPresets(mState, query).size());
    };
    const auto choose = [this](const std::string& folderId) {
        return [this, folderId] {
            if (mLocalFolder == folderId)
            {
                return;
            }

            mLocalFolder = folderId;
            Mark(kPage);
        };
    };

    for (const auto* id : {kPresetFolderRecents, kPresetFolderFavorites, kPresetFolderAll})
    {
        nav.items.push_back({FolderName(id), mLocalFolder == id, choose(id), count(id), 0});
    }

    for (const auto& [folder, depth] : FlattenPresetFolders(mState.presetFolders))
    {
        nav.items.push_back({folder->name, mLocalFolder == folder->id, choose(folder->id), count(folder->id), depth});
    }

    // The toolbar (.tones-local-toolbar): the search, a tag (only when a preset here has one) and
    // the sort, with the count at the right.
    PresetQuery folderQuery;
    folderQuery.folderId = mLocalFolder;
    const auto tags = CollectPresetTags(FilterPresets(mState, folderQuery));

    // A tag no preset in this folder carries any more stops filtering.
    if (!mLocalTag.empty() && std::find(tags.begin(), tags.end(), mLocalTag) == tags.end())
    {
        mLocalTag.clear();
    }

    if (!mLocalSearchBox)
    {
        auto [element, box] = el::input_box("Search presets\xE2\x80\xA6", Font(Weight::Regular), 1.0f);
        mLocalSearchBox = box;
        mLocalSearchElement = el::share(std::move(element));
        mLocalSearchBox->on_text = [this](std::string_view text) {
            mLocalQuery = std::string(text);
            RefillTonesLocalGrid();
            mSession.View().layout();
            mSession.View().refresh();
        };
        mLocalSearchBox->on_enter = [](std::string_view) { return true; };
    }

    // The search keeps its focus while the page is rebuilt round it.
    if (mLocalSearchBox->is_focus())
    {
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), box = std::weak_ptr(mLocalSearchBox)] {
            const auto field = box.lock();

            if (alive.lock() && field && field == mLocalSearchBox)
            {
                FocusInView(mSession.View(), *field);
            }
        });
    }

    std::vector<Ptr> toolbar{WidthRange(140.0f, el::full_extent, mLocalSearchElement)};

    if (!tags.empty())
    {
        auto tag = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        tag->Size(130, 30).Label(mLocalTag.empty() ? std::string("All tags") : mLocalTag, 11.0f).Caret(true);
        tag->AccessName("Filter presets by tag");
        tag->onClick = [this, tags, t = tag.get()] {
            std::vector<MenuItem> items{{"All tags", mLocalTag.empty(), false, [this] {
                                             mLocalTag.clear();
                                             Mark(kPage);
                                         }}};

            for (const auto& name : tags)
            {
                items.push_back({name, mLocalTag == name, false, [this, name] {
                                     mLocalTag = name;
                                     Mark(kPage);
                                 }});
            }

            OpenDropdown(t->Bounds(), std::move(items), 170.0f);
        };
        toolbar.push_back(HGap(8));
        toolbar.push_back(tag);
    }

    auto sort = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    sort->Size(120, 30).Label(mLocalByRating ? "Top rated" : "Name A-Z", 11.0f).Caret(true);
    sort->AccessName("Sort presets");
    sort->onClick = [this, s = sort.get()] {
        OpenDropdown(s->Bounds(),
                     {{"Name A-Z", !mLocalByRating, false,
                       [this] {
                           mLocalByRating = false;
                           Mark(kPage);
                       }},
                      {"Top rated", mLocalByRating, false,
                       [this] {
                           mLocalByRating = true;
                           Mark(kPage);
                       }}},
                     150.0f);
    };
    toolbar.push_back(HGap(8));
    toolbar.push_back(sort);

    mLocalSummarySlot = MakeSlot();
    toolbar.push_back(HGap(8));
    toolbar.push_back(FixedWidth(mWide ? 110.0f : 90.0f, mLocalSummarySlot));

    // The grid's width: the stage less the side list, the margins and the scroll bar.
    const float stageWidth = StageWidth() - (mWide ? 245.0f : 0.0f);
    const float padLeft = mWide ? 14.0f : metrics::kPagePad;
    mLocalGridWidth = stageWidth - padLeft - 14.0f - 14.0f;
    mLocalGridSlot = MakeSlot();
    RefillTonesLocalGrid();

    auto grid = VScroll(Pad(0, 1, 4, 16, mLocalGridSlot));
    auto main = Pad(padLeft, mWide ? 12.0f : metrics::kPagePadTop, 14, 0,
                    Col({FixedHeight(30, Row(toolbar)), VGap(10), grid}));
    return WithSectionNav(std::move(nav), main);
}
} // namespace nanoq::ui
