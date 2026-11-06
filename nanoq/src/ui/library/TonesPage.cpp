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
