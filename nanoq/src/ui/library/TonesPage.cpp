// The Tones page: Soundshed's tone sharing, as the web UI's Tones tab
// (ui-components/panels/sharing-panel.html, css/tone-sharing.css, css/compact/panels.css): the
// account and the sections (Presets, Packs, Installed, My Content) in the side list, with Create
// Pack, the Cards / List switch and a help card (chips across the top at the compact size); a
// search with a tag filter; then the tones, as cards or as rows - the latest packs in a strip that
// scrolls sideways over the community presets - each with Preview, Install and Share, a pack with
// Open for its presets. The search runs as it is typed (250 ms after the last key). Installed lists
// what this profile has installed, each with a way to remove it; My Content, the account's own
// presets and packs, with their moderation state and a way to delete them. A tone a link opened
// the page on sits over the rest. The account is one dialog: signing in, then the profile.
// Previews, a pack's own view and links are TonePreview.cpp's.
//
// The requests, the account, previewing and installing are the shared ToneService's
// (core/src/uiclient/ToneService.h); both fetch the Tone3000 models a preset names from Soundshed's
// relay first. Publishing, and making packs, are Soundshed Guitar's: Publish and Create Pack say so
// (SectionNav.cpp).

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "ui/library/ToneCard.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <ctime>

namespace nanoq::ui
{
namespace tones = guitarfx::uiclient::tones;

using namespace tone_cards;

namespace
{
/// A section's heading (.tone-sharing-row-title): small capitals.
Ptr SectionTitle(const Theme& theme, const std::string& text)
{
    auto t = std::make_shared<Text>(theme, Upper(text), 9.5f, Weight::SemiBold);
    t->AccessHeading(2); // a section title, to a screen reader
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
                    "/" + std::to_string(mToneHomeRows.size()) + "/" + std::to_string(mToneMine.size()) + "/" +
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

    const auto downloading = [&](const std::vector<tones::Tone>& list) {
        for (const auto& tone : list)
        {
            if (service.IsDownloading(tones::EntryIdFor(tone)))
            {
                s += "|d" + tone.id;
            }
        }
    };

    for (const auto* list : {&mToneItems, &mTonePacks, &mToneMine, &mToneMinePacks})
    {
        downloading(*list);
    }

    for (const auto& row : mToneHomeRows)
    {
        downloading(row.tones);
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
            mTonesError = "Load failed: " +
                          (response.error.empty() ? std::string("tone sharing could not be reached") : response.error);
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
            mToneHomeRows.clear();
        }

        if (home)
        {
            // The featured page's pack rows, each a strip over the presets, as the web's
            // featuredPackRows: its preset rows are left out, as Presets lists those itself.
            service.Get("/home", [this, generation](const tones::Response& response) {
                if (generation != mTonesGeneration || !response.ok)
                {
                    return;
                }

                mToneHomeRows.clear();

                for (auto row : tones::ParseHome(response.data))
                {
                    std::erase_if(row.tones, [](const tones::Tone& t) { return !t.IsPack(); });

                    if (!row.tones.empty())
                    {
                        mToneHomeRows.push_back(std::move(row));
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

    // Cards or List, as last chosen here or in Soundshed Guitar (they share the setting).
    if (!mTonesListRead && mState.appSettings.is_object())
    {
        mTonesListRead = true;
        const auto layout = mState.appSettings.find("toneSharing.feedLayout");
        mTonesList = layout != mState.appSettings.end() && layout->is_string() && *layout == "list";
    }

    // The search keeps its focus while the page is rebuilt round it (results arriving).
    if (mTonesSearchBox && mTonesSearchBox->is_focus() && mTonesSearchElement)
    {
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), box = std::weak_ptr(mTonesSearchBox)] {
            const auto field = box.lock();

            if (alive.lock() && field && field == mTonesSearchBox)
            {
                FocusInView(mSession.View(), *field);
            }
        });
    }

    // The side list takes 245 px in the full layout.
    const float stageWidth = StageWidth() - (mWide ? 245.0f : 0.0f);
    const float contentWidth = stageWidth - (mWide ? 14.0f : metrics::kPagePad) - 14.0f - 14.0f; // margins, scroll bar
    auto image = [this](const std::string& url) { return RemoteImage(url); };
    // A pack card at the compact size is 150 px, as the web UI's compact rule (panels.css): at
    // 206 the packs row took most of an 800x480 stage and the community presets barely showed.
    const float compactPackHeight = mWide ? kPackCardHeight : 150.0f;

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
            mTonesListRead = true;
            mSession.Commands().SetSetting("toneSharing.feedLayout", mTonesList ? "list" : "cards");
            mEffectSignature.clear();
            Mark(kPage);
        };
        return s;
    };
    nav.tools = {FixedHeight(26, createPack(208.0f)), layout(104.0f)};
    nav.compactTools = {FixedWidth(110, Middle(createPack(110.0f))), FixedWidth(112, Middle(layout(56.0f)))};
    nav.compactToolsWidth = 110.0f + 112.0f;
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
                mTonesSearchPending = false;
                mTonesTyped = std::string(text);
                mTonesQuery = mTonesTyped;
                LoadTones(false);
                Mark(kPage);
                return true;
            };
            // As it is typed: 250 ms after the last key (TickToneSearch).
            mTonesSearchBox->on_text = [this](std::string_view text) {
                mTonesTyped = std::string(text);
                mTonesSearchPending = true;
                mTonesSearchDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
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
            mTonesTyped.clear();
            mTonesSearchPending = false;
            mTonesTag.clear();

            if (mTonesSearchBox)
            {
                mTonesSearchBox->set_text("");
            }

            LoadTones(false);
            Mark(kPage);
        };
        // The field gives way down to 120 px so All tags and Clear stay in view at 640 wide (its
        // own minimum is its hint's width).
        body.push_back(FixedHeight(
            30, Row({WidthRange(120.0f, el::full_extent, mTonesSearchElement), HGap(8), tag, HGap(6), clear})));
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

        // The account's own, in My Content: deleted from tone sharing (the web's Delete).
        if (mTonesTab == TonesTab::Mine)
        {
            actions.push_back({"ui-trash", false, true, {}, [this, tone] { DeleteMyTone(tone); }});
        }

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
            // As the web's active filter: what is shown, and Clear Filter.
            auto close = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            close->Size(84, 24).Label("Clear Filter", 10.5f);
            close->onClick = [this] {
                mTonesShared.reset();
                Mark(kPage);
            };
            auto filtered = std::make_shared<Text>(mTheme, "Filtered to a shared tone.", 11.0f);
            filtered->Colour(mTheme.TextSecondary());
            body.push_back(FixedHeight(30, Row({Middle(filtered), Stretch(), Pad(0, 4, 0, 2, close)})));
            body.push_back(grid({*mTonesShared}, kPresetCardMin, kPresetCardHeight));
        }

        const bool searching = !mTonesQuery.empty() || !mTonesTag.empty();

        // Each of /home's pack rows a strip under its own title; a search shows only results.
        for (const auto& homeRow : searching ? std::vector<tones::HomeRow>{} : mToneHomeRows)
        {
            body.push_back(SectionTitle(mTheme, homeRow.title.empty() ? std::string("Latest packs") : homeRow.title));
            std::vector<Ptr> strip;

            // As rows, the strip is a line of wide bars.
            const float packWidth = mTonesList ? 320.0f : kPackCardWidth;
            const float packHeight = mTonesList ? 32.0f : compactPackHeight;

            for (const auto& pack : homeRow.tones)
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

            const float stripWidth = static_cast<float>(homeRow.tones.size()) * (packWidth + kCardGap);
            body.push_back(FixedHeight(packHeight + 14.0f, HScroll(FixedWidth(stripWidth, Row(strip)))));
        }

        // The web's texts: what is being fetched, a search with nothing, a service with nothing.
        if (mToneItems.empty())
        {
            if (mTonesLoading)
            {
                body.push_back(message(searching ? "Searching..." : "Loading..."));
            }
            else if (mTonesError.empty())
            {
                body.push_back(message(searching ? tones::NoMatchText(mTonesQuery, mTonesTag)
                                                 : std::string("No content yet. Publish the first tone.")));
            }
        }
        else
        {
            body.push_back(SectionTitle(mTheme, searching ? "Search results" : "Community presets"));
            body.push_back(grid(mToneItems, kPresetCardMin, kPresetCardHeight));

            // Load More, and how many are loaded (the web's feed footer).
            auto loadMore = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            loadMore->Size(110, 30).Label(mTonesLoading ? "Loading..." : "Load More", 11.0f);
            loadMore->Disabled(mTonesLoading || !mTonesMore);
            loadMore->onClick = [this] {
                LoadTones(true);
                Mark(kPage);
            };
            auto loaded = std::make_shared<Text>(mTheme, tones::LoadedText(mToneItems.size(), mTonesMore), 11.0f);
            loaded->Colour(mTheme.TextMuted()).Fixed(true);
            body.push_back(VGap(10));
            body.push_back(FixedHeight(30, Row({Stretch(), loadMore, HGap(12), Middle(loaded), Stretch()})));
        }

        break;
    }
    case TonesTab::Packs: {
        body.push_back(SectionTitle(mTheme, "Latest packs"));

        if (mTonePacks.empty())
        {
            body.push_back(message(mTonesLoading ? "Loading..." : "No content yet. Publish the first tone."));
        }
        else
        {
            body.push_back(grid(mTonePacks, kPackCardWidth, compactPackHeight));
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
        // As tall as the web's pack cards (.tone-sharing-pack-hero, 210 px; 150 at the compact size).
        const float kInstalledHeight = compactPackHeight;
        const auto entries = tones::InstalledEntries(mState.appSettings);

        if (entries.empty())
        {
            body.push_back(EmptyState("No installed packs yet",
                                      "Install a preset or pack from Presets or Packs, or import a zip, first.",
                                      PageRoom()));
            break;
        }

        body.push_back(SectionTitle(mTheme, "Installed packs"));

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
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, list[j], nullptr, width, kInstalledHeight);
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

            rows.push_back(FixedHeight(kInstalledHeight, Row(row)));
        }

        body.push_back(Col(rows));
        break;
    }
    case TonesTab::Mine: {
        if (!service.HasSession())
        {
            body.push_back(
                EmptyState("Sign in to view your content.", "Sign In is at the left of the sections.", PageRoom()));
            break;
        }

        // Nothing published at all: one message, not one per row.
        if (!mTonesLoading && mToneMine.empty() && mToneMinePacks.empty())
        {
            auto publish = std::make_shared<IconButton>(mTheme, mIcons, "ui-share", IconButton::Look::Accent);
            publish->Size(120, 30).IconSize(13).Label("How to publish", 11.0f);
            publish->onClick = [this] { OpenPublishInfo(); };
            const float room = PageRoom();
            body.push_back(EmptyState("Nothing published yet",
                                      "Presets and packs you publish from Soundshed Guitar show here.", room - 40.0f));
            body.push_back(FixedHeight(30, Row({Stretch(), publish, Stretch()})));
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
                body.push_back(packs ? grid(list, kPackCardWidth, compactPackHeight)
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
    auto content =
        VScroll(Pad(mWide ? 14.0f : metrics::kPagePad, mWide ? 18.0f : metrics::kPagePadTop, 14, 0, Col(body)));
    return Over(WithSectionNav(std::move(nav), content), std::make_shared<StageBackdrop>(mTheme));
}

void Shell::OpenToneAccount(el::rect)
{
    // The web's Account dialog (tone-sharing-signin-modal.html, account.ts): signed out, an email
    // and the code it is sent; signed in, the creator name and bio, saved to the profile.
    auto& service = mSession.Tones();
    const auto& account = service.Account();
    const bool signedIn = account.has_value() || service.HasSession();

    const auto label = [this](const char* text) {
        auto l = std::make_shared<Text>(mTheme, text, 12.0f);
        l->Colour(mTheme.TextSecondary());
        return FixedHeight(22, l);
    };
    const auto field = [this](const std::string& placeholder, const std::string& text) {
        auto [element, box] = el::input_box(placeholder, Font(Weight::Regular), 1.0f);
        box->set_text(text);
        return std::pair{el::share(std::move(element)), box};
    };
    auto status = std::make_shared<Text>(
        mTheme, account ? "Signed in as " + account->email : std::string(signedIn ? "Signed in" : "Signed out"), 12.0f);
    status->Colour(mTheme.TextSecondary());
    std::vector<Ptr> rows{FixedHeight(26, status), VGap(6)};
    std::vector<Ptr> actions;

    // The dialog's own fields and buttons are held weakly by its handlers, so closing it frees them.
    using Weak = std::weak_ptr<el::basic_input_box>;
    const auto read = [](const Weak& weak) {
        const auto box = weak.lock();
        return box ? box->get_utf8() : std::string();
    };

    if (!signedIn)
    {
        auto [emailElement, email] = field("you@example.com", "");
        auto [codeElement, code] = field("Enter code from email", "");
        rows.insert(rows.end(), {label("Email"), FixedHeight(36, emailElement), VGap(8), label("One-time Code"),
                                 FixedHeight(36, codeElement)});
        auto verify = DialogButton("Sign In", {}, true);
        auto send = DialogButton("Send Code", {}, false);
        verify->Enabled(false);
        const std::weak_ptr<Button> weakSend = send, weakVerify = verify;
        send->OnClick([this, read, email = Weak(email), status, weakSend, weakVerify] {
            const auto address = read(email);

            if (address.find('@') == std::string::npos)
            {
                status->Set("Enter an email address");
                mSession.View().refresh();
                return;
            }

            status->Set("Sending code...");
            mSession.View().refresh();
            mSession.Tones().RequestCode(address, [this, status, weakSend, weakVerify](const tones::Response& r) {
                status->Set(r.ok ? "Code sent. Check your email." : "Send code failed: " + r.error);
                const auto sendButton = weakSend.lock();
                const auto verifyButton = weakVerify.lock();

                if (r.ok && sendButton && verifyButton)
                {
                    sendButton->Set("Re-Send Code");
                    verifyButton->Enabled(true);
                }

                mSession.View().refresh();
            });
        });
        auto signIn = [this, read, email = Weak(email), code = Weak(code), status] {
            const auto address = read(email), entered = read(code);

            if (address.empty() || entered.empty())
            {
                status->Set("Enter email and code");
                mSession.View().refresh();
                return;
            }

            status->Set("Signing in...");
            mSession.View().refresh();
            mSession.Tones().VerifyCode(address, entered, [this, status](const tones::Response& r) {
                if (!r.ok)
                {
                    status->Set("Sign-in failed: " + r.error);
                    mSession.View().refresh();
                    return;
                }

                CloseOverlay();
                Mark(kPage);
            });
        };
        verify->OnClick(signIn);
        code->on_enter = [signIn](std::string_view) {
            signIn();
            return true;
        };
        actions = {send, verify};
    }
    else
    {
        auto [nameElement, name] = field("How your name appears on shared tones", account ? account->displayName : "");
        auto bio = el::share(el::basic_text_box(account ? account->bio : std::string(), Font(Weight::Regular)));
        auto bioField = VScroll(el::share(el::margin(el::rect{10.0f, 8.0f, 10.0f, 8.0f}, el::hold(bio))));
        auto note = std::make_shared<Text>(mTheme, "Your avatar is set in Soundshed Guitar's Account.", 11.0f);
        note->Colour(mTheme.TextMuted());
        auto profileStatus = std::make_shared<Text>(mTheme, "", 11.5f);
        profileStatus->Colour(mTheme.TextSecondary());
        rows.insert(rows.end(), {label("Creator Name"), FixedHeight(36, nameElement), VGap(8), label("Bio"),
                                 FixedHeight(76, Rounded(mTheme.Tertiary(), 3.0f, bioField)), VGap(6),
                                 FixedHeight(20, note), FixedHeight(22, profileStatus)});
        auto save = DialogButton("Save Profile", {}, true);
        save->OnClick([this, read, name = Weak(name), bio = std::weak_ptr<el::basic_text_box>(bio), profileStatus,
                       weakSave = std::weak_ptr<Button>(save)] {
            const auto trimmed = [](std::string text) {
                const auto first = text.find_first_not_of(" \t\r\n");
                return first == std::string::npos ? std::string()
                                                  : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
            };
            const auto bioBox = bio.lock();
            nlohmann::json body{{"displayName", trimmed(read(name)).substr(0, 80)},
                                {"bio", trimmed(bioBox ? bioBox->get_utf8() : std::string()).substr(0, 600)}};
            profileStatus->Set("Saving profile...");

            if (const auto button = weakSave.lock())
            {
                button->Enabled(false);
            }

            mSession.View().refresh();
            mSession.Tones().Patch(tones::kProfilePath, body,
                                   [this, profileStatus, weakSave](const tones::Response& r) {
                                       profileStatus->Set(r.ok ? "Profile saved." : "Profile save failed: " + r.error);

                                       if (const auto button = weakSave.lock())
                                       {
                                           button->Enabled(true);
                                       }

                                       if (r.ok)
                                       {
                                           mSession.Tones().RefreshAccount();
                                           LoadTones(false);
                                           Mark(kPage);
                                       }

                                       mSession.View().refresh();
                                   });
        });
        auto signOut = DialogButton(
            "Sign Out",
            [this] {
                OpenConfirm("Sign out of tone sharing", "This signs Soundshed Guitar out too: they share the account.",
                            "Sign out", [this] {
                                mSession.Tones().SignOut();
                                Mark(kPage);
                            });
            },
            false);
        actions = {signOut, save};
    }

    ShowDialog({.title = "Account",
                .body = Col(rows),
                .actions = actions,
                .width = 460.0f,
                .height = signedIn ? 420.0f : 330.0f});
}

void Shell::TickToneSearch()
{
    if (!mTonesSearchPending || std::chrono::steady_clock::now() < mTonesSearchDue)
    {
        return;
    }

    mTonesSearchPending = false;

    if (mTonesTyped == mTonesQuery)
    {
        return;
    }

    mTonesQuery = mTonesTyped;
    LoadTones(false);
    Mark(kPage);
}

void Shell::DeleteMyTone(const tones::Tone& tone)
{
    const bool pack = tone.IsPack();
    OpenConfirm(pack ? "Delete pack" : "Delete preset",
                std::string("Delete ") + (pack ? "pack" : "preset") + " \"" + tone.title + "\"? This cannot be undone.",
                "Delete", [this, pack, id = tone.id] {
                    mSession.Tones().Delete(pack ? tones::PackPath(id) : tones::ItemPath(id),
                                            [this, pack](const tones::Response& r) {
                                                if (!r.ok)
                                                {
                                                    ShowToast("Delete failed", r.error, true);
                                                    return;
                                                }

                                                ShowToast(pack ? "Pack deleted." : "Preset deleted.", {}, false);
                                                LoadTones(false);
                                                Mark(kPage);
                                            });
                });
}

void Shell::ShareTone(const tones::Tone& tone)
{
    el::set_clipboard({{"text/plain", tones::ShareLink(tone)}});
    ShowToast("Link copied", tone.title, false);
}
} // namespace nanoq::ui
