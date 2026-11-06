// TonePreview.cpp - the Tones page's extras, as the web UI's toneSharingPanel has them:
//
// - A shared preset auditioned before it is installed (ToneService::Preview; the engine plays it
//   from the download and writes nothing). While one plays, a bar sits over every page, as the web
//   UI's preview indicator stands in for its preset selector: what is previewing, Install, and
//   Stop, which puts back the preset that was playing before.
// - A pack's own view (packDetail.ts): its picture, description and presets, each with Preview
//   and Install, and the pack's Share and Install.
// - Links to a shared tone (shareLinks.ts): a soundshed:// link the browser hands the standalone
//   on its command line, or a share link, opens the Tones page on that preset or pack.

#include "ui/shell/Shell.h"

#include "Paths.h"

#include <algorithm>

namespace nanoq::ui
{
namespace tones = guitarfx::uiclient::tones;

namespace
{
constexpr float kBarHeight = 40.0f;
constexpr float kBarAboveFooter = 40.0f;

/// Up to `maxLines` lines of `text` in `width`, the last cut with an ellipsis.
class WrappedText final : public el::element
{
  public:
    WrappedText(const Theme& theme, std::string text, float size, int maxLines, el::color colour)
        : mTheme(theme), mText(std::move(text)), mSize(size), mMaxLines(maxLines), mColour(colour)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = (mSize + 4.0f) * static_cast<float>(mMaxLines);
        return {{40.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto font = Font(Weight::Regular).size(mSize);
        const float width = ctx.bounds.width();
        std::string text = mText;
        std::replace_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
        float y = ctx.bounds.top;
        std::size_t i = 0;

        for (int line = 0; line < mMaxLines && i < text.size(); ++line)
        {
            // As many words as fit; the last line takes the rest, cut to fit.
            std::size_t end = i;
            std::size_t fit = i;

            while (end < text.size())
            {
                auto next = text.find(' ', end + 1);

                if (next == std::string::npos)
                {
                    next = text.size();
                }

                if (el::measure_text(cnv, text.substr(i, next - i), font).x > width && fit > i)
                {
                    break;
                }

                fit = next;
                end = next;
            }

            const bool last = line == mMaxLines - 1;
            const auto piece = last ? text.substr(i) : text.substr(i, fit - i);
            FillText(cnv, Ellipsize(cnv, piece, font, width), font, mColour,
                     el::rect{ctx.bounds.left, y, ctx.bounds.right, y + mSize + 4.0f}, HAlign::Left);
            y += mSize + 4.0f;
            i = fit + 1;
        }
    }

  private:
    const Theme& mTheme;
    std::string mText;
    float mSize;
    int mMaxLines;
    el::color mColour;
};

/// A pack's picture across the top of its view, faded into the dialog, its title over it.
class PackHero final : public el::element
{
  public:
    PackHero(const Theme& theme, std::string title, std::string count,
             std::function<std::shared_ptr<artist::image>()> picture)
        : mTheme(theme), mTitle(std::move(title)), mCount(std::move(count)), mPicture(std::move(picture))
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 110.0f}, {el::full_extent, 110.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, 10.0f);
            cnv.clip();

            if (const auto image = mPicture ? mPicture() : nullptr)
            {
                DrawImageCovering(cnv, *image, b);
            }
            else
            {
                cnv.fill_style(mTheme.Accent().opacity(0.18f));
                cnv.fill_rect(b);
            }

            artist::canvas::linear_gradient shade{b.top_left(), b.bottom_left()};
            shade.add_color_stop(0.0f, mTheme.Elevated().opacity(0.08f));
            shade.add_color_stop(0.6f, mTheme.Elevated().opacity(0.65f));
            shade.add_color_stop(1.0f, mTheme.Elevated().opacity(0.97f));
            cnv.fill_style(shade);
            cnv.fill_rect(b);
        }
        const auto titleFont = Font(Weight::SemiBold).size(17.0f);
        FillText(cnv, Ellipsize(cnv, mTitle, titleFont, b.width() - 24.0f), titleFont, mTheme.Ink(),
                 el::rect{b.left + 12.0f, b.bottom - 46.0f, b.right - 12.0f, b.bottom - 24.0f}, HAlign::Left);
        const auto countFont = Font(Weight::Medium).size(11.0f);
        FillText(cnv, mCount, countFont, mTheme.TextSecondary(),
                 el::rect{b.left + 12.0f, b.bottom - 24.0f, b.right - 12.0f, b.bottom - 8.0f}, HAlign::Left);
    }

  private:
    const Theme& mTheme;
    std::string mTitle, mCount;
    std::function<std::shared_ptr<artist::image>()> mPicture;
};
} // namespace

std::string Shell::TonePreviewSignature() const
{
    const auto& service = mSession.Tones();
    std::string s = service.PreviewItemId() + "|" + service.PreviewTitle() + (service.IsPreviewLoading() ? "|l" : "|");

    for (const auto& entry : tones::InstalledEntries(mState.appSettings))
    {
        s += "|" + entry.id;
    }

    for (const auto& [id, install] : mState.toneInstalls)
    {
        s += "|t" + id + std::to_string(static_cast<int>(install.status));
    }

    return s;
}

void Shell::SyncTonePreview()
{
    auto& service = mSession.Tones();
    auto& view = mSession.View();

    // A link the standalone was started with, once the engine has said what it has.
    if (!mToneLinkTaken && mState.haveState)
    {
        mToneLinkTaken = true;

        if (const auto target = tones::ParseLink(StandaloneCommandLine()))
        {
            OpenToneLink(*target);
        }
    }

    const auto signature =
        TonePreviewSignature() + "|" + mAppliedTheme + "|" + std::to_string(static_cast<int>(ViewSize().x / 40.0f));

    // The pack view follows what is previewing and installed.
    if (mPackDetail && mOverlay && mOverlay == mPackDetailOverlay && signature != mPackDetailSignature)
    {
        ShowPackDetail(*mPackDetail);
    }

    if (signature == mTonePreviewBarShown)
    {
        return;
    }

    mTonePreviewBarShown = signature;

    if (mTonePreviewBar)
    {
        RemoveLayer(std::exchange(mTonePreviewBar, nullptr));
    }

    if (service.PreviewItemId().empty())
    {
        return;
    }

    tones::Tone item;
    item.id = service.PreviewItemId();
    item.title = service.PreviewTitle();
    const auto entryId = tones::ItemEntryId(item.id);
    const bool installed = tones::FindInstalled(mState.appSettings, entryId).has_value();
    const bool installing = service.IsDownloading(entryId) || (mState.toneInstalls.count(entryId) > 0 &&
                                                               mState.toneInstalls.at(entryId).status ==
                                                                   guitarfx::uiclient::ToneInstall::Status::Installing);

    auto dot = std::make_shared<IconButton>(mTheme, mIcons, service.IsPreviewLoading() ? "ui-clock" : "ui-play",
                                            IconButton::Look::Ghost);
    dot->Size(24, 24).IconSize(13).Ink(mTheme.Accent());
    dot->Disabled(false);
    auto text =
        std::make_shared<Text>(mTheme, (service.IsPreviewLoading() ? "Loading preview: " : "Previewing: ") + item.title,
                               metrics::kSmallText, Weight::Medium);
    auto install =
        std::make_shared<IconButton>(mTheme, mIcons, installed ? "" : "ui-download", IconButton::Look::Boxed);
    install->Size(installed || installing ? 86.0f : 30.0f, 28)
        .Label(installed ? "Installed" : (installing ? "Installing..." : ""), 10.5f);
    install->Disabled(installed || installing);
    install->onClick = [this, item] { mSession.Tones().Install(item); };
    auto stop = std::make_shared<IconButton>(mTheme, mIcons, "ui-stop", IconButton::Look::Primary);
    stop->Size(70, 28).IconSize(12).Label("Stop", 10.5f);
    stop->onClick = [this] { mSession.Tones().StopPreview(); };

    const float width = std::min(520.0f, std::max(300.0f, ViewSize().x - 24.0f));
    auto content = PadXY(
        10, 6,
        Row({Middle(dot), HGap(6), text, Stretch(nullptr, 0.01), HGap(8), Middle(install), HGap(6), Middle(stop)}));
    auto face = Over(content, el::share(Surface(mTheme.Elevated(), mTheme.Accent().opacity(0.55f), 10.0f)));
    mTonePreviewBar = AvoidKeyboard(el::share(el::align_bottom(
        el::align_center(el::margin({0.0f, 0.0f, 0.0f, kBarAboveFooter},
                                    el::hold_any(FixedWidth(width, FixedHeight(kBarHeight, Expand(face)))))))));
    view.add(mTonePreviewBar, false);
    view.refresh();
}

void Shell::OpenToneLink(const tones::LinkTarget& target)
{
    ShowPage(Page::Tones);
    auto& service = mSession.Tones();

    if (target.kind == tones::Tone::Kind::Pack)
    {
        service.Get(tones::PackPath(target.id), [this](const tones::Response& response) {
            const auto detail = response.ok ? tones::ParsePackDetail(response.data) : std::nullopt;

            if (detail)
            {
                ShowPackDetail(*detail);
            }
            else
            {
                ShowToast("Could not open the shared pack", response.error, true);
            }
        });
        return;
    }

    mTonesTab = TonesTab::Presets;
    service.Get(tones::ItemPath(target.id), [this](const tones::Response& response) {
        auto item = response.ok ? tones::ParseItem(response.data) : std::nullopt;

        if (!item)
        {
            ShowToast("Could not open the shared preset", response.error, true);
            return;
        }

        mTonesShared = std::move(*item);
        Mark(kPage);
    });
}

void Shell::OpenPackDetail(const tones::Tone& pack)
{
    mSession.Tones().Get(tones::PackPath(pack.id), [this, pack](const tones::Response& response) {
        const auto detail = response.ok ? tones::ParsePackDetail(response.data) : std::nullopt;

        if (!detail)
        {
            ShowToast("Could not open \"" + pack.title + "\"", response.error, true);
            return;
        }

        ShowPackDetail(*detail);
    });
}

void Shell::ShowPackDetail(const tones::PackDetail& detail)
{
    auto& service = mSession.Tones();
    const auto& pack = detail.pack;
    const auto view = ViewSize();
    const float width = std::min(560.0f, std::max(300.0f, view.x - 48.0f));
    const float height = std::min(600.0f, std::max(320.0f, view.y - 48.0f));

    const auto count = std::to_string(detail.items.size()) + (detail.items.size() == 1 ? " preset" : " presets");
    auto hero = std::make_shared<PackHero>(mTheme, pack.title, count,
                                           [this, url = pack.thumbnailUrl] { return RemoteImage(url); });

    // The pack's Share and Install.
    const auto packEntry = tones::PackEntryId(pack.id);
    const bool packInstalled = tones::FindInstalled(mState.appSettings, packEntry).has_value();
    const bool packBusy = service.IsDownloading(packEntry);
    auto share = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Boxed);
    share->Size(30, 28).IconSize(13);
    share->onClick = [this, pack] { ShareTone(pack); };
    auto installPack =
        std::make_shared<IconButton>(mTheme, mIcons, packInstalled || packBusy ? "" : "ui-download",
                                     packInstalled || packBusy ? IconButton::Look::Boxed : IconButton::Look::Primary);
    installPack->Size(packInstalled || packBusy ? 96.0f : 116.0f, 28)
        .IconSize(12)
        .Label(packInstalled ? "Installed" : (packBusy ? "Installing..." : "Install pack"), 10.5f);
    installPack->Disabled(packInstalled || packBusy);
    installPack->onClick = [this, pack] { mSession.Tones().Install(pack); };

    std::vector<Ptr> rows;

    if (!pack.description.empty())
    {
        rows.push_back(std::make_shared<WrappedText>(mTheme, pack.description, 11.5f, 3, mTheme.TextSecondary()));
        rows.push_back(VGap(8));
    }

    rows.push_back(FixedHeight(30, Row({Middle(share), HGap(6), Middle(installPack), Stretch()})));
    rows.push_back(VGap(8));

    // Each preset: its title, what it is, and Preview and Install.
    std::vector<Ptr> list;

    for (const auto& entry : detail.items)
    {
        tones::Tone item;
        item.id = entry.itemId;
        item.title = entry.title;
        item.type = entry.type;
        const auto entryId = tones::ItemEntryId(item.id);
        const bool installed = packInstalled || tones::FindInstalled(mState.appSettings, entryId).has_value();
        const bool busy = service.IsDownloading(entryId) || (mState.toneInstalls.count(entryId) > 0 &&
                                                             mState.toneInstalls.at(entryId).status ==
                                                                 guitarfx::uiclient::ToneInstall::Status::Installing);
        const bool previewing = service.PreviewItemId() == item.id;

        auto title = std::make_shared<Text>(mTheme, entry.title.empty() ? item.id : entry.title, metrics::kTextSize,
                                            Weight::SemiBold);
        std::string about;

        if (!entry.type.empty() && entry.type != "preset")
        {
            about = entry.type;
        }

        if (!entry.description.empty())
        {
            about += (about.empty() ? "" : " \xC2\xB7 ") + entry.description;
        }

        for (const auto& tag : entry.tags)
        {
            about += (about.empty() ? "#" : "  #") + tag;
        }

        auto aboutText = std::make_shared<Text>(mTheme, about, 11.0f);
        aboutText->Colour(mTheme.TextSecondary());

        auto preview = std::make_shared<IconButton>(mTheme, mIcons,
                                                    previewing && !service.IsPreviewLoading() ? "ui-stop" : "ui-play",
                                                    previewing ? IconButton::Look::Accent : IconButton::Look::Boxed);
        preview->Size(30, 28).IconSize(12);
        preview->Disabled(previewing && service.IsPreviewLoading());
        preview->onClick = [this, item, previewing] {
            if (previewing)
            {
                mSession.Tones().StopPreview();
            }
            else
            {
                mSession.Tones().Preview(item);
            }
        };
        auto install =
            std::make_shared<IconButton>(mTheme, mIcons, installed || busy ? "" : "ui-download",
                                         installed || busy ? IconButton::Look::Boxed : IconButton::Look::Primary);
        install->Size(installed || busy ? 80.0f : 30.0f, 28)
            .IconSize(12)
            .Label(installed ? "Installed" : (busy ? "Installing..." : ""), 10.0f);
        install->Disabled(installed || busy);
        install->onClick = [this, item] { mSession.Tones().Install(item); };

        auto text = about.empty() ? Middle(title) : Col({VGap(4), FixedHeight(18, title), FixedHeight(16, aboutText)});
        auto row = Row({HGap(10), text, HGap(8), Middle(preview), HGap(6), Middle(install), HGap(10)});
        auto face =
            previewing
                ? Over(row, el::share(Surface(mTheme.Accent().opacity(0.10f), mTheme.Accent().opacity(0.5f), 8.0f)))
                : Over(row, el::share(Surface(mTheme.Panel(), mTheme.Border(), 8.0f)));

        if (!list.empty())
        {
            list.push_back(VGap(6));
        }

        list.push_back(FixedHeight(46, face));
    }

    if (list.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "No presets in this pack.", metrics::kSmallText);
        none->Colour(mTheme.TextMuted());
        list.push_back(FixedHeight(30, none));
    }

    auto body = Col({FixedHeight(110, hero), VGap(10), Col(rows), VScroll(Pad(0, 0, 10, 0, Col(list)))});
    mPackDetail = detail;
    mPackDetailSignature =
        TonePreviewSignature() + "|" + mAppliedTheme + "|" + std::to_string(static_cast<int>(ViewSize().x / 40.0f));
    ShowDialog({.title = pack.title, .body = body, .width = width, .height = height, .padX = 14.0f, .padY = 14.0f});
    mPackDetailOverlay = mOverlay;
}
} // namespace nanoq::ui
