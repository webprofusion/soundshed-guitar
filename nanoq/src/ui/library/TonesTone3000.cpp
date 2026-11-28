// Tones > Tone 3000, as the web UI's (tone3000Browser.ts, tones-tone3000.html, css/tones.css and
// the browser's parts of css/settings.css): community NAM models and IRs from Tone3000, through
// Soundshed's relay or the player's own key. Search or Favourites (favourites need the player's
// own key); a search, the sort and Search; the categories down the side (a row of chips at the
// compact size); the tones as cards with Details and Import; then how old the results are,
// Refresh, and the pages.
//
// Tone3000 is asked as little as the web UI asks it: a page of results is kept for an hour under
// the request that fetched it (uiclient/TonesBrowse.h), so a category, a page, the sort or coming
// back to the tab send nothing while that hour lasts. Search, Enter and Refresh fetch again. The
// tab's first search goes out only when it is first shown. Unlike the web UI the results are not
// kept across a restart.
//
// Import brings every model of a tone into the library; Details lists them to pick from.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "ui/library/ToneCard.h"

#include "uiclient/TonesBrowse.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;
namespace t3 = guitarfx::uiclient::tone3000;

struct Shell::Tone3000Browse
{
    bool started = false;    // the tab's first search has gone out
    bool favourites = false; // Search | Favourites
    std::string category = "pedal";
    std::string sort = "popular";
    int page = 1, totalPages = 1;
    bool loading = false;
    bool waitingForKey = false; // the last search stopped at "Add a Tone3000 API key"
    std::string error;
    std::vector<t3::Tone> tones;
    std::optional<std::chrono::system_clock::time_point> fetchedAt; // of the results on screen
    int sequence = 0;                                               // an older search's answer is dropped
    int version = 0;                                                // bumped by every change it shows
    Tone3000ResultsCache cache;
    std::shared_ptr<el::basic_input_box> box;
    Ptr boxElement;
    std::string typed; // the search as typed, run on Search or Enter
    /// The tones whose models are being imported: how many of how many are done.
    std::map<std::string, std::pair<std::size_t, std::size_t>> importing;
};

namespace
{
constexpr float kCardMin = 220.0f; // .tone3000-results: minmax(220px, 1fr)
constexpr float kCardGap = 10.0f;
constexpr float kHeroHeight = 128.0f; // one height for every picture (tones.css)
constexpr float kActionBar = 48.0f;

bool OwnKey(ToneService& service)
{
    return !service.Tone3000UsesRelay() && !service.Tone3000NeedsKey();
}

std::string ResourceTypeOf(const t3::Tone& tone)
{
    return Lower(tone.platform) == "ir" ? "ir" : "nam";
}

std::string CreatorOf(const t3::Tone& tone)
{
    auto name = !tone.creatorUsername.empty() ? tone.creatorUsername : tone.creatorName;

    while (!name.empty() && name.front() == '@')
    {
        name.erase(name.begin());
    }

    return name.empty() ? std::string("community") : name;
}

/// One of the categories (.tone3000-category): small capitals in a bordered box, the open one
/// on the selected face with a stronger edge.
class CategoryButton final : public Pressable
{
  public:
    CategoryButton(const Theme& theme, std::string label, bool active, bool chip)
        : mTheme(theme), mLabel(Upper(std::move(label))), mActive(active), mChip(chip)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Tab;
        info.name = NameOr(mLabel);
        info.selected = mActive;
        return info;
    }

    [[nodiscard]] float Width() const
    {
        return MeasureTextWidth(mLabel, LabelFont()) + mLabel.size() * 0.6f + 24.0f;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = metrics::Hit(28.0f, 36.0f);

        if (mChip)
        {
            return {{Width(), h}, {Width(), h}};
        }

        return {{80.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = metrics::Face(ctx.bounds, ctx.bounds.width(), 28.0f).inset(0.5f, 0.5f);
        cnv.fill_style(mActive ? mTheme.AccentTint() : (mHover ? mTheme.Elevated() : mTheme.Card()));
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mActive ? mTheme.Accent().opacity(0.7f) : mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, 6.0f);
        FillTrackedText(cnv, mLabel, LabelFont(), mActive ? mTheme.Ink() : mTheme.TextSecondary(),
                        el::rect{b.left + 10.0f, b.top, b.right - 10.0f, b.bottom},
                        mChip ? HAlign::Centre : HAlign::Left, 0.06f);
    }

  private:
    static el::font_descr LabelFont()
    {
        return Font(Weight::Medium).size(10.0f);
    }

    const Theme& mTheme;
    std::string mLabel;
    bool mActive, mChip;
};

/// A tone's card (.tone3000-item) under its buttons: the picture (or the initials on a wash), the
/// title and its import badge, the gear, platform and creator as chips, then its counts, over the
/// bar the buttons sit in. Drawn only: the buttons over it take the clicks.
class Tone3000CardFace final : public el::element
{
  public:
    Tone3000CardFace(const Theme& theme, t3::Tone tone, std::string badge,
                     std::function<std::shared_ptr<artist::image>(const std::string&)> image, float width, float height)
        : mTheme(theme), mTone(std::move(tone)), mBadge(std::move(badge)), mImage(std::move(image)), mWidth(width),
          mHeight(height)
    {
    }

    static el::font_descr TitleFont()
    {
        return Font(Weight::SemiBold).size(13.0f);
    }

    /// How tall a card must be in `width`: the title on one line or two.
    static float HeightFor(const t3::Tone& tone, const std::string& badge, float width)
    {
        const float room = width - 20.0f - (badge.empty() ? 0.0f : 70.0f);
        const int lines = MeasureTextWidth(tone.title, TitleFont()) > room ? 2 : 1;
        return kHeroHeight + 10.0f + static_cast<float>(lines) * 17.0f + 7.0f + 18.0f + 7.0f + 14.0f + 10.0f +
               kActionBar;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mWidth, mHeight}, {mWidth, mHeight}};
    }

    /// The picture alone, with no title, chips or bar: the details dialog's (.tone3000-details-hero).
    Tone3000CardFace& PictureOnly()
    {
        mPictureOnly = true;
        return *this;
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        const float kRadius = mPictureOnly ? 10.0f : 12.0f;
        const el::rect hero{b.left, b.top, b.right, mPictureOnly ? b.bottom : b.top + kHeroHeight};
        const el::rect bar{b.left, b.bottom - kActionBar, b.right, b.bottom};
        const auto picture = mImage ? mImage(mTone.imageUrl) : nullptr;

        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, kRadius);
            cnv.clip();
            cnv.fill_style(mTheme.Card());
            cnv.fill_rect(b);

            if (picture)
            {
                DrawImageCovering(cnv, *picture, hero);
            }
            else
            {
                const auto accent = tone_cards::AccentFor(mTone.id);
                artist::canvas::linear_gradient wash{hero.top_left(), hero.bottom_right()};
                wash.add_color_stop(0.0f, accent.opacity(0.34f));
                wash.add_color_stop(1.0f, mTheme.Accent().opacity(0.08f));
                cnv.fill_style(wash);
                cnv.fill_rect(hero);
                FillText(cnv, Tone3000Initials(mTone.title), Font(Weight::SemiBold).size(24.0f), mTheme.Ink(),
                         el::rect{hero.left + 8.0f, hero.bottom - 36.0f, hero.right - 8.0f, hero.bottom - 8.0f},
                         HAlign::Right);
            }

            if (!mPictureOnly)
            {
                cnv.fill_style(mTheme.Text().opacity(0.03f));
                cnv.fill_rect(bar);
            }
        }

        if (mPictureOnly)
        {
            cnv.stroke_style(mTheme.Border());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b, kRadius);
            return;
        }

        cnv.fill_style(mTheme.Border().opacity(0.75f));
        cnv.fill_rect(el::rect{b.left, hero.bottom, b.right, hero.bottom + 1.0f});
        cnv.fill_rect(el::rect{b.left, bar.top, b.right, bar.top + 1.0f});
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, kRadius);

        // The title, and the badge at its right (.tone3000-status).
        const float x = b.left + 10.0f, w = b.width() - 20.0f;
        float y = hero.bottom + 10.0f;
        float titleRight = x + w;

        if (!mBadge.empty())
        {
            const auto font = Font(Weight::SemiBold).size(9.0f);
            const auto text = Upper(mBadge);
            const float bw = MeasureTextWidth(text, font) + text.size() * 0.54f + 14.0f;
            const el::rect badge{x + w - bw, y + 1.0f, x + w, y + 16.0f};
            const auto ink = mBadge == "Imported" ? mTheme.Success() : mTheme.Warning();
            cnv.fill_style(ink.opacity(0.16f));
            cnv.fill_round_rect(badge, 7.5f);
            FillTrackedText(cnv, text, font, ink, badge, HAlign::Centre, 0.06f);
            titleRight = badge.left - 8.0f;
        }

        const auto title = mTone.title.empty() ? std::string("Untitled Tone") : mTone.title;
        const auto lines = tone_cards::Wrap(cnv, title, TitleFont(), titleRight - x, 2);

        for (const auto& line : lines)
        {
            FillText(cnv, line, TitleFont(), mTheme.Ink(), el::rect{x, y, titleRight, y + 17.0f}, HAlign::Left);
            y += 17.0f;
        }

        // The chips (.tone3000-meta-chip): gear, platform, @creator, as many as fit.
        y += 7.0f;
        const auto chipFont = Font(Weight::Regular).size(10.0f);
        float cx = x;

        for (const auto& chip : {mTone.gear.empty() ? std::string("tone") : mTone.gear,
                                 Upper(mTone.platform.empty() ? std::string("unknown") : mTone.platform),
                                 "@" + CreatorOf(mTone)})
        {
            const float cw = MeasureTextWidth(chip, chipFont) + 14.0f;
            const float right = std::min(cx + cw, x + w);

            if (right - cx < 30.0f)
            {
                break;
            }

            const el::rect r{cx, y, right, y + 18.0f};
            cnv.fill_style(mTheme.Input());
            cnv.fill_round_rect(r, 9.0f);
            cnv.stroke_style(mTheme.Border());
            cnv.stroke_round_rect(r.inset(0.5f, 0.5f), 9.0f);
            FillText(cnv, Ellipsize(cnv, chip, chipFont, r.width() - 12.0f), chipFont, mTheme.TextSecondary(), r,
                     HAlign::Centre);
            cx = right + 6.0f;
        }

        // The counts (.tone3000-item-stats): the number bold, the word after it.
        y += 18.0f + 7.0f;
        const auto numberFont = Font(Weight::SemiBold).size(11.0f);
        const auto wordFont = Font(Weight::Regular).size(10.0f);
        float sx = x;

        for (const auto& [number, word] :
             {std::pair{FormatCompactCount(mTone.modelsCount), std::string(" models")},
              std::pair{FormatCompactCount(std::max<std::int64_t>(0, mTone.downloads)), std::string(" downloads")}})
        {
            const float nw = MeasureTextWidth(number, numberFont);
            FillText(cnv, number, numberFont, mTheme.Ink(), el::rect{sx, y, sx + nw, y + 14.0f}, HAlign::Left);
            const float ww = MeasureTextWidth(word, wordFont);
            FillText(cnv, word, wordFont, mTheme.TextSecondary(), el::rect{sx + nw, y, sx + nw + ww, y + 14.0f},
                     HAlign::Left);
            sx += nw + ww + 12.0f;
        }
    }

  private:
    const Theme& mTheme;
    t3::Tone mTone;
    std::string mBadge;
    std::function<std::shared_ptr<artist::image>(const std::string&)> mImage;
    float mWidth, mHeight;
    bool mPictureOnly = false;
};

/// The details dialog's state: the tone, its models as they arrive, which are ticked, and the
/// import under way. `open` goes false when the dialog closes, so late answers draw nothing.
struct Tone3000Details
{
    t3::Tone tone;
    std::vector<t3::Model> models;
    std::set<std::string> selected;
    bool loading = true;
    std::string error;
    bool importing = false;
    std::size_t done = 0, total = 0;
    bool open = true;
    SlotPtr body = MakeSlot();
    std::function<void()> refill;
};
} // namespace

std::string Shell::TonesTone3000Signature() const
{
    if (!mT3)
    {
        return "none";
    }

    std::string s = std::to_string(mT3->version) + "|" + (mWide ? "W" : "") +
                    std::to_string(static_cast<int>(ViewSize().x / 32.0f)) + "|" +
                    (OwnKey(mSession.Tones()) ? "k" : "") + (mSession.Tones().Tone3000NeedsKey() ? "n" : "");

    // The import badges follow the library.
    for (const char* type : {"nam", "ir"})
    {
        if (const auto it = mState.resources.find(type); it != mState.resources.end())
        {
            s += "|" + std::to_string(it->second.size());
        }
    }

    for (const auto& [id, progress] : mT3->importing)
    {
        s += "|i" + id + ":" + std::to_string(progress.first);
    }

    return s;
}

void Shell::BrowseTone3000(int page, bool refresh)
{
    if (!mT3)
    {
        return;
    }

    auto& b = *mT3;
    auto& service = mSession.Tones();
    const int sequence = ++b.sequence;
    b.page = std::max(1, page);
    b.error.clear();
    b.waitingForKey = false;
    ++b.version;

    if (b.favourites && !OwnKey(service))
    {
        b.loading = false;
        b.tones.clear();
        b.fetchedAt.reset();
        b.totalPages = 1;
        Mark(kPage);
        return;
    }

    const auto& category = Tone3000CategoryById(b.category);
    Tone3000ResultsRequest request{b.favourites, b.category, b.favourites ? std::string() : b.typed, b.sort, b.page};

    // The hour's results first: nothing goes to Tone3000 for them.
    if (!refresh)
    {
        if (const auto cached = b.cache.Find(request, std::chrono::system_clock::now()))
        {
            b.loading = false;
            b.tones = cached->tones;
            b.page = cached->page;
            b.totalPages = cached->totalPages;
            b.fetchedAt = cached->fetchedAt;
            Mark(kPage);
            return;
        }
    }

    if (service.Tone3000NeedsKey())
    {
        b.waitingForKey = true;
        b.loading = false;
        b.tones.clear();
        b.fetchedAt.reset();
        Mark(kPage);
        return;
    }

    b.loading = true;
    b.tones.clear();
    b.fetchedAt.reset();
    Mark(kPage);

    auto done = [this, alive = std::weak_ptr<bool>(mAlive), state = std::weak_ptr(mT3), sequence, request,
                 platform = std::string(b.favourites ? "" : category.platform)](const t3::Page<t3::Tone>& result,
                                                                                const std::string& error) {
        const auto browse = state.lock();

        if (!alive.lock() || !browse || browse->sequence != sequence)
        {
            return;
        }

        browse->loading = false;
        browse->error = error;
        ++browse->version;

        if (error.empty())
        {
            Tone3000CachedResults results;
            results.request = request;
            results.tones = FilterTone3000ByPlatform(result.items, platform);
            results.page = std::max(1, result.page);
            results.totalPages = std::max(results.page, result.totalPages);
            results.fetchedAt = std::chrono::system_clock::now();
            browse->tones = results.tones;
            browse->page = results.page;
            browse->totalPages = results.totalPages;
            browse->fetchedAt = results.fetchedAt;
            browse->cache.Store(std::move(results));
        }

        Mark(kPage);
        mSession.View().refresh();
    };

    if (b.favourites)
    {
        service.Tone3000Favourites(b.page, std::move(done));
    }
    else
    {
        service.SearchTone3000(b.typed, category.gear, Tone3000SortById(b.sort).api, std::string(), b.page,
                               std::move(done));
    }
}

void Shell::ImportTone3000Tone(const t3::Tone& tone)
{
    if (!mT3 || mT3->importing.count(tone.id) > 0)
    {
        return;
    }

    mT3->importing[tone.id] = {0, 0};
    ++mT3->version;
    ShowToast("Import started", tone.title.empty() ? std::string("Tone3000") : tone.title, false);
    Mark(kPage);

    auto finish = [this, state = std::weak_ptr(mT3), id = tone.id] {
        if (const auto browse = state.lock())
        {
            browse->importing.erase(id);
            ++browse->version;
        }

        Mark(kPage);
        mSession.View().refresh();
    };

    mSession.Tones().Tone3000Models(tone.id, [this, alive = std::weak_ptr<bool>(mAlive), state = std::weak_ptr(mT3),
                                              tone, finish](const std::vector<t3::Model>& models,
                                                            const std::string& error) {
        if (!alive.lock())
        {
            return;
        }

        if (models.empty())
        {
            ShowToast("Import failed", error.empty() ? std::string("No models found for tone") : error, true);
            finish();
            return;
        }

        const auto type = ResourceTypeOf(tone);
        const auto category = type == "ir" ? tone.gear : std::string();
        // The rest of the models, in turn; each answer counts one done.
        auto remaining = std::make_shared<std::size_t>(models.size());
        auto failures = std::make_shared<std::vector<std::string>>();

        if (const auto browse = state.lock())
        {
            browse->importing[tone.id] = {0, models.size()};
            ++browse->version;
        }

        for (const auto& model : models)
        {
            mSession.Tones().ImportTone3000(
                tone, model, type, category,
                [this, alive, state, tone, remaining, failures, finish](const std::string&, const std::string& error) {
                    if (!alive.lock())
                    {
                        return;
                    }

                    if (!error.empty())
                    {
                        failures->push_back(error);
                    }

                    if (const auto browse = state.lock())
                    {
                        ++browse->importing[tone.id].first;
                        ++browse->version;
                    }

                    if (--*remaining > 0)
                    {
                        Mark(kPage);
                        return;
                    }

                    if (failures->empty())
                    {
                        ShowToast("Import complete", tone.title.empty() ? std::string("Tone3000") : tone.title, false);
                    }
                    else
                    {
                        ShowToast("Import failed", failures->front(), true);
                    }

                    finish();
                });
        }
    });
}

void Shell::OpenTone3000Details(const t3::Tone& tone)
{
    auto d = std::make_shared<Tone3000Details>();
    d->tone = tone;
    const auto view = ViewSize();
    const float width = std::min(560.0f, std::max(300.0f, view.x - 48.0f));
    const float height = std::min(620.0f, std::max(340.0f, view.y - 48.0f));
    const float textWidth = width - 40.0f;

    d->refill = [this, weak = std::weak_ptr(d), textWidth] {
        const auto d = weak.lock();

        if (!d || !d->open)
        {
            return;
        }

        const auto& tone = d->tone;
        std::vector<Ptr> rows;

        // The picture and what the tone is (.tone3000-details-hero).
        std::vector<std::string> meta;

        if (!tone.gear.empty())
        {
            meta.push_back("Gear: " + tone.gear);
        }

        if (!tone.platform.empty())
        {
            meta.push_back("Platform: " + tone.platform);
        }

        meta.push_back("Models: " + std::to_string(tone.modelsCount));

        if (const auto creator = CreatorOf(tone); creator != "community")
        {
            meta.push_back("By: " + creator);
        }

        std::string metaLine;

        for (const auto& part : meta)
        {
            metaLine += (metaLine.empty() ? "" : " \xC2\xB7 ") + part;
        }

        auto hero = std::make_shared<Tone3000CardFace>(mTheme, tone, std::string(),
                                                       [this](const std::string& url) { return RemoteImage(url); },
                                                       160.0f, kHeroHeight);
        hero->PictureOnly();
        auto metaText = std::make_shared<Paragraph>(mTheme, metaLine, textWidth - 176.0f, 11.5f);
        metaText->Colour(mTheme.ModalTextSecondary());
        rows.push_back(FixedHeight(kHeroHeight, Row({FixedWidth(160, hero), HGap(16), Middle(metaText)})));
        rows.push_back(VGap(14));

        const auto heading = [this](const std::string& text) {
            auto t = std::make_shared<Text>(mTheme, text, 12.0f, Weight::SemiBold);
            t->AccessHeading(3);
            t->Colour(mTheme.ModalText());
            return FixedHeight(22, t);
        };

        rows.push_back(heading("Description"));
        auto description = std::make_shared<Paragraph>(
            mTheme, tone.description.empty() ? std::string("No description provided.") : tone.description, textWidth,
            11.5f);
        description->Colour(mTheme.ModalTextSecondary());
        rows.push_back(description);
        rows.push_back(VGap(12));

        rows.push_back(heading("Tags"));
        {
            std::string tags;

            for (const auto& tag : tone.tags)
            {
                tags += (tags.empty() ? "" : ", ") + tag;
            }

            auto t = std::make_shared<Paragraph>(mTheme, tags.empty() ? std::string("No tags available.") : tags,
                                                 textWidth, 11.5f);
            t->Colour(mTheme.ModalTextMuted());
            rows.push_back(t);
        }
        rows.push_back(VGap(12));

        rows.push_back(heading("Models"));
        const auto status = d->loading       ? std::string("Loading models...")
                            : !d->error.empty() ? "Unable to load models: " + d->error
                            : d->models.empty() ? std::string("No models available.")
                                                : "Selected " + std::to_string(d->selected.size()) + " of " +
                                                      std::to_string(d->models.size()) + " models.";
        auto statusText = std::make_shared<Text>(mTheme, status, 11.0f);
        statusText->Colour(mTheme.ModalTextSecondary());
        rows.push_back(FixedHeight(20, statusText));

        if (!d->models.empty())
        {
            const auto button = [this](const std::string& label, float w, bool primary, bool disabled,
                                       std::function<void()> onClick) {
                auto b = std::make_shared<IconButton>(mTheme, mIcons, "",
                                                      primary ? IconButton::Look::Primary : IconButton::Look::Boxed);
                b->Size(w, 28).Label(label, 11.0f).Disabled(disabled);
                b->onClick = std::move(onClick);
                return b;
            };
            auto all = button("Select All", 84, false, d->importing, [weak] {
                if (const auto d = weak.lock())
                {
                    for (const auto& m : d->models)
                    {
                        d->selected.insert(m.id);
                    }

                    d->refill();
                }
            });
            auto none = button("Select None", 92, false, d->importing, [weak] {
                if (const auto d = weak.lock())
                {
                    d->selected.clear();
                    d->refill();
                }
            });
            auto import = button(d->importing ? "Importing..." : "Import Selected", 128, true, d->importing, [this, weak] {
                const auto d = weak.lock();

                if (!d || d->importing)
                {
                    return;
                }

                if (d->selected.empty())
                {
                    ShowToast("Import failed", "Select at least one model.", true);
                    return;
                }

                std::vector<t3::Model> chosen;

                for (const auto& m : d->models)
                {
                    if (d->selected.count(m.id) > 0)
                    {
                        chosen.push_back(m);
                    }
                }

                d->importing = true;
                d->done = 0;
                d->total = chosen.size();
                d->refill();
                const auto type = ResourceTypeOf(d->tone);
                auto failures = std::make_shared<std::vector<std::string>>();

                for (const auto& model : chosen)
                {
                    mSession.Tones().ImportTone3000(
                        d->tone, model, type, type == "ir" ? d->tone.gear : std::string(),
                        [this, alive = std::weak_ptr<bool>(mAlive), weak, failures](const std::string&,
                                                                                    const std::string& error) {
                            const auto d = weak.lock();

                            if (!alive.lock() || !d)
                            {
                                return;
                            }

                            if (!error.empty())
                            {
                                failures->push_back(error);
                            }

                            if (++d->done < d->total)
                            {
                                d->refill();
                                return;
                            }

                            d->importing = false;

                            if (failures->empty())
                            {
                                ShowToast("Import complete", d->tone.title, false);
                            }
                            else
                            {
                                ShowToast("Import failed", failures->front(), true);
                            }

                            d->refill();
                            Mark(kPage);
                        });
                }
            });
            rows.push_back(VGap(4));
            rows.push_back(FixedHeight(30, Row({all, HGap(6), none, Stretch(), import})));

            if (d->importing)
            {
                auto progress = std::make_shared<Text>(
                    mTheme, "Importing " + std::to_string(std::min(d->done + 1, d->total)) + " of " +
                                std::to_string(d->total) + "...",
                    11.0f);
                progress->Colour(mTheme.ModalTextSecondary());
                rows.push_back(FixedHeight(22, progress));
            }

            rows.push_back(VGap(6));

            for (const auto& model : d->models)
            {
                const bool imported = t3::FindImported(mState, ResourceTypeOf(tone), model) != nullptr;
                auto label = model.name.empty() ? model.id : model.name;

                if (!model.architecture.empty())
                {
                    label += "  (A" + model.architecture + ")";
                }

                if (imported)
                {
                    label += "  \xC2\xB7 in the library";
                }

                auto check = std::make_shared<CheckBox>(mTheme, label, 11.5f);
                check->Set(d->selected.count(model.id) > 0);
                check->Enabled(!d->importing);
                check->onClick = [weak, id = model.id] {
                    if (const auto d = weak.lock())
                    {
                        if (!d->selected.erase(id))
                        {
                            d->selected.insert(id);
                        }

                        d->refill();
                    }
                };
                rows.push_back(FixedHeight(30, check));
            }
        }

        Fill(*d->body, Col(rows));
        mSession.View().layout();
        mSession.View().refresh();
    };

    ShowDialog({.title = tone.title.empty() ? std::string("Tone Details") : tone.title,
                .body = VScroll(Pad(0, 0, 10, 8, d->body)),
                .width = width,
                .height = height,
                .onClose =
                    [this, d] {
                        d->open = false;
                        CloseOverlay();
                    },
                .stretch = true});
    d->refill();

    mSession.Tones().Tone3000Models(tone.id, [alive = std::weak_ptr<bool>(mAlive), weak = std::weak_ptr(d)](
                                                 const std::vector<t3::Model>& models, const std::string& error) {
        const auto d = weak.lock();

        if (!alive.lock() || !d || !d->open)
        {
            return;
        }

        d->loading = false;
        d->models = models;
        d->error = models.empty() ? error : std::string();

        // Every model ticked to begin with, as the web's.
        for (const auto& m : models)
        {
            d->selected.insert(m.id);
        }

        d->refill();
    });
}

Ptr Shell::BuildTonesTone3000()
{
    if (!mT3)
    {
        mT3 = std::make_shared<Tone3000Browse>();
    }

    auto& b = *mT3;
    auto& service = mSession.Tones();

    // The tab's first search goes out when it is first shown; coming back to it fetches only if
    // what it shows has gone stale, or the last search had no way to Tone3000 and there is one now.
    const bool stale = b.fetchedAt && !Tone3000ResultsCache::IsFresh(*b.fetchedAt, std::chrono::system_clock::now());

    if ((!b.started || stale || (b.waitingForKey && !service.Tone3000NeedsKey())) && !b.loading)
    {
        b.started = true;
        b.loading = true; // until the posted search runs
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
            if (alive.lock() && mT3)
            {
                BrowseTone3000(mT3->page, false);
            }
        });
    }

    if (!b.box)
    {
        auto [element, box] = el::input_box("Search tones\xE2\x80\xA6", Font(Weight::Regular), 1.0f);
        b.box = box;
        b.boxElement = el::share(std::move(element));
        b.box->on_text = [state = std::weak_ptr(mT3)](std::string_view text) {
            if (const auto browse = state.lock())
            {
                browse->typed = std::string(text);
            }
        };
        // Enter searches afresh, as Search does.
        b.box->on_enter = [this](std::string_view text) {
            if (mT3)
            {
                mT3->typed = std::string(text);
                BrowseTone3000(1, true);
            }

            return true;
        };
    }

    // The search keeps its focus while the page is rebuilt round it (results arriving).
    if (b.box->is_focus())
    {
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), box = std::weak_ptr(b.box)] {
            const auto field = box.lock();

            if (alive.lock() && field && mT3 && field == mT3->box)
            {
                FocusInView(mSession.View(), *field);
            }
        });
    }

    const float padX = mWide ? 24.0f : 10.0f;
    std::vector<Ptr> column;

    // The hint (.tones-tone3000-hint), left out at the compact size as the web's.
    if (mWide)
    {
        auto hint = std::make_shared<Paragraph>(
            mTheme,
            "Browse community NAM models and IRs from Tone3000. For fastest auditioning, use the resource selector "
            "directly on Neural FX and Neural Amp nodes in your signal chain.",
            StageWidth() - 2.0f * padX, 12.0f);
        hint->Colour(mTheme.TextSecondary());
        column.push_back(VGap(12));
        column.push_back(hint);
        column.push_back(VGap(10));
    }
    else
    {
        column.push_back(VGap(8));
    }

    // Search | Favourites, then the search, the sort and Search (hidden for favourites).
    auto mode = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Search", "Favourites"}, 92.0f);
    mode->AccessName("Browse");
    mode->Select(b.favourites ? 1 : 0);
    mode->Tooltip(1, "Favorites are available only when using your own Tone3000 API key");
    mode->onSelect = [this](int index) {
        if (mT3 && mT3->favourites != (index == 1))
        {
            mT3->favourites = index == 1;
            BrowseTone3000(1, false);
        }
    };
    std::vector<Ptr> controls{FixedWidth(186, Middle(mode))};

    if (!b.favourites)
    {
        auto sort = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        sort->Size(132, 30).Label(Tone3000SortById(b.sort).label, 11.0f).Caret(true);
        sort->AccessName("Sort");
        sort->onClick = [this, s = sort.get()] {
            std::vector<MenuItem> items;

            for (const auto& option : Tone3000SortOptions())
            {
                items.push_back({option.label, mT3 && mT3->sort == option.id, false, [this, id = std::string(option.id)] {
                                     if (mT3 && mT3->sort != id)
                                     {
                                         mT3->sort = id;
                                         BrowseTone3000(1, false);
                                     }
                                 }});
            }

            OpenDropdown(s->Bounds(), std::move(items), 160.0f);
        };
        auto search = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        search->Size(72, 30).Label("Search", 11.5f);
        search->onClick = [this] {
            if (mT3)
            {
                // The field keeps `typed` as it changes (on_text).
                BrowseTone3000(1, true);
            }
        };
        controls.push_back(HGap(14));
        controls.push_back(WidthRange(120.0f, el::full_extent, b.boxElement));
        controls.push_back(HGap(8));
        controls.push_back(sort);
        controls.push_back(HGap(8));
        controls.push_back(search);
    }
    else
    {
        controls.push_back(Stretch());
    }

    column.push_back(FixedHeight(30, Row(controls)));
    column.push_back(VGap(12));

    // The categories: a column at the left, or a row of chips over the results at the compact size
    // (and not for favourites, which span every category).
    std::vector<Ptr> categories;

    if (!b.favourites)
    {
        for (const auto& category : Tone3000Categories())
        {
            auto button = std::make_shared<CategoryButton>(mTheme, category.label, b.category == category.id, !mWide);
            button->onClick = [this, id = std::string(category.id)] {
                if (mT3 && mT3->category != id)
                {
                    mT3->category = id;
                    BrowseTone3000(1, false);
                }
            };

            if (!categories.empty())
            {
                categories.push_back(mWide ? VGap(7) : HGap(6));
            }

            categories.push_back(mWide ? FixedHeight(metrics::Hit(28.0f, 36.0f), button) : Ptr(button));
        }
    }

    // The results' width: the stage less the margins, the categories' column and the scroll bar.
    const bool sideCategories = mWide && !categories.empty();
    const float resultsWidth = StageWidth() - 2.0f * padX - (sideCategories ? 168.0f + 12.0f : 0.0f) - 14.0f;

    const auto message = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, text, 12.0f);
        t->Colour(mTheme.TextSecondary());
        return FixedHeight(56, Pad(4, 20, 4, 0, t));
    };

    std::vector<Ptr> results;

    if (b.favourites && !OwnKey(service))
    {
        results.push_back(message("To view your favourites, add your own Tone3000 API key under Settings."));
    }
    else if (b.waitingForKey || service.Tone3000NeedsKey())
    {
        results.push_back(message("Add a Tone3000 API key to browse models."));
    }
    else if (b.loading)
    {
        results.push_back(message("Loading..."));
    }
    else if (!b.error.empty())
    {
        results.push_back(message(b.error));
    }
    else if (b.tones.empty())
    {
        results.push_back(message(b.favourites ? "No favourite tones found. Favourite them on Tone3000 first, then refresh."
                                               : "No tones found in this category."));
    }
    else
    {
        const int columns = std::max(1, static_cast<int>((resultsWidth + kCardGap) / (kCardMin + kCardGap)));
        const float cardWidth =
            (resultsWidth - static_cast<float>(columns - 1) * kCardGap) / static_cast<float>(columns);
        const auto image = [this](const std::string& url) { return RemoteImage(url); };

        for (std::size_t i = 0; i < b.tones.size(); i += static_cast<std::size_t>(columns))
        {
            const auto end = std::min(b.tones.size(), i + static_cast<std::size_t>(columns));
            std::vector<std::string> badges;
            float height = 0.0f;

            for (std::size_t j = i; j < end; ++j)
            {
                const auto status = Tone3000ToneImportStatus(mState, b.tones[j]);
                badges.push_back(status.kind == Tone3000ImportStatus::Kind::Imported  ? "Imported"
                                 : status.kind == Tone3000ImportStatus::Kind::Partial ? "Partial"
                                                                                      : "");
                height = std::max(height, Tone3000CardFace::HeightFor(b.tones[j], badges.back(), cardWidth));
            }

            std::vector<Ptr> row;

            for (std::size_t j = i; j < end; ++j)
            {
                const auto& tone = b.tones[j];
                const auto& badge = badges[j - i];
                auto face = std::make_shared<Tone3000CardFace>(mTheme, tone, badge, image, cardWidth, height);

                // The buttons (.tone3000-details-btn, .tone3000-import-btn), sharing the bar.
                const float buttonWidth = (cardWidth - 20.0f - 6.0f) * 0.5f;
                auto details = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
                details->Size(buttonWidth, 30).Label("Details", 10.5f);
                details->AccessName("Details of " + tone.title);
                details->onClick = [this, tone] { OpenTone3000Details(tone); };

                const auto progress = b.importing.find(tone.id);
                std::string importLabel = badge.empty() ? "Import" : "Re-import";

                if (progress != b.importing.end())
                {
                    importLabel = progress->second.second == 0
                                      ? std::string("Importing...")
                                      : "Importing " + std::to_string(progress->second.first) + "/" +
                                            std::to_string(progress->second.second) + "...";
                }

                auto import = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
                import->Size(buttonWidth, 30).Label(importLabel, 10.5f).Disabled(progress != b.importing.end());
                import->AccessName(importLabel + " " + tone.title);
                import->onClick = [this, tone] { ImportTone3000Tone(tone); };

                auto buttons = FixedHeight(kActionBar, Pad(10, 8, 10, 10, Row({details, HGap(6), import})));
                auto card = Over(Col({FixedHeight(height - kActionBar, std::make_shared<NoHit>()), buttons}), face);

                if (!row.empty())
                {
                    row.push_back(HGap(kCardGap));
                }

                row.push_back(FixedWidth(cardWidth, card));
            }

            row.push_back(Stretch());

            if (!results.empty())
            {
                results.push_back(VGap(kCardGap));
            }

            results.push_back(FixedHeight(height, Row(row)));
        }
    }

    results.push_back(VGap(8));
    Ptr resultsView = VScroll(Pad(0, 0, 14, 0, Col(results)));

    if (sideCategories)
    {
        categories.push_back(std::make_shared<el::element>());
        column.push_back(Row({FixedWidth(168, Col(categories)), HGap(12), resultsView}));
    }
    else
    {
        if (!categories.empty())
        {
            float chipsWidth = 0.0f;

            for (const auto& category : Tone3000Categories())
            {
                chipsWidth += CategoryButton(mTheme, category.label, false, true).Width() + 6.0f;
            }

            column.push_back(FixedHeight(metrics::Hit(28.0f, 36.0f),
                                         MakeScroller(FixedWidth(chipsWidth, Row(categories)),
                                                      el::no_vscroll | el::no_scrollbars)));
            column.push_back(VGap(10));
        }

        column.push_back(resultsView);
    }

    // The row under the results (.tone3000-pagination): how old they are and Refresh at the left,
    // the pages at the right.
    std::vector<Ptr> pager;

    if (b.fetchedAt)
    {
        auto age = std::make_shared<Text>(
            mTheme, "Updated " + DescribeTone3000ResultsAge(*b.fetchedAt, std::chrono::system_clock::now()), 11.0f);
        age->Colour(mTheme.TextMuted());
        pager.push_back(Middle(age));
        pager.push_back(HGap(10));
    }

    const auto pageButton = [this](const std::string& label, float w, bool disabled, std::function<void()> onClick) {
        auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        button->Size(w, 28).Label(label, 11.0f).Disabled(disabled);
        button->onClick = std::move(onClick);
        return button;
    };
    auto refresh = pageButton("Refresh", 72, b.loading, [this] {
        if (mT3)
        {
            BrowseTone3000(mT3->page, true);
        }
    });
    refresh->Tooltip("Fetch these results from Tone3000 again");
    pager.push_back(refresh);
    pager.push_back(Stretch());
    pager.push_back(pageButton("Previous", 76, b.loading || b.page <= 1, [this] {
        if (mT3)
        {
            BrowseTone3000(mT3->page - 1, false);
        }
    }));
    pager.push_back(HGap(10));
    auto label = std::make_shared<Text>(
        mTheme, "Page " + std::to_string(b.page) + " of " + std::to_string(std::max(b.page, b.totalPages)), 11.0f);
    label->Colour(mTheme.TextSecondary().opacity(b.loading ? 0.6f : 1.0f));
    pager.push_back(Middle(label));
    pager.push_back(HGap(10));
    pager.push_back(pageButton("Next", 60, b.loading || b.page >= b.totalPages, [this] {
        if (mT3)
        {
            BrowseTone3000(mT3->page + 1, false);
        }
    }));
    column.push_back(VGap(10));
    column.push_back(FixedHeight(30, Row(pager)));
    column.push_back(VGap(12));

    return Pad(padX, 0, padX, 0, Col(column));
}
} // namespace nanoq::ui
