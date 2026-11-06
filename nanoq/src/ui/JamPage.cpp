// The Jam tab, as the web UI draws it (ui-components/panels/jam-panel.html, css/jam.css; at the
// compact size css/compact/panels.css): its sections along the top - Search and Favourites for
// backing tracks, Scales, the Riff Library and the Practice Tool - then the one open.
//
// Backing tracks are YouTube videos: the search is the shared ToneService's, with the engine's
// bundled key, and the results, the one cached search and the favourites are kept in the web UI's
// own settings (core/src/uiclient/Jam.h), so a favourite starred in either product shows in both.
// A track plays in YouTube's embedded player, which is web content: a web view of its own beside
// the results (nanoq/src/WebView.h), playing on while other pages are shown, with a bar over them
// to bring it back or stop it; where there is no web view (not Windows, or no WebView2 Runtime) it
// plays in the browser. Scales is the Scale Guide, drawn natively (ScaleGuidePage.cpp). The
// Practice Tool is PracticeToolPage.cpp's.

#include "ui/Shell.h"

#include "Log.h"
#include "Paths.h"
#include "uiclient/FeatureFlags.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
namespace jam = guitarfx::uiclient::jam;
using guitarfx::uiclient::Feature;
using guitarfx::uiclient::IsFeatureEnabled;

/// Where a web view goes: an empty dark area the layout keeps for it. It notes where it was last
/// drawn, which is where the native view is put; Elements' drawing is under that view.
class WebViewSlot final : public el::element
{
  public:
    WebViewSlot(const Theme& theme, float width, float height, std::string text)
        : mTheme(theme), mWidth(width), mHeight(height), mText(std::move(text))
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        if (mWidth <= 0.0f)
        {
            return {{60.0f, mHeight}, {el::full_extent, mHeight > 0.0f ? mHeight : el::full_extent}};
        }

        return {{mWidth, mHeight}, {mWidth, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        mBounds = ctx.bounds;
        mDrawn = true;
        auto& cnv = ctx.canvas;
        cnv.fill_style(el::rgba(0, 0, 0, 255));
        cnv.fill_round_rect(ctx.bounds, 6.0f);
        FillText(cnv, mText, Font(Weight::Regular).size(11.0f), mTheme.TextMuted(), ctx.bounds, HAlign::Centre);
    }

    /// Where it was drawn, in the view's coordinates; nothing before its first draw.
    [[nodiscard]] std::optional<el::rect> Bounds() const
    {
        return mDrawn ? std::optional<el::rect>(mBounds) : std::nullopt;
    }

  private:
    const Theme& mTheme;
    float mWidth, mHeight;
    std::string mText;
    el::rect mBounds;
    bool mDrawn = false;
};

namespace
{
constexpr float kCardGap = 10.0f;
constexpr float kPlayerGap = 12.0f;
constexpr float kBarHeight = 40.0f;
constexpr float kBarAboveFooter = 40.0f;
constexpr float kCardMin = 220.0f; // .jam-results: minmax(220px, 1fr)

/// One backing track as a card (.jam-result-card): the thumbnail with a play mark over it, the
/// title and channel, and the favourite star. With focus Enter plays it and F stars it.
class VideoCard final : public el::element, public Focusable, public TapTarget
{
  public:
    VideoCard(const Theme& theme, jam::Video video, bool favorite,
              std::function<std::shared_ptr<artist::image>(const std::string&)> image, float width)
        : mTheme(theme), mVideo(std::move(video)), mFavorite(favorite), mImage(std::move(image)), mWidth(width)
    {
    }

    std::function<void()> onPlay, onFavorite;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;
        info.name = NameOr(mVideo.title);
        info.value = mVideo.channelTitle + (mFavorite ? ", favourite" : "");
        return info;
    }

    bool Activate(const el::rect&) override
    {
        if (!onPlay)
        {
            return false;
        }

        const auto play = onPlay;
        play();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(onPlay != nullptr);
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
        if (onFavorite && k.action == el::key_action::press && k.key == el::key_code::f &&
            (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0)
        {
            const auto star = onFavorite;
            star();
            return true;
        }

        return IsPressKey(k) && Activate(ctx.bounds);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mWidth, Height()}, {mWidth, Height()}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        constexpr float kRadius = 10.0f;
        const auto art = ArtRect(b);

        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, kRadius);
            cnv.clip();
            cnv.fill_style(mTheme.Pick(el::rgba(26, 26, 33, 255), mTheme.Card()));
            cnv.fill_rect(b);
            cnv.fill_style(el::rgba(0, 0, 0, 255));
            cnv.fill_rect(art);

            if (const auto picture = mImage ? mImage(mVideo.thumbnailUrl) : nullptr)
            {
                DrawImageCovering(cnv, *picture, art);
            }

            if (mHoverArt)
            {
                cnv.fill_style(el::rgba(0, 0, 0, 70));
                cnv.fill_rect(art);
            }
        }
        cnv.stroke_style(mTheme.Pick(mHover ? el::rgba(255, 255, 255, 40) : el::rgba(255, 255, 255, 18),
                                     mHover ? mTheme.BorderStrong() : mTheme.Border()));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, kRadius);

        // The play mark, in a dark disc.
        const el::point c{art.left + art.width() * 0.5f, art.top + art.height() * 0.5f};
        cnv.fill_style(el::rgba(0, 0, 0, mHoverArt ? 200 : 150));
        cnv.add_circle(artist::circle(c, 18.0f));
        cnv.fill();
        cnv.fill_style(el::rgba(255, 255, 255, 235));
        cnv.move_to(c.x - 5.0f, c.y - 8.0f);
        cnv.line_to(c.x + 9.0f, c.y);
        cnv.line_to(c.x - 5.0f, c.y + 8.0f);
        cnv.close_path();
        cnv.fill();

        // The title (two lines at most), the channel, and the star at the right.
        const auto star = StarRect(b);
        const float x = b.left + 10.0f, w = star.left - 6.0f - x;
        float y = art.bottom + 8.0f;
        const auto titleFont = Font(Weight::SemiBold).size(11.5f);

        for (const auto& line : WrapTitle(cnv, titleFont, w))
        {
            FillText(cnv, line, titleFont, mTheme.Ink(), el::rect{x, y, x + w, y + 15.0f}, HAlign::Left);
            y += 15.0f;
        }

        const auto chipFont = Font(Weight::Medium).size(9.5f);
        const auto channel = Ellipsize(cnv, mVideo.channelTitle, chipFont, w - 12.0f);
        const float cw = el::measure_text(cnv, channel, chipFont).x + 12.0f;
        const el::rect chip{x, b.bottom - 24.0f, x + cw, b.bottom - 8.0f};
        cnv.fill_style(mTheme.Pick(el::rgba(255, 255, 255, 18), mTheme.Panel()));
        cnv.fill_round_rect(chip, 8.0f);
        FillText(cnv, channel, chipFont, mTheme.TextSecondary(), chip, HAlign::Centre);

        cnv.fill_style(mHoverStar ? mTheme.Text().opacity(0.08f) : el::rgba(0, 0, 0, 0));
        cnv.fill_round_rect(star, 6.0f);
        // U+2605 / U+2606, as the web UI's toggle.
        FillText(cnv, mFavorite ? "\xE2\x98\x85" : "\xE2\x98\x86", Font(Weight::Regular).size(17.0f),
                 mFavorite ? el::rgba(245, 190, 60, 255) : mTheme.TextSecondary(), star, HAlign::Centre);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!btn.down)
        {
            const auto b = ctx.bounds.inset(0.5f, 0.5f);

            if (StarRect(b).includes(btn.pos))
            {
                if (onFavorite)
                {
                    onFavorite();
                }
            }
            else if (ArtRect(b).includes(btn.pos) && onPlay)
            {
                onPlay();
            }
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        const bool art = inside && ArtRect(b).includes(p);
        const bool star = inside && StarRect(b).includes(p);

        if (inside != mHover || art != mHoverArt || star != mHoverStar)
        {
            mHover = inside;
            mHoverArt = art;
            mHoverStar = star;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    [[nodiscard]] float Height() const
    {
        return mWidth * 9.0f / 16.0f + 70.0f;
    }

    [[nodiscard]] el::rect ArtRect(const el::rect& b) const
    {
        return el::rect{b.left, b.top, b.right, b.top + b.width() * 9.0f / 16.0f};
    }

    [[nodiscard]] static el::rect StarRect(const el::rect& b)
    {
        return el::rect{b.right - 34.0f, b.bottom - 34.0f, b.right - 6.0f, b.bottom - 6.0f};
    }

    std::vector<std::string> WrapTitle(artist::canvas& cnv, const el::font_descr& font, float width) const
    {
        std::vector<std::string> lines;
        std::string line;
        std::size_t i = 0;
        const auto& text = mVideo.title;

        while (i < text.size() && lines.size() < 2)
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

        if (lines.size() < 2 && !line.empty())
        {
            lines.push_back(line);
        }

        if (!lines.empty() && (i < text.size() || el::measure_text(cnv, lines.back(), font).x > width))
        {
            lines.back() = Ellipsize(cnv, lines.back() + (i < text.size() ? "..." : ""), font, width);
        }

        return lines;
    }

    const Theme& mTheme;
    jam::Video mVideo;
    bool mFavorite;
    std::function<std::shared_ptr<artist::image>(const std::string&)> mImage;
    float mWidth;
    bool mHover = false, mHoverArt = false, mHoverStar = false;
};

Ptr Message(const Theme& theme, const std::string& text, bool error = false)
{
    auto t = std::make_shared<Text>(theme, text, 12.0f);
    t->Colour(error ? el::rgba(232, 110, 110, 255) : theme.TextMuted()).Alignment(Text::Align::Centre);
    return FixedHeight(60, t);
}
} // namespace

bool Shell::JamShown() const
{
    return IsFeatureEnabled(mState, Feature::Jam) || IsFeatureEnabled(mState, Feature::RiffLibrary) ||
           IsFeatureEnabled(mState, Feature::PracticeTool);
}

std::vector<Shell::JamSection> Shell::JamSections() const
{
    // The web UI's order; each section follows its feature switch (Scales is always there).
    std::vector<JamSection> sections;

    if (IsFeatureEnabled(mState, Feature::Jam))
    {
        sections.insert(sections.end(), {JamSection::Search, JamSection::Favorites});
    }

    sections.push_back(JamSection::Scales);

    if (IsFeatureEnabled(mState, Feature::RiffLibrary))
    {
        sections.push_back(JamSection::Riffs);
    }

    if (IsFeatureEnabled(mState, Feature::PracticeTool))
    {
        sections.push_back(JamSection::Practice);
    }

    return sections;
}

std::string Shell::JamPageSignature() const
{
    const auto sections = JamSections();
    std::string s = std::to_string(static_cast<int>(mJamSection)) + "|" + std::to_string(sections.size()) + "|" +
                    std::to_string(static_cast<int>(ViewSize().x / 16.0f)) + "x" +
                    std::to_string(static_cast<int>(ViewSize().y / 16.0f)) + (mJamWebViewUnavailable ? "|nw" : "");

    switch (mJamSection)
    {
    case JamSection::Search:
    case JamSection::Favorites: {
        s += "|" + mJamQuery + (mJamLoading ? "|l" : "|") + mJamError + "|v" +
             (mJamVideo ? mJamVideo->videoId : std::string());
        const auto favorites = jam::Favorites(mState.appSettings);

        for (const auto& v : mJamSection == JamSection::Search ? mJamResults : favorites)
        {
            s += "|" + v.videoId + (jam::IsFavorite(favorites, v.videoId) ? "*" : "");
        }

        s += "|" + std::to_string(favorites.size());
        break;
    }
    case JamSection::Practice:
        s += "|" + PracticeToolSignature();
        break;
    case JamSection::Scales:
        s += "|" + ScaleGuideSignature();
        break;
    case JamSection::Riffs:
        break;
    }

    return s;
}

void Shell::RunJamSearch()
{
    const auto query = jam::NormalizeQuery(mJamQuery);
    mJamSearched = true;

    if (query.empty())
    {
        mJamError = "Enter a search term to find backing tracks.";
        mJamResults.clear();
        Mark(kPage);
        return;
    }

    // The last search is kept: asked again, it answers without the network.
    if (const auto cached = jam::CachedSearch(mState.appSettings, mJamQuery))
    {
        ++mJamGeneration; // supersedes an answer still on its way
        mJamResults = cached->results;
        mJamLoading = false;
        mJamError = mJamResults.empty() ? "No matching backing tracks found." : "";
        Mark(kPage);
        return;
    }

    const auto key = jam::ApiKey(mState.appSettings);

    if (key.empty())
    {
        mJamError = "Jam search is unavailable in this build.";
        mJamResults.clear();
        Mark(kPage);
        return;
    }

    const int generation = ++mJamGeneration;
    mJamLoading = true;
    mJamError.clear();
    Mark(kPage);
    const auto rawQuery = mJamQuery;
    mSession.Tones().SearchBackingTracks(
        query, key, [this, generation, rawQuery](const std::vector<jam::Video>& videos, const std::string& error) {
            if (generation != mJamGeneration)
            {
                return;
            }

            mJamLoading = false;

            if (!error.empty())
            {
                mJamResults.clear();
                mJamError = error;
                ShowToast("Jam search failed", error, true);
            }
            else
            {
                mJamResults = videos;
                mJamError = videos.empty() ? "No matching backing tracks found." : "";
                mSession.Commands().SetSetting(jam::kSearchCacheSetting, jam::SearchCacheJson(rawQuery, videos));
            }

            Mark(kPage);
        });
}

void Shell::ToggleJamFavorite(const jam::Video& video)
{
    auto favorites = jam::ToggleFavorite(jam::Favorites(mState.appSettings), video);
    mSession.Commands().SetSetting(jam::kFavoritesSetting, jam::VideosJson(favorites));
    Mark(kPage);
}

nanoq::WebView* Shell::JamWebView()
{
    auto& view = mJamPlayerView;

    if (view && view->Failed())
    {
        view.reset();
        mJamWebViewUnavailable = true;
    }

    if (!view && !mJamWebViewUnavailable)
    {
        nanoq::WebView::Options options;
        // Beside the profile's other folders, so the player keeps its cookies (YouTube's consent).
        options.dataFolder = ProfileFolder() / "webview";
        options.allowNavigation = [](const std::string& url) { return jam::StaysInWebView(url); };
        options.openExternal = [this, alive = std::weak_ptr<bool>(mAlive)](const std::string& url) {
            if (alive.lock())
            {
                mSession.Commands().OpenUrl(url);
            }
        };
        view = nanoq::WebView::Create(static_cast<void*>(mSession.View().host()), std::move(options));

        if (!view)
        {
            mJamWebViewUnavailable = true;
        }
    }

    return view.get();
}

void Shell::PlayJamVideo(const jam::Video& video)
{
    auto* player = JamWebView();

    if (player == nullptr)
    {
        // No web view here: YouTube plays it in the browser.
        mSession.Commands().OpenUrl(jam::WatchUrl(video.videoId));
        return;
    }

    mJamVideo = video;
    player->Navigate(jam::EmbedUrl(video.videoId));
    Mark(kPage);
}

void Shell::CloseJamPlayer()
{
    mJamVideo.reset();
    // Gone, not hidden: a hidden player would play on.
    mJamPlayerView.reset();
    Mark(kPage);
}

Ptr Shell::BuildJamPlayer(float width)
{
    // The web UI's floating player (.jam-floating-player), docked beside the results: the title
    // and channel over the video, with the favourite star and close.
    const auto& video = *mJamVideo;
    auto title = std::make_shared<Text>(mTheme, video.title, 12.0f, Weight::SemiBold);
    title->Colour(mTheme.Ink());
    auto channel = std::make_shared<Text>(mTheme, video.channelTitle, 10.5f);
    channel->Colour(mTheme.TextSecondary());
    const bool favorite = jam::IsFavorite(jam::Favorites(mState.appSettings), video.videoId);
    auto star = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
    star->Size(30, 30).Label(favorite ? "\xE2\x98\x85" : "\xE2\x98\x86", 15.0f);

    if (favorite)
    {
        star->Ink(el::rgba(245, 190, 60, 255));
    }

    star->onClick = [this, video] { ToggleJamFavorite(video); };
    auto youtube = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Ghost);
    youtube->Size(30, 30).IconSize(13);
    youtube->onClick = [this, video] { mSession.Commands().OpenUrl(jam::WatchUrl(video.videoId)); };
    auto close = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
    close->Size(30, 30).Label("\xC3\x97", 16.0f);
    close->onClick = [this] { CloseJamPlayer(); };

    auto slot = std::make_shared<WebViewSlot>(mTheme, width, std::floor(width * 9.0f / 16.0f), "Loading the player...");
    mJamPlayerSlot = slot;
    auto hint = std::make_shared<Paragraph>(
        mTheme, "It plays on while you use the rest of the app; a bar over the other pages stops it.", width, 10.5f);
    hint->Colour(mTheme.TextMuted());
    return Col(
        {FixedHeight(36, Row({Col({FixedHeight(18, title), FixedHeight(16, channel)}), HGap(4), star, youtube, close})),
         VGap(6), FixedHeight(std::floor(width * 9.0f / 16.0f), slot), VGap(8), FixedHeight(30, hint), Stretch()});
}

void Shell::SyncJamWebViews()
{
    // A native window sits over everything Elements draws: it shows only while its slot is on
    // screen with nothing (a dialog, a drop-down, a toast) opened over the page.
    const bool covered = mOverlay != nullptr || mDropdown != nullptr;
    const float scale = mSession.View().scale();
    const float dpi = nanoq::WebView::PixelScale(static_cast<void*>(mSession.View().host()));
    const auto place = [&](nanoq::WebView* web, const std::weak_ptr<WebViewSlot>& weak, bool wanted) {
        if (web == nullptr)
        {
            return;
        }

        const auto slot = weak.lock();
        const auto bounds = slot ? slot->Bounds() : std::nullopt;

        if (!wanted || covered || !bounds)
        {
            web->Show(false);
            return;
        }

        const float k = scale * dpi;
        const int x = static_cast<int>(std::lround(bounds->left * k));
        const int y = static_cast<int>(std::lround(bounds->top * k));
        web->Place(x, y, static_cast<int>(std::lround(bounds->right * k)) - x,
                   static_cast<int>(std::lround(bounds->bottom * k)) - y);
        web->Show(true);
    };
    const bool jam = mPage == Page::Jam;

    if (mJamPlayerView)
    {
        place(mJamPlayerView.get(), mJamPlayerSlot,
              jam && mJamVideo && (mJamSection == JamSection::Search || mJamSection == JamSection::Favorites));
    }

    // Whether the player is making sound, in the log: what tells a hidden player still plays.
    if (const bool audible = mJamPlayerView && mJamPlayerView->PlayingAudio(); audible != mJamPlayerAudible)
    {
        mJamPlayerAudible = audible;
        nanoq::Log(std::string("Jam player ") + (audible ? "playing" : "silent") +
                   (mPage == Page::Jam ? "" : " (on another page)"));
    }

    // A view that could not start: the player goes to the browser from now on.
    if (mJamPlayerView && mJamPlayerView->Failed())
    {
        if (mJamVideo)
        {
            mSession.Commands().OpenUrl(jam::WatchUrl(mJamVideo->videoId));
        }

        mJamPlayerView.reset();
        mJamVideo.reset();
        mJamWebViewUnavailable = true;
        Mark(kPage);
    }
}

void Shell::SyncJamPlayerBar()
{
    // Away from the Jam tab, a bar over the page says what plays, with Show and Stop.
    const bool wanted = mJamVideo && mJamPlayerView && mPage != Page::Jam;
    const auto signature =
        wanted ? mJamVideo->videoId + "|" + mAppliedTheme + "|" + std::to_string(static_cast<int>(ViewSize().x / 40.0f))
               : std::string();

    if (signature == mJamPlayerBarShown)
    {
        return;
    }

    mJamPlayerBarShown = signature;

    if (mJamPlayerBar)
    {
        RemoveLayer(std::exchange(mJamPlayerBar, nullptr));
    }

    if (!wanted)
    {
        return;
    }

    auto icon = std::make_shared<IconButton>(mTheme, mIcons, "ui-jam", IconButton::Look::Ghost);
    icon->Size(24, 24).IconSize(14).Ink(mTheme.Accent());
    auto text =
        std::make_shared<Text>(mTheme, "Backing track: " + mJamVideo->title, metrics::kSmallText, Weight::Medium);
    auto show = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    show->Size(64, 28).Label("Show", 10.5f);
    show->onClick = [this] {
        // The player shows beside Search and Favourites.
        if (mJamSection != JamSection::Favorites)
        {
            mJamSection = JamSection::Search;
        }

        ShowPage(Page::Jam);
    };
    auto stop = std::make_shared<IconButton>(mTheme, mIcons, "ui-stop", IconButton::Look::Primary);
    stop->Size(70, 28).IconSize(12).Label("Stop", 10.5f);
    stop->onClick = [this] { CloseJamPlayer(); };

    const float width = std::min(520.0f, std::max(300.0f, ViewSize().x - 24.0f));
    auto content = PadXY(10, 6, Row({icon, HGap(6), text, HGap(8), show, HGap(6), stop}));
    auto face = Over(content, el::share(Surface(mTheme.Elevated(), mTheme.Accent().opacity(0.55f), 10.0f)));
    mJamPlayerBar = AvoidKeyboard(el::share(el::align_bottom(
        el::align_center(el::margin({0.0f, 0.0f, 0.0f, kBarAboveFooter},
                                    el::hold_any(FixedWidth(width, FixedHeight(kBarHeight, Expand(face)))))))));
    mSession.View().add(mJamPlayerBar, false);
    mSession.View().refresh();
}

Ptr Shell::BuildBackingTracks(float width)
{
    const bool search = mJamSection == JamSection::Search;
    std::vector<Ptr> body;

    if (search)
    {
        // The first visit searches the default query, unless the last search is kept.
        if (!mJamSearched && !mJamLoading)
        {
            mJamSearched = true;

            if (const auto cache = jam::ReadSearchCache(mState.appSettings))
            {
                mJamQuery = cache->query;
                mJamResults = cache->results;
                mJamError = mJamResults.empty() ? "No matching backing tracks found." : "";
            }
            else
            {
                if (mJamQuery.empty())
                {
                    mJamQuery = jam::kDefaultQuery;
                }

                if (!jam::ApiKey(mState.appSettings).empty() && jam::Favorites(mState.appSettings).empty())
                {
                    // After this build, so the page it marks is the next one.
                    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
                        if (alive.lock())
                        {
                            RunJamSearch();
                        }
                    });
                }
            }
        }

        if (!mJamSearchBox)
        {
            auto [element, box] =
                el::input_box("Search by artist, song, key, or style...", Font(Weight::Regular), 1.0f);
            mJamSearchBox = box;
            mJamSearchElement = el::share(std::move(element));
            mJamSearchBox->on_text = [this](std::string_view text) { mJamQuery = std::string(text); };
            mJamSearchBox->on_enter = [this](std::string_view text) {
                mJamQuery = std::string(text);
                RunJamSearch();
                return true;
            };
            mJamSearchBox->set_text(mJamQuery);
        }

        auto go = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Accent);
        go->Size(80, 30).Label(mJamLoading ? "Searching..." : "Search", 11.0f).Disabled(mJamLoading);
        go->onClick = [this] { RunJamSearch(); }; // the box keeps mJamQuery current
        body.push_back(FixedHeight(30, Row({mJamSearchElement, HGap(8), go})));
        body.push_back(VGap(10));
    }

    const auto favorites = jam::Favorites(mState.appSettings);
    const auto& list = search ? mJamResults : favorites;

    if (!IsFeatureEnabled(mState, Feature::Jam))
    {
        body.push_back(Message(mTheme, "Backing tracks are turned off in Settings > Features."));
    }
    else if (search && mJamLoading)
    {
        body.push_back(Message(mTheme, "Searching YouTube for backing tracks..."));
    }
    else if (search && !mJamError.empty())
    {
        body.push_back(Message(mTheme, mJamError, true));
    }
    else if (!search && favorites.empty())
    {
        body.push_back(Message(mTheme, "No favourites yet. Star a backing track to pin it here."));
    }
    else if (search && list.empty())
    {
        body.push_back(
            Message(mTheme, "Search for an artist, song, style, or key. \"Backing track\" is added automatically."));
    }
    else
    {
        const int columns = std::max(1, static_cast<int>((width + kCardGap) / (kCardMin + kCardGap)));
        const float cardWidth = (width - static_cast<float>(columns - 1) * kCardGap) / static_cast<float>(columns);
        auto image = [this](const std::string& url) { return RemoteImage(url); };
        std::vector<Ptr> rows;

        for (std::size_t i = 0; i < list.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> row;
            float height = 0.0f;

            for (std::size_t j = i; j < std::min(list.size(), i + static_cast<std::size_t>(columns)); ++j)
            {
                const auto video = list[j];
                auto card = std::make_shared<VideoCard>(mTheme, video, jam::IsFavorite(favorites, video.videoId), image,
                                                        cardWidth);
                card->onPlay = [this, video] { PlayJamVideo(video); };
                card->onFavorite = [this, video] { ToggleJamFavorite(video); };
                height = cardWidth * 9.0f / 16.0f + 70.0f;

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

        body.push_back(Col(rows));
    }

    body.push_back(VGap(16));
    return Col(body);
}

Ptr Shell::BuildRiffSection()
{
    // The web UI's Riff Library section: capture a new riff, or open the library (where the
    // storage folder is set too). Both are RiffDialogs.cpp's.
    auto heading = std::make_shared<Text>(mTheme, "Riff Library", 15.0f, Weight::SemiBold);
    heading->Colour(mTheme.Ink());
    auto hint = std::make_shared<Paragraph>(
        mTheme,
        "Riffs are saved as measure-aligned DI WAV takes with metadata for tempo, time signature, "
        "pattern and preset context. Capture one from your playing, then keep, audition and reuse "
        "takes from the library - or add one to the looper.",
        std::min(560.0f, StageWidth() - 32.0f), 12.0f);
    auto capture = std::make_shared<IconButton>(mTheme, mIcons, "ui-record", IconButton::Look::Accent);
    capture->Size(120, 32).IconSize(14).Label("New Riff", 11.5f);
    capture->onClick = [this] { OpenRiffCapture(false); };
    auto library = std::make_shared<IconButton>(mTheme, mIcons, "ui-folder", IconButton::Look::Boxed);
    library->Size(150, 32).IconSize(14).Label("Open Riff Library", 11.5f);
    library->onClick = [this] { OpenRiffLibrary(); };
    return Col({FixedHeight(28, heading), VGap(6), FixedHeight(60, hint), VGap(10),
                FixedHeight(32, Row({capture, HGap(10), library, Stretch()})), Stretch()});
}

Ptr Shell::BuildJamPage()
{
    const auto sections = JamSections();

    if (std::find(sections.begin(), sections.end(), mJamSection) == sections.end())
    {
        mJamSection = sections.front();
    }

    // The side list takes 245 px in the full layout.
    const float stageWidth = StageWidth() - (mWide ? 245.0f : 0.0f);
    const float contentWidth = stageWidth - 2.0f * 12.0f - 14.0f; // margins, scroll bar

    // ── The sections: the web UI's side list, chips along the top at the compact size ──
    SectionNav nav;
    const auto label = [](JamSection section) {
        switch (section)
        {
        case JamSection::Search:
            return "Search";
        case JamSection::Favorites:
            return "Favourites";
        case JamSection::Scales:
            return "Scales";
        case JamSection::Riffs:
            return "Riff Library";
        case JamSection::Practice:
            return "Practice Tool";
        }

        return "";
    };

    for (const auto section : sections)
    {
        nav.items.push_back({label(section), mJamSection == section, [this, section] {
                                 if (mJamSection == section)
                                 {
                                     return;
                                 }

                                 mJamSection = section;
                                 mJamScroll.reset(); // a section opens at its top
                                 Mark(kPage);
                             }});
    }

    nav.help = "Find a backing track, pin favourites, and keep the YouTube player playing while you use the rest "
               "of the app.";

    Ptr content;

    switch (mJamSection)
    {
    case JamSection::Search:
    case JamSection::Favorites:

        if (mJamVideo)
        {
            // The player beside the results, as wide as leaves two columns of cards when it can.
            const float playerWidth = std::floor(std::clamp(stageWidth * 0.42f, 280.0f, 460.0f));
            const float resultsWidth = contentWidth - playerWidth - kPlayerGap;
            content = Row({KeptVScroll(Pad(12, 10, 14, 0, BuildBackingTracks(resultsWidth)), mJamScroll),
                           FixedWidth(playerWidth + 12.0f, Pad(0, 10, 12, 0, BuildJamPlayer(playerWidth)))});
        }
        else
        {
            content = KeptVScroll(Pad(12, 10, 14, 0, BuildBackingTracks(contentWidth)), mJamScroll);
        }

        break;
    case JamSection::Scales:
        content = KeptVScroll(Pad(12, 10, 14, 0, BuildScaleGuide(contentWidth)), mJamScroll);
        break;
    case JamSection::Riffs:
        content = Pad(16, 14, 16, 0, BuildRiffSection());
        break;
    case JamSection::Practice:
        content = KeptVScroll(Pad(12, 10, 14, 0, BuildPracticeTool(contentWidth)), mJamScroll);
        break;
    }

    return Over(WithSectionNav(std::move(nav), content), std::make_shared<StageBackdrop>(mTheme));
}

void Shell::SyncJam()
{
    if (mPractice)
    {
        mPractice->Tick(UiSession::Now());

        // The undo banner goes after ten seconds, as the web UI's.
        if (mPractice->DeletedLoop())
        {
            if (mPracticeDeletedAt <= 0.0)
            {
                mPracticeDeletedAt = UiSession::Now();
            }
            else if (UiSession::Now() - mPracticeDeletedAt > 10.0)
            {
                mPractice->ForgetDeleted();
            }
        }
        else
        {
            mPracticeDeletedAt = 0.0;
        }
    }

    SyncJamWebViews();
    SyncJamPlayerBar();
    SyncScaleGuideListen();
}
} // namespace nanoq::ui
