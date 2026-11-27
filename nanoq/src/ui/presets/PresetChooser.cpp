// The preset chooser, as the web UI's preset library popover (preset-library-popover.html,
// presets/popover.ts, presets/library.ts): a card hanging under the preset selector, over the
// chain, with the Presets and Multi-Rig tabs, the search, Export and a close cross; the tag chips;
// the folders at the left and, at the right, the folder tools (a name to create a folder with,
// rename and delete) over the presets as cards; and Setlists folded at the foot. It stays open
// while presets are tried, and follows the library as it changes.
//
// On a short window (ShortChooser) the list comes first: the folders are a drop-down beside the
// tag chips, with a "..." menu for the folder tools; Setlists is a third tab rather than a fold
// at the foot; and the cards are denser (one line under the name, stars at its right). The web
// UI keeps its folder column and Setlists fold at every height.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/ToneSharing.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// The chooser's short-window arrangement (BuildPresetChooser): the compact frame on a short
/// window (800x480, 640x400; not 960x600), as the web UI's data-compact-short.
bool ShortChooser(el::point view, bool wide)
{
    return !wide && view.y <= metrics::kCompactShortMaxHeight;
}

/// A five-pointed star filling `r`, as the web UI's ★.
void FillStar(el::canvas& cnv, const el::rect& r, el::color colour)
{
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f + 0.6f;
    const float outer = std::min(r.width(), r.height()) * 0.52f, inner = outer * 0.42f;
    cnv.begin_path();

    for (int i = 0; i < 10; ++i)
    {
        const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f;
        const float rad = i % 2 == 0 ? outer : inner;
        const el::point p{cx + rad * std::cos(a), cy + rad * std::sin(a)};

        if (i == 0)
        {
            cnv.move_to(p);
        }
        else
        {
            cnv.line_to(p);
        }
    }

    cnv.close_path();
    cnv.fill_style(colour);
    cnv.fill();
}

/// One preset (.preset-item): its name with "+ Mixer" (or "In Mixer") at the right; its category
/// as a badge (only when it has one) and the folders it is in; its tags as chips; its description;
/// then its rating, "3/5" or "Not Rated", and the stars. The one playing is outlined in the
/// accent, and the one loading is dimmed. A tap loads it, the stars rate it, a right click opens
/// its menu.
class PresetCard final : public Pressable
{
  public:
    PresetCard(const Theme& theme, Icons& icons, std::string name, std::string category, std::string path, int rating,
               bool active, bool mixer, bool inMixer)
        : mTheme(theme), mIcons(icons), mName(std::move(name)), mSource(std::move(category)), mPath(std::move(path)),
          mRating(rating), mActive(active), mMixer(mixer), mInMixer(inMixer)
    {
    }

    /// Dense, on a short window: two lines, the name over the category, its folders and the stars.
    static constexpr float kDenseHeight = 52.0f;

    PresetCard& Dense(bool dense)
    {
        mDense = dense;
        return *this;
    }

    PresetCard& Details(std::vector<std::string> tags, std::string description)
    {
        mTags = std::move(tags);
        mDescription = std::move(description);
        return *this;
    }

    PresetCard& Loading(bool loading)
    {
        mLoading = loading;
        return *this;
    }

    /// The card's height: the name, then each line it has, then the rating.
    [[nodiscard]] float Height() const
    {
        const bool meta = !mSource.empty() || !mPath.empty();
        return 8.0f + 30.0f + (meta ? 22.0f : 0.0f) + (mTags.empty() ? 0.0f : 22.0f) +
               (mDescription.empty() ? 0.0f : 18.0f) + 30.0f + 8.0f;
    }

    /// The mixer button: true adds it, false takes it out again.
    std::function<void(bool add)> onToggleMixer;
    std::function<void(int rating)> onRate;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.name = NameOr(mName);
        info.value = mSource + (mPath.empty() ? "" : ", " + mPath) +
                     (mRating > 0 ? ", " + std::to_string(mRating) + " stars" : std::string());
        info.selected = mActive;
        return info;
    }

    // 0 to 5 rate it, as the stars do.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (onRate && k.action == el::key_action::press && k.key >= el::key_code::_0 && k.key <= el::key_code::_5 &&
            (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0)
        {
            onRate(static_cast<int>(k.key) - static_cast<int>(el::key_code::_0));
            return true;
        }

        return Pressable::key(ctx, k);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = mDense ? metrics::Hit(kDenseHeight, kDenseHeight + 8.0f) : Height();
        return {{240.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.0f, mDense ? 3.0f : 4.0f);
        cnv.fill_style(mActive ? mTheme.AccentTint() : (mHover ? mTheme.Elevated() : mTheme.Card()));
        cnv.fill_round_rect(b, 8.0f);
        cnv.stroke_style(mActive ? mTheme.Accent().opacity(0.7f) : mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);
        DrawContent(cnv, b);

        // Loading (.preset-item.loading): dimmed, with what it is doing.
        if (mLoading)
        {
            cnv.fill_style(mTheme.Card().opacity(0.6f));
            cnv.fill_round_rect(b, 8.0f);
            FillText(cnv, "Loading\xE2\x80\xA6", Font(Weight::SemiBold).size(11.0f), mTheme.Text(), b, HAlign::Centre);
        }
    }

    void DrawContent(el::canvas& cnv, const el::rect& b)
    {
        // The name, and the mixer button at the right.
        const auto nameFont = Font(Weight::SemiBold).size(13.5f);
        const float nameRight = mMixer ? MixerRect(b).left - 8.0f : b.right - 12.0f;
        FillText(cnv, Ellipsize(cnv, mName, nameFont, nameRight - b.left - 12.0f), nameFont, mTheme.Text(),
                 el::rect{b.left + 12.0f, b.top + (mDense ? 4.0f : 8.0f), nameRight, b.top + (mDense ? 24.0f : 30.0f)},
                 HAlign::Left);

        if (mMixer)
        {
            const auto m = MixerRect(b);
            cnv.fill_style(mInMixer ? mTheme.Accent().opacity(0.14f) : mTheme.Bar());
            cnv.fill_round_rect(m, 5.0f);
            cnv.stroke_style(mInMixer ? mTheme.Accent().opacity(0.7f) : mTheme.BorderStrong());
            cnv.stroke_round_rect(m.inset(0.5f, 0.5f), 5.0f);
            FillText(cnv, mInMixer ? "\xE2\x9C\x93 In Mixer" : "+ Mixer", Font(Weight::SemiBold).size(10.0f),
                     mInMixer ? mTheme.Accent() : mTheme.TextSecondary(), m, HAlign::Centre);
        }

        // The category as a badge (only when it has one), then the folders it is in.
        const auto badgeFont = Font(Weight::SemiBold).size(9.5f);
        const float badgeTop = mDense ? b.bottom - 20.0f : b.top + 36.0f;
        el::rect badge{b.left + 12.0f, badgeTop, b.left + 6.0f, badgeTop + 16.0f};

        if (!mSource.empty())
        {
            badge.right = badge.left + MeasureTextWidth(mSource, badgeFont) + 14.0f;
            cnv.fill_style(mTheme.Text().opacity(0.1f));
            cnv.fill_round_rect(badge, 8.0f);
            FillText(cnv, mSource, badgeFont, mTheme.TextSecondary(), badge, HAlign::Centre);
        }

        // Dense, the folders stop short of the stars on the same line.
        const float pathRight = mDense ? StarSlot(b, 0).left - 8.0f : b.right - 12.0f;

        if (!mPath.empty() && pathRight > badge.right + 20.0f)
        {
            const auto pathFont = Font(Weight::Regular).size(10.5f);
            FillText(cnv, Ellipsize(cnv, mPath, pathFont, pathRight - badge.right - 8.0f), pathFont, mTheme.TextMuted(),
                     el::rect{badge.right + 6.0f, badge.top, pathRight, badge.bottom}, HAlign::Left);
        }

        if (mDense)
        {
            DrawStars(cnv, b);
            return;
        }

        float y = b.top + 8.0f + 30.0f + (!mSource.empty() || !mPath.empty() ? 22.0f : 0.0f);

        // The tags (.preset-item-tags): small chips on one line, as many as fit.
        if (!mTags.empty())
        {
            const auto tagFont = Font(Weight::Regular).size(9.5f);
            float x = b.left + 12.0f;

            for (const auto& tag : mTags)
            {
                const float w = MeasureTextWidth(tag, tagFont) + 12.0f;

                if (x + w > b.right - 12.0f)
                {
                    break;
                }

                const el::rect chip{x, y, x + w, y + 16.0f};
                cnv.fill_style(mTheme.Accent().opacity(0.12f));
                cnv.fill_round_rect(chip, 8.0f);
                FillText(cnv, tag, tagFont, mTheme.TextSecondary(), chip, HAlign::Centre);
                x += w + 4.0f;
            }

            y += 22.0f;
        }

        // The description, on one line.
        if (!mDescription.empty())
        {
            const auto descFont = Font(Weight::Regular).size(11.0f);
            FillText(cnv, Ellipsize(cnv, mDescription, descFont, b.width() - 24.0f), descFont, mTheme.TextSecondary(),
                     el::rect{b.left + 12.0f, y, b.right - 12.0f, y + 16.0f}, HAlign::Left);
        }

        // The rating: "3/5" or "Not Rated" at the left, the stars at the right.
        const auto ratingFont = Font(Weight::Regular).size(11.0f);
        const float ratingRight = metrics::Touch() ? StarSlot(b, 0).left - 4.0f : b.right - 110.0f;
        const std::string rating = mRating > 0 ? std::to_string(mRating) + "/5" : std::string("Not Rated");
        FillText(cnv, Ellipsize(cnv, rating, ratingFont, std::max(0.0f, ratingRight - b.left - 12.0f)), ratingFont,
                 mTheme.TextSecondary(), el::rect{b.left + 12.0f, b.bottom - 26.0f, ratingRight, b.bottom - 8.0f},
                 HAlign::Left);
        DrawStars(cnv, b);
    }

    void DrawStars(el::canvas& cnv, const el::rect& b)
    {
        // Filled, as the web's ★: the accent up to the rating, grey after.
        for (int i = 0; i < 5; ++i)
        {
            FillStar(cnv, StarRect(b, i),
                     i < mRating ? mTheme.Accent() : mTheme.TextMuted().opacity(mHover ? 0.65f : 0.4f));
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left)
        {
            const auto b = ctx.bounds.inset(0.0f, mDense ? 3.0f : 4.0f);

            if (mMixer && MixerRect(b).inset(-3.0f, -3.0f).includes(btn.pos))
            {
                // A second click takes it out again, as the web's.
                if (!btn.down && onToggleMixer)
                {
                    onToggleMixer(!mInMixer);
                }

                return true;
            }

            for (int i = 0; i < 5; ++i)
            {
                if (StarHit(ctx.bounds, b, i).includes(btn.pos))
                {
                    if (!btn.down && onRate)
                    {
                        onRate(i + 1 == mRating ? 0 : i + 1);
                    }

                    return true;
                }
            }
        }

        return Pressable::click(ctx, btn);
    }

  private:
    [[nodiscard]] el::rect MixerRect(const el::rect& b) const
    {
        const float top = b.top + (mDense ? 5.0f : 9.0f);
        return el::rect{b.right - 12.0f - 64.0f, top, b.right - 12.0f, top + 20.0f};
    }

    /// With touch sizes each star has a 40 px slot, the five ending 4 px in from the card's edge.
    static constexpr float kTouchStarSlot = 40.0f;

    /// A star's slot: with touch sizes 40 px, else the star and its gap (16 px), at the right of
    /// the bottom line.
    [[nodiscard]] el::rect StarSlot(const el::rect& b, int index) const
    {
        if (!metrics::Touch())
        {
            const float left = b.right - 12.0f - 5.0f * 16.0f + static_cast<float>(index) * 16.0f;
            return el::rect{left, b.bottom - StarBottom() - 13.0f, left + 16.0f, b.bottom - StarBottom()};
        }

        const float left = b.right - 4.0f - 5.0f * kTouchStarSlot + static_cast<float>(index) * kTouchStarSlot;
        return el::rect{left, b.bottom - (mDense ? 30.0f : 36.0f), left + kTouchStarSlot, b.bottom + 4.0f};
    }

    /// The stars' baseline above the card's foot: lower in a dense card.
    [[nodiscard]] float StarBottom() const
    {
        return mDense ? 6.0f : 10.0f;
    }

    [[nodiscard]] el::rect StarRect(const el::rect& b, int index) const
    {
        const auto slot = StarSlot(b, index);
        const float left = metrics::Touch() ? slot.left + (kTouchStarSlot - 13.0f) * 0.5f : slot.left;
        return el::rect{left, b.bottom - StarBottom() - 13.0f, left + 13.0f, b.bottom - StarBottom()};
    }

    /// Where a star takes a press: its 40 px slot, or 2 by 6 px round it (by 3 px, dense).
    [[nodiscard]] el::rect StarHit(const el::rect& bounds, const el::rect& b, int index) const
    {
        if (metrics::Touch())
        {
            const auto slot = StarSlot(b, index);
            return el::rect{slot.left, slot.top, slot.right, std::min(slot.bottom, bounds.bottom)};
        }

        return StarRect(b, index).inset(-2.0f, mDense ? -3.0f : -6.0f);
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::string mName, mSource, mPath;
    std::vector<std::string> mTags;
    std::string mDescription;
    int mRating;
    bool mActive, mMixer, mInMixer;
    bool mDense = false;
    bool mLoading = false;
};

/// The Setlists fold's bar (.setlist-toggle): the width of the chooser, its name at the left and a
/// chevron at the right that points up while it is open, on a faint face with an edge.
class SetlistsBar final : public Pressable
{
  public:
    SetlistsBar(const Theme& theme, Icons& icons, bool open) : mTheme(theme), mIcons(icons), mOpen(open)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr("Setlists");
        info.toggled = mOpen ? 1 : 0;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 34.0f}, {el::full_extent, 34.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Text().opacity(mHover ? 0.08f : 0.04f));
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(HasFocus() ? mTheme.Accent() : mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        FillText(cnv, "Setlists", Font(Weight::Regular).size(12.0f), mTheme.Text(),
                 el::rect{b.left + 10.0f, b.top, b.right - 30.0f, b.bottom}, HAlign::Left);
        const float cy = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, mOpen ? "ui-caret-up" : "ui-caret",
                    el::rect{b.right - 22.0f, cy - 6.0f, b.right - 10.0f, cy + 6.0f}, mTheme.TextSecondary());
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    bool mOpen;
};
} // namespace

void Shell::OpenPresetChooser(el::rect anchor)
{
    // A second tap on the selector closes it.
    if (mChooserOpen)
    {
        CloseOverlay();
        return;
    }

    CloseOverlay();
    mLoadingPresetId.clear(); // a load asked for last time and then cancelled is not loading

    // It opens on the preset playing (revealActivePresetInChooser): when the folder on show does
    // not list it, the first folder it is in, else All Presets.
    if (!mState.activePresetId.empty())
    {
        mQuery.tags = mPresetTags;
        const auto shown = FilterPresets(mState, mQuery);
        const auto& active = mState.activePresetId;

        if (std::none_of(shown.begin(), shown.end(), [&active](const PresetSummary& p) { return p.id == active; }))
        {
            std::string folderId = kPresetFolderAll;

            for (const auto& [f, depth] : FlattenPresetFolders(mState.presetFolders))
            {
                if (std::find(f->presetIds.begin(), f->presetIds.end(), active) != f->presetIds.end())
                {
                    folderId = f->id;
                    break;
                }
            }

            mQuery.folderId = folderId;
            mShowMultiRigs = false;
            mSession.Commands().SelectPresetFolder(folderId);
        }
    }

    mChooserSlot = MakeSlot();
    Fill(*mChooserSlot, BuildPresetChooser());

    // Under the selector, centred on it, as wide as the web UI's and down to the footer.
    const auto view = ViewSize();
    const float width = std::min(760.0f, view.x - 24.0f);
    const float centre =
        std::clamp((anchor.left + anchor.right) * 0.5f, 12.0f + width * 0.5f, view.x - 12.0f - width * 0.5f);
    const float footer = mWide ? metrics::kWideFooterHeight : metrics::FooterHeight();
    const float height = std::max(260.0f, view.y - anchor.bottom - footer - 14.0f);
    ShowPopover(Pad(14, 12, 14, 12, mChooserSlot),
                el::rect{centre - width * 0.5f, anchor.top, centre + width * 0.5f, anchor.bottom}, width, height);
    mChooserOpen = true;
    mOnOverlayClosed = [this] {
        mChooserOpen = false;
        mChooserSlot.reset();
        mPresetListSlot.reset();
    };

    // It follows the library, the setlists, the preset playing and the mix.
    for (const auto topic : {Topic::PresetLibrary, Topic::Setlists, Topic::ActivePreset, Topic::Mixer})
    {
        mInstrumentSubs.push_back(mSession.Client().Subscribe(topic, [this] { RebuildPresetChooser(); }));
    }
}

void Shell::RebuildPresetChooser()
{
    // Once the event that brought the change is done: a tap on something inside it (a tab, a
    // chip) would otherwise free what was tapped while its click is still running.
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
        if (alive.lock() && mChooserOpen && mChooserSlot)
        {
            Fill(*mChooserSlot, BuildPresetChooser());
            mSession.View().layout();
            mSession.View().refresh();
        }
    });
}

Ptr Shell::BuildPresetChooser()
{
    const bool multiRig = IsFeatureEnabled(mState, Feature::MultiRig);

    if (!multiRig)
    {
        mShowMultiRigs = false;
    }

    // A short window (800x480, 640x400) showed two presets under the chips, the folder tools and
    // the Setlists bar. There, as the web UI's short-screen library, the list comes first: the
    // folders are a drop-down beside the tag chips, the folder tools sit behind "...", Setlists is
    // a tab beside Presets and Multi-Rig (mChooserSetlistsOpen is then that tab), and the presets
    // take a row of two lines each.
    const bool shortScreen = ShortChooser(ViewSize(), mWide);
    const bool setlistsTab = shortScreen && mChooserSetlistsOpen;

    // ── The header (.preset-library-header): tabs, search, Export, close ──
    std::vector<Ptr> header;

    if (multiRig || shortScreen)
    {
        const auto tab = [this, shortScreen, setlistsTab](const char* label, bool rigs, bool setlists) {
            const bool chosen = setlists ? setlistsTab : !setlistsTab && mShowMultiRigs == rigs;
            auto b = std::make_shared<Button>(
                mTheme, label,
                [this, rigs, setlists, shortScreen] {
                    mShowMultiRigs = rigs;

                    if (shortScreen)
                    {
                        mChooserSetlistsOpen = setlists;
                    }

                    if (rigs)
                    {
                        mSession.Commands().GetCompositePresetList();
                    }

                    RebuildPresetChooser();
                },
                chosen ? Button::Style::Primary : Button::Style::Plain);
            b->MinWidth(shortScreen ? 72 : 84);
            return b;
        };
        header.push_back(tab("Presets", false, false));
        header.push_back(HGap(6));

        if (multiRig)
        {
            header.push_back(tab("Multi-Rig", true, false));
            header.push_back(HGap(6));
        }

        if (shortScreen)
        {
            header.push_back(tab("Setlists", false, true));
            header.push_back(HGap(6));
        }

        header.push_back(HGap(6));
    }

    if (!mSearchBox)
    {
        // Made once and kept: the chooser is rebuilt as the library changes, and a search box
        // made afresh each time would lose what is typed in it, and its focus.
        auto [element, box] = el::input_box("Search presets\xE2\x80\xA6", Font(Weight::Regular), 1.0f);
        mSearchBox = box;
        mSearchElement = el::share(std::move(element));
        mSearchBox->on_text = [this](std::string_view text) {
            mQuery.text = std::string(text);
            RebuildPresetList();
        };
    }

    header.push_back(shortScreen ? WidthRange(120.0f, el::full_extent, mSearchElement) : mSearchElement);
    header.push_back(HGap(10));

    // Export (.preset-library-export-btn): the folder on show, or every preset; named after what
    // it exports, and off when there is nothing (updatePresetFolderExportButtons).
    std::vector<std::string> exportIds;
    {
        PresetQuery q;
        q.folderId = mQuery.folderId;

        for (const auto& p : FilterPresets(mState, q))
        {
            exportIds.push_back(p.id);
        }
    }
    const auto* exportFolder = FindPresetFolder(mState.presetFolders, mQuery.folderId);
    const bool exportAll = mQuery.folderId.empty() || mQuery.folderId == kPresetFolderAll;
    const std::string exportName = exportAll                                   ? std::string("All Presets")
                                   : mQuery.folderId == kPresetFolderFavorites ? std::string("Favourites")
                                   : mQuery.folderId == kPresetFolderRecents   ? std::string("Recents")
                                   : exportFolder != nullptr                   ? exportFolder->name
                                                                               : std::string("Folder");
    const std::string exportCount = std::to_string(exportIds.size());
    const std::string exportTitle =
        !exportIds.empty() ? (exportAll ? "Export all presets (" + exportCount + ")"
                                        : "Export " + exportName + " (" + exportCount + ")")
                           : (exportAll ? std::string("No presets to export") : "No presets in " + exportName);
    auto exportButton = std::make_shared<IconButton>(mTheme, mIcons, "ui-upload", IconButton::Look::Boxed);
    exportButton->Size(32, 30).IconSize(15).Disabled(exportIds.empty());
    exportButton->AccessName(exportTitle);
    exportButton->Tooltip(exportTitle);
    exportButton->onClick = [this, exportIds, exportName, exportAll] {
        if (!exportIds.empty())
        {
            mSession.Commands().ExportPresetArchive(exportIds, exportAll ? std::string("Presets") : exportName,
                                                    mQuery.folderId, true);
        }
    };
    header.push_back(Middle(exportButton));
    header.push_back(HGap(6));
    auto close = std::make_shared<CloseCross>(mTheme);
    close->onClick = [this] { CloseOverlay(); };
    header.push_back(Middle(close));

    std::vector<Ptr> column{FixedHeight(36, Row(header)), VGap(8)};

    // ── The setlists, folded at the foot (.setlist-collapsible) ──
    auto setlistToggle = std::make_shared<SetlistsBar>(mTheme, mIcons, mChooserSetlistsOpen);
    setlistToggle->onClick = [this] {
        mChooserSetlistsOpen = !mChooserSetlistsOpen;
        RebuildPresetChooser();
    };

    if (setlistsTab)
    {
        column.push_back(BuildSetlistsView(nullptr));
        return Col(column);
    }

    // ── The tag chips (.preset-tag-filter-bar) ──
    const auto tagChips = [this] {
        std::vector<Ptr> chips;

        for (const auto& tag : tones::StandardTags())
        {
            const bool on = std::find(mPresetTags.begin(), mPresetTags.end(), tag) != mPresetTags.end();
            auto chip = std::make_shared<Button>(
                mTheme, tag,
                [this, tag, on] {
                    if (on)
                    {
                        mPresetTags.erase(std::remove(mPresetTags.begin(), mPresetTags.end(), tag), mPresetTags.end());
                    }
                    else
                    {
                        mPresetTags.push_back(tag);
                    }

                    RebuildPresetChooser();
                },
                Button::Style::Flat);
            chip->Selected(on).TextSize(11.0f);

            if (!chips.empty())
            {
                chips.push_back(HGap(2));
            }

            chips.push_back(chip);
        }

        return chips;
    };

    if (mShowMultiRigs)
    {
        column.push_back(BuildMultiRigList());
    }
    else if (shortScreen)
    {
        // The folder on show as a drop-down, the folder tools behind "...", then the chips.
        const auto* current = FindPresetFolder(mState.presetFolders, mQuery.folderId);
        const std::string folderLabel = current != nullptr                          ? current->name
                                        : mQuery.folderId == kPresetFolderRecents   ? std::string("Recents")
                                        : mQuery.folderId == kPresetFolderFavorites ? std::string("Favourites")
                                                                                    : std::string("All Presets");
        auto folders = std::make_shared<PickerField>(mTheme, mIcons);
        folders->AccessName("Folder");
        folders->Set(folderLabel);
        folders->onClick = [this, f = folders.get()] {
            // The folder tree, as the side list shows it: the three lists, then the folders.
            // Counted as the side list counts: what a virtual one lists, a folder's own presets.
            const auto listed = [this](const std::string& folderId) {
                PresetQuery q;
                q.folderId = folderId;
                return FilterPresets(mState, q).size();
            };
            std::vector<MenuItem> items;
            const auto add = [&](const std::string& id, const std::string& label, std::size_t count, int depth) {
                items.push_back({std::string(static_cast<std::size_t>(depth) * 3, ' ') + label + "  (" +
                                     std::to_string(count) + ")",
                                 mQuery.folderId == id, false, [this, id] { SelectPresetBrowserFolder(id); }});
            };
            add(kPresetFolderRecents, "Recents", listed(kPresetFolderRecents), 0);
            add(kPresetFolderFavorites, "Favourites", listed(kPresetFolderFavorites), 0);
            add(kPresetFolderAll, "All Presets", listed(kPresetFolderAll), 0);
            const auto flat = FlattenPresetFolders(mState.presetFolders);

            if (!flat.empty())
            {
                items.push_back({"Folders", false, true, {}});
            }

            for (const auto& [folder, depth] : flat)
            {
                add(folder->id, folder->name, folder->presetIds.size(), depth);
            }

            OpenDropdown(f->Bounds(), std::move(items), std::max(220.0f, f->Bounds().width()));
        };
        auto more = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        more->Size(32, 30).Label("\xE2\x80\xA6", 14.0f);
        more->AccessName("Folder tools");
        more->onClick = [this, m = more.get(), id = current != nullptr ? current->id : std::string()] {
            // The folder tools: a new folder (inside the one on show), and the one on show's own.
            std::vector<MenuItem> items{{"New folder...", false, false, [this, id] {
                                             PostPrompt("New folder", "", "Create",
                                                        [this, id](const std::string& name) {
                                                            mSession.Commands().CreatePresetFolder(name, id);
                                                        });
                                         }}};

            if (!id.empty())
            {
                items.push_back({"This folder...", false, false, [this, id] {
                                     mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), id] {
                                         if (alive.lock())
                                         {
                                             OpenFolderMenu(id);
                                         }
                                     });
                                 }});
            }

            OpenDropdown(m->Bounds(), std::move(items), 180.0f);
        };
        column.push_back(
            FixedHeight(32, Row({FixedWidth(170, folders), HGap(6), Middle(more), HGap(8), HScroll(Row(tagChips()))})));
        column.push_back(VGap(6));
        mPresetListSlot = MakeSlot();
        Fill(*mPresetListSlot, BuildPresetListContent());
        column.push_back(mPresetListSlot);
        return Col(column);
    }
    else
    {
        // The chips wrap onto as many lines as they need (.preset-tag-filter-bar is flex-wrap),
        // measured against the popover's inside.
        const float chipsWidth = std::min(760.0f, ViewSize().x - 24.0f) - 28.0f;
        const auto chipFont = Font(Weight::Medium).size(11.0f);
        std::vector<Ptr> line;
        float lineWidth = 0.0f;
        const auto endLine = [&] {
            if (!line.empty())
            {
                line.push_back(Stretch());
                column.push_back(FixedHeight(30, Row(line)));
                column.push_back(VGap(4));
                line.clear();
                lineWidth = 0.0f;
            }
        };

        const auto tags = tones::StandardTags();
        std::size_t tagIndex = 0;

        for (const auto& chip : tagChips())
        {
            // tagChips() puts a gap between chips, a chip per tag in order; the lines take their own.
            if (dynamic_cast<const Button*>(chip.get()) == nullptr || tagIndex >= tags.size())
            {
                continue;
            }

            const float w = MeasureTextWidth(tags[tagIndex++], chipFont) + 2.0f * metrics::kPad;

            if (lineWidth > 0.0f && lineWidth + 4.0f + w > chipsWidth)
            {
                endLine();
            }

            if (lineWidth > 0.0f)
            {
                line.push_back(HGap(4));
                lineWidth += 4.0f;
            }

            line.push_back(chip);
            lineWidth += w;
        }

        endLine();
        column.push_back(VGap(4));

        // ── The folder tools (.preset-folder-toolbar) ──
        if (!mFolderNameBox)
        {
            auto [element, box] = el::input_box("New Folder Name", Font(Weight::Regular), 1.0f);
            mFolderNameBox = box;
            mFolderNameElement = el::share(std::move(element));
        }

        const auto* current = FindPresetFolder(mState.presetFolders, mQuery.folderId);
        const auto tool = [this](const char* icon, bool enabled, std::function<void()> onClick, bool danger = false) {
            auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Boxed);
            b->Size(32, 30).IconSize(15).Disabled(!enabled);

            if (danger)
            {
                b->Ink(mTheme.Error());
            }

            b->onClick = std::move(onClick);
            return b;
        };
        const std::string folderId = current != nullptr ? current->id : std::string();
        const std::string folderName = current != nullptr ? current->name : std::string();
        auto create = tool("ui-folder-add", true, [this, folderId] {
            auto name = mFolderNameBox->get_utf8();
            const auto first = name.find_first_not_of(" \t");

            if (first == std::string::npos)
            {
                OpenPrompt("New folder", "", "Create", [this, folderId](const std::string& entered) {
                    mSession.Commands().CreatePresetFolder(entered, folderId);
                });
                return;
            }

            mSession.Commands().CreatePresetFolder(name.substr(first), folderId);
            mFolderNameBox->set_text("");
        });
        auto rename = tool("ui-folder-rename", current != nullptr, [this, folderId, folderName] {
            OpenPrompt("Rename folder", folderName, "Rename", [this, folderId](const std::string& entered) {
                mSession.Commands().RenamePresetFolder(folderId, entered);
            });
        });
        auto remove = tool(
            "ui-trash", current != nullptr,
            [this, folderId, folderName] {
                OpenConfirm("Delete folder", "Delete \"" + folderName + "\"? Its presets stay in the library.",
                            "Delete", [this, folderId] { mSession.Commands().DeletePresetFolder(folderId); });
            },
            true);

        mPresetListSlot = MakeSlot();
        Fill(*mPresetListSlot, BuildPresetListContent());
        auto right = Col({FixedHeight(32, Row({mFolderNameElement, HGap(6), Middle(create), HGap(4), Middle(rename),
                                               HGap(4), Middle(remove)})),
                          VGap(8), mPresetListSlot});
        column.push_back(Row({FixedWidth(220, BuildFolderColumn()), HGap(14), right}));
    }

    if (shortScreen)
    {
        return Col(column); // Setlists is a tab
    }

    column.push_back(VGap(10));
    column.push_back(FixedHeight(34, setlistToggle));

    if (mChooserSetlistsOpen)
    {
        column.push_back(VGap(8));
        column.push_back(FixedHeight(260, BuildSetlistsView(nullptr)));
    }

    return Col(column);
}

Ptr Shell::BuildPresetCard(const PresetSummary& p)
{
    const bool multiRig = IsFeatureEnabled(mState, Feature::MultiRig);
    const bool inMixer = std::find(mState.mixer.activePresetIds.begin(), mState.mixer.activePresetIds.end(), p.id) !=
                         mState.mixer.activePresetIds.end();

    // The one asked for until it is the one playing (.preset-item.loading).
    if (!mLoadingPresetId.empty() && mLoadingPresetId == mState.activePresetId)
    {
        mLoadingPresetId.clear();
    }

    auto card =
        std::make_shared<PresetCard>(mTheme, mIcons, p.name, p.category, PresetFolderPath(mState.presetFolders, p.id),
                                     PresetRating(mState, p.id), p.id == mState.activePresetId, multiRig, inMixer);
    card->Dense(ShortChooser(ViewSize(), mWide));
    card->Details(p.tags, p.description).Loading(p.id == mLoadingPresetId);
    card->onClick = [this, id = p.id] {
        if (id != mState.activePresetId)
        {
            mLoadingPresetId = id;
        }

        LoadWithConfirm(id);
        RebuildPresetChooser(); // posted: this card is still running its click
    };
    card->onMenu = [this, p](el::point) { OpenPresetRowMenu(p); };
    card->onRate = [this, id = p.id](int rating) { mSession.Commands().SetPresetRating(id, rating); };
    card->onToggleMixer = [this, id = p.id](bool add) {
        if (add)
        {
            mSession.Commands().AddActivePreset(id);
        }
        else
        {
            mSession.Commands().RemoveActivePreset(id);
        }
    };
    return card;
}
} // namespace nanoq::ui
