#include "nativeui/views/TonesPage.h"

#include "nativeui/sheets/ToneSheets.h"
#include "nativeui/views/PresetsPage.h"

#include <algorithm>

namespace soundshed::nano
{
namespace tones = guitarfx::uiclient::tones;

namespace
{
/// The featured page shows this many presets across its rows; the Presets tab has the rest.
constexpr std::size_t kFeaturedPresetLimit = 10;

juce::String utf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

/// "12 Mar 2026" from the ISO time an install was recorded at.
juce::String installedDate (const std::string& iso)
{
    const auto time = juce::Time::fromISO8601 (utf8 (iso));
    return time.toMilliseconds() > 0 ? time.formatted ("%e %b %Y").trim() : juce::String();
}
} // namespace

TonesPage::TonesPage (NanoContext& contextIn, ShellActions& actionsIn)
    : context (contextIn),
      actions (actionsIn),
      service (*contextIn.tones),
      featuredTab (contextIn, "tones-tab-featured", {}, "Featured"),
      presetsTab (contextIn, "tones-tab-presets", {}, "Presets"),
      packsTab (contextIn, "tones-tab-packs", {}, "Packs"),
      installedTab (contextIn, "tones-tab-installed", {}, "Installed"),
      accountButton (contextIn, "tones-account", {}, "Sign in"),
      tagButton (contextIn, "tones-tag", {}, "All tags")
{
    setComponentID ("page-tones");

    for (auto* button : { &featuredTab, &presetsTab, &packsTab, &installedTab, &accountButton })
    {
        button->setFlat (true);
        button->setClickingTogglesState (false);
        addAndMakeVisible (*button);
    }

    featuredTab.onClick = [this] { setTab (Tab::Featured); };
    presetsTab.onClick = [this] { setTab (Tab::Presets); };
    packsTab.onClick = [this] { setTab (Tab::Packs); };
    installedTab.onClick = [this] { setTab (Tab::Installed); };
    accountButton.onClick = [this] { onAccountButton(); };
    accountButton.setTooltip ("Your Soundshed account");

    tagButton.setClickingTogglesState (false);
    tagButton.onClick = [this] { showTagMenu(); };
    addChildComponent (tagButton);

    search.setComponentID ("tones-search");
    search.setTextToShowWhenEmpty ("Search community presets", context.theme.textMuted());
    search.setFont (context.font (NanoTheme::textBody));
    search.setIndents (12, 0);
    search.setJustification (juce::Justification::centredLeft);
    search.onTextChange = [this] { startTimer (350); };
    search.onReturnKey = [this] { timerCallback(); };
    addChildComponent (search);

    list.setRowHeight (context.touch ? 64 : 54);
    list.rowCount = [this] { return (int) rows.size(); };
    list.paintRow = [this] (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed) { paintRow (g, row, bounds, pressed); };
    list.onTap = [this] (int row) { tapRow (row); };
    list.onLongPress = [this] (int row, juce::Rectangle<int> screen) { longPressRow (row, screen); };
    list.rowName = [this] (int row) {
        const auto& entry = rows[(std::size_t) row];

        switch (entry.kind)
        {
            case Row::Kind::Tone: return "tone:" + utf8 (entry.tone.title);
            case Row::Kind::Installed: return "installed:" + utf8 (entry.installed.title);
            case Row::Kind::Action: return "action:" + utf8 (entry.text);
            default: return juce::String ("row:") + juce::String (row);
        }
    };
    addAndMakeVisible (list);

    service.addListener (this);

    // What is installed lives in a setting (Session); installs in flight in toneInstalls.
    sessionSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Session, [this] { rebuildRows(); });
    installsSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Tones, [this] { list.updateContent(); });

    updateAccountButton();
    setTab (Tab::Featured);
}

TonesPage::~TonesPage()
{
    service.removeListener (this);
}

void TonesPage::activate()
{
    if (! activated)
    {
        activated = true;
        service.refreshAccount();
    }

    if (tab != Tab::Installed && ! feedFor (tab).loaded && ! feedFor (tab).loading)
        load (tab, false);
}

void TonesPage::setTab (Tab newTab)
{
    tab = newTab;
    featuredTab.setToggleState (tab == Tab::Featured, juce::dontSendNotification);
    presetsTab.setToggleState (tab == Tab::Presets, juce::dontSendNotification);
    packsTab.setToggleState (tab == Tab::Packs, juce::dontSendNotification);
    installedTab.setToggleState (tab == Tab::Installed, juce::dontSendNotification);
    search.setVisible (tab == Tab::Presets);
    tagButton.setVisible (tab == Tab::Presets);

    if (activated && tab != Tab::Installed && ! feedFor (tab).loaded && ! feedFor (tab).loading)
        load (tab, false);

    rebuildRows();
    resized();
}

// ── Loading ─────────────────────────────────────────────────────────────────────

void TonesPage::load (Tab which, bool nextPage)
{
    auto& feed = feedFor (which);

    if (! nextPage)
    {
        // A new list: whatever the last one was still waiting for is dropped when it lands.
        const int generation = feed.generation + 1;
        feed = Feed {};
        feed.generation = generation;
    }

    feed.loading = true;
    feed.error.clear();
    const int generation = feed.generation;
    const int page = feed.page + 1;
    rebuildRows();

    const auto path = which == Tab::Featured ? std::string ("/home")
                                             : (which == Tab::Presets ? tones::ItemsPath (page, query, tag) : tones::PacksPath (page));

    // The views are rebuilt on a theme change: an answer can outlive this page.
    service.get (path, [safe = juce::Component::SafePointer<TonesPage> (this), which, generation, page] (const ToneSharingService::Response& response) {
        if (safe == nullptr)
            return;

        auto& self = *safe;
        auto& target = self.feedFor (which);

        if (generation != target.generation)
            return; // the search or tag changed while this was on its way

        target.loading = false;

        if (! response.ok)
        {
            target.error = response.error;
            self.rebuildRows();
            return;
        }

        target.loaded = true;
        target.page = page;

        if (which == Tab::Featured)
        {
            target.rows = tones::ParseHome (response.data);
        }
        else
        {
            const auto more = which == Tab::Presets ? tones::ParseItems (response.data) : tones::ParsePacks (response.data);
            target.tones.insert (target.tones.end(), more.begin(), more.end());
            target.more = (int) more.size() >= (which == Tab::Presets ? tones::kItemsPageSize : tones::kPacksPageSize);
        }

        self.rebuildRows();
    });
}

void TonesPage::timerCallback()
{
    stopTimer();
    const auto text = search.getText().trim().toStdString();

    if (text != query)
    {
        query = text;
        load (Tab::Presets, false);
    }
}

// ── Rows ────────────────────────────────────────────────────────────────────────

void TonesPage::addFeedRows (Feed& feed, Tab which, const juce::String& emptyText)
{
    if (! feed.error.empty())
    {
        rows.push_back ({ Row::Kind::Message, feed.error });
        rows.push_back ({ Row::Kind::Action, "Try again", {}, {}, [this, which, next = ! feed.tones.empty()] { load (which, next); } });
        return;
    }

    if (feed.loading && feed.tones.empty() && feed.rows.empty())
    {
        rows.push_back ({ Row::Kind::Message, "Loading..." });
        return;
    }

    if (which == Tab::Featured)
    {
        // As the web UI lays it out: rows of packs first, and a handful of presets.
        auto ordered = feed.rows;
        std::stable_partition (ordered.begin(), ordered.end(), [] (const tones::HomeRow& row) {
            return std::all_of (row.tones.begin(), row.tones.end(), [] (const tones::Tone& tone) { return tone.IsPack(); });
        });

        std::size_t presetsShown = 0;
        std::size_t presetsHidden = 0;

        for (const auto& row : ordered)
        {
            std::vector<tones::Tone> shown;

            for (const auto& tone : row.tones)
            {
                if (! tone.IsPack() && presetsShown >= kFeaturedPresetLimit)
                {
                    ++presetsHidden;
                    continue;
                }

                presetsShown += tone.IsPack() ? 0 : 1;
                shown.push_back (tone);
            }

            if (shown.empty())
                continue;

            rows.push_back ({ Row::Kind::Heading, row.title });

            for (auto& tone : shown)
                rows.push_back ({ Row::Kind::Tone, {}, std::move (tone) });
        }

        if (presetsHidden > 0)
            rows.push_back ({ Row::Kind::Action, "Browse all community presets", {}, {}, [this] { setTab (Tab::Presets); } });
    }
    else
    {
        for (const auto& tone : feed.tones)
            rows.push_back ({ Row::Kind::Tone, {}, tone });

        if (feed.loading)
            rows.push_back ({ Row::Kind::Message, "Loading..." });
        else if (feed.more)
            rows.push_back ({ Row::Kind::Action, "Load more", {}, {}, [this, which] { load (which, true); } });
    }

    if (rows.empty() && feed.loaded)
        rows.push_back ({ Row::Kind::Message, emptyText.toStdString() });
}

void TonesPage::rebuildRows()
{
    rows.clear();

    switch (tab)
    {
        case Tab::Featured:
            addFeedRows (featured, Tab::Featured, "Nothing featured right now.");
            break;

        case Tab::Presets:
            addFeedRows (presets, Tab::Presets,
                         query.empty() && tag.empty() ? juce::String ("No community presets yet.") : juce::String ("No presets match."));
            break;

        case Tab::Packs:
            addFeedRows (packs, Tab::Packs, "No packs yet.");
            break;

        case Tab::Installed:
            for (auto& entry : tones::InstalledEntries (context.state().appSettings))
                rows.push_back ({ Row::Kind::Installed, {}, {}, std::move (entry) });

            if (rows.empty())
                rows.push_back ({ Row::Kind::Message, "Presets and packs you install appear here." });

            break;
    }

    list.updateContent();
}

juce::String TonesPage::statusOf (const tones::Tone& tone) const
{
    const auto entryId = tones::EntryIdFor (tone);
    const auto& installs = context.state().toneInstalls;
    const auto install = installs.find (entryId);

    if (service.isDownloading (entryId)
        || (install != installs.end() && install->second.status == guitarfx::uiclient::ToneInstall::Status::Installing))
        return "Installing...";

    return tones::FindInstalled (context.state().appSettings, entryId) ? juce::String ("Installed") : juce::String();
}

void TonesPage::paintRow (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed)
{
    if (row < 0 || row >= (int) rows.size())
        return;

    const auto& theme = context.theme;
    const auto& entry = rows[(std::size_t) row];
    const bool tappable = entry.kind == Row::Kind::Tone || entry.kind == Row::Kind::Installed || entry.kind == Row::Kind::Action;

    if (pressed && tappable)
    {
        g.setColour (theme.pressedFill());
        g.fillRoundedRectangle (bounds.toFloat().reduced (4.0f, 1.0f), (float) NanoTheme::controlRadius);
    }

    auto area = bounds.reduced (16, 6);

    switch (entry.kind)
    {
        case Row::Kind::Heading:
            g.setColour (theme.textMuted());
            g.setFont (context.font (NanoTheme::textOverline, FontWeight::semibold).withExtraKerningFactor (0.06f));
            g.drawFittedText (utf8 (entry.text).toUpperCase(), area, juce::Justification::bottomLeft, 1);
            return;

        case Row::Kind::Message:
            g.setColour (theme.textMuted());
            g.setFont (context.font (NanoTheme::textBody));
            g.drawFittedText (utf8 (entry.text), area, juce::Justification::centred, 2);
            return;

        case Row::Kind::Action:
            g.setColour (theme.accent());
            g.setFont (context.font (NanoTheme::textBody, FontWeight::medium));
            g.drawFittedText (utf8 (entry.text), area, juce::Justification::centred, 1);
            return;

        case Row::Kind::Tone:
            paintToneRow (g, entry.tone, area);
            break;

        case Row::Kind::Installed:
            paintInstalledRow (g, entry.installed, area);
            break;
    }

    // A hairline between tones, inset to the text.
    if (row + 1 < (int) rows.size() && (rows[(std::size_t) row + 1].kind == Row::Kind::Tone || rows[(std::size_t) row + 1].kind == Row::Kind::Installed))
    {
        g.setColour (theme.border().withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> ((float) area.getX() + 52.0f, (float) bounds.getBottom() - 1.0f, (float) area.getWidth() - 52.0f, 1.0f));
    }
}

void TonesPage::paintToneRow (juce::Graphics& g, const tones::Tone& tone, juce::Rectangle<int> area)
{
    const auto& theme = context.theme;

    // The pack's picture, or an icon on a tile.
    const auto tile = area.removeFromLeft (40).withSizeKeepingCentre (40, 40).toFloat();
    const auto picture = tone.thumbnailUrl.empty() ? juce::Image() : service.image (tone.thumbnailUrl);

    if (picture.isValid())
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (tile, (float) NanoTheme::controlRadius);
        g.reduceClipRegion (clip);
        g.drawImage (picture, tile, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    }
    else
    {
        g.setColour (theme.card().overlaidWith (theme.selectedFill()));
        g.fillRoundedRectangle (tile, (float) NanoTheme::controlRadius);
        context.icons->draw (g, tone.IsPack() ? "package" : "note", tile.withSizeKeepingCentre (NanoTheme::iconSize, NanoTheme::iconSize),
                             theme.textSecondary());
    }

    area.removeFromLeft (12);

    const auto status = statusOf (tone);

    if (status.isNotEmpty())
    {
        // "Installed" with a tick, or "Installing...", at the row's end.
        auto badge = area.removeFromRight (104);
        const bool installed = status == "Installed";
        g.setFont (context.font (NanoTheme::textCaption, FontWeight::medium));
        g.setColour (installed ? theme.success() : theme.textMuted());
        const int textWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), status)) + 2;
        g.drawText (status, badge.removeFromRight (textWidth), juce::Justification::centredRight);

        if (installed)
            context.icons->draw (g, "check", badge.removeFromRight (20).toFloat().withSizeKeepingCentre (14.0f, 14.0f), theme.success());
    }

    // Title, then who shared it and how often it has been taken.
    juce::StringArray details;

    if (tone.IsPack())
        details.add ("Pack");

    if (! tone.creatorName.empty())
        details.add ("by " + utf8 (tone.creatorName));
    else if (! tone.creatorHandle.empty())
        details.add ("by " + utf8 (tone.creatorHandle));

    if (tone.downloads > 0)
        details.add (utf8 (tones::FormatCount (tone.downloads)) + (tone.downloads == 1 ? " download" : " downloads"));

    if (! tone.tags.empty())
    {
        juce::StringArray tags;

        for (std::size_t i = 0; i < tone.tags.size() && i < 3; ++i)
            tags.add (utf8 (tone.tags[i]));

        details.add (tags.joinIntoString (", "));
    }

    g.setColour (theme.text());
    g.setFont (context.font (NanoTheme::textBody + 1.0f, FontWeight::medium));
    g.drawFittedText (utf8 (tone.title), area.removeFromTop (area.getHeight() * 11 / 20), juce::Justification::bottomLeft, 1);
    g.setColour (theme.textMuted());
    g.setFont (context.font (NanoTheme::textCaption));
    g.drawFittedText (details.joinIntoString (juce::String::fromUTF8 ("  \xc2\xb7  ")), area.withTrimmedTop (2),
                      juce::Justification::topLeft, 1);
}

void TonesPage::paintInstalledRow (juce::Graphics& g, const tones::InstalledEntry& entry, juce::Rectangle<int> area)
{
    const auto& theme = context.theme;
    const auto tile = area.removeFromLeft (40).withSizeKeepingCentre (40, 40).toFloat();
    g.setColour (theme.card().overlaidWith (theme.selectedFill()));
    g.fillRoundedRectangle (tile, (float) NanoTheme::controlRadius);
    context.icons->draw (g, entry.IsPack() ? "package" : "note", tile.withSizeKeepingCentre (NanoTheme::iconSize, NanoTheme::iconSize),
                         theme.textSecondary());
    area.removeFromLeft (12);

    juce::StringArray details;
    const auto count = entry.presetIds.size();

    if (entry.source == "zipImport")
        details.add ("Imported archive");
    else if (entry.IsPack())
        details.add ("Pack");

    details.add (juce::String ((int) count) + (count == 1 ? " preset" : " presets"));

    if (const auto date = installedDate (entry.importedAt); date.isNotEmpty())
        details.add (date);

    g.setColour (theme.text());
    g.setFont (context.font (NanoTheme::textBody + 1.0f, FontWeight::medium));
    g.drawFittedText (utf8 (entry.title.empty() ? entry.id : entry.title), area.removeFromTop (area.getHeight() * 11 / 20),
                      juce::Justification::bottomLeft, 1);
    g.setColour (theme.textMuted());
    g.setFont (context.font (NanoTheme::textCaption));
    g.drawFittedText (details.joinIntoString (juce::String::fromUTF8 ("  \xc2\xb7  ")), area.withTrimmedTop (2),
                      juce::Justification::topLeft, 1);
}

// ── Gestures ────────────────────────────────────────────────────────────────────

void TonesPage::tapRow (int row)
{
    if (row < 0 || row >= (int) rows.size())
        return;

    const auto entry = rows[(std::size_t) row]; // a copy: the action may rebuild the rows

    switch (entry.kind)
    {
        case Row::Kind::Tone:
            openTone (entry.tone);
            break;

        case Row::Kind::Installed:
            showInstalledMenu (entry.installed, {});
            break;

        case Row::Kind::Action:
            if (entry.action)
                entry.action();

            break;

        default:
            break;
    }
}

void TonesPage::longPressRow (int row, juce::Rectangle<int> screenBounds)
{
    if (row < 0 || row >= (int) rows.size())
        return;

    const auto entry = rows[(std::size_t) row];

    if (entry.kind == Row::Kind::Installed)
    {
        showInstalledMenu (entry.installed, screenBounds);
        return;
    }

    if (entry.kind != Row::Kind::Tone)
        return;

    const auto tone = entry.tone;
    const bool installed = tones::FindInstalled (context.state().appSettings, tones::EntryIdFor (tone)).has_value();
    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 (tone.title));
    menu.addItem ("Open", [this, tone] { openTone (tone); });
    menu.addItem ("Install", ! installed && statusOf (tone).isEmpty(), false, [this, tone] { service.install (tone); });
    menu.addItem ("Copy link", [this, tone] { copyToneLink (context, tone); });
    context.showMenu (menu, juce::PopupMenu::Options().withTargetScreenArea (screenBounds).withStandardItemHeight (context.touch ? 40 : 28));
}

void TonesPage::openTone (const tones::Tone& tone)
{
    if (actions.openSheet)
    {
        // A pack lists its presets; a preset with nothing to say needs little room.
        const int height = tone.IsPack() ? 460 : (tone.description.empty() ? 280 : 400);
        actions.openSheet (utf8 (tone.title), std::make_unique<ToneDetailContent> (context, actions, tone), { 560, height });
    }
}

void TonesPage::showInstalledMenu (const tones::InstalledEntry& entry, juce::Rectangle<int> screenBounds)
{
    juce::PopupMenu menu;
    const auto title = utf8 (entry.title.empty() ? entry.id : entry.title);
    menu.addSectionHeader (title);

    // Load its first preset that is still in the library.
    std::string firstPreset;

    for (const auto& id : entry.presetIds)
    {
        if (context.state().FindPresetSummary (id) != nullptr)
        {
            firstPreset = id;
            break;
        }
    }

    menu.addItem (entry.presetIds.size() > 1 ? "Load its first preset" : "Load", ! firstPreset.empty(), false,
                  [this, firstPreset] { PresetsPage::loadWithConfirm (context, actions, firstPreset); });

    menu.addItem ("Remove...", [this, entry, title] {
        if (! actions.confirm)
            return;

        const auto count = entry.presetIds.size();
        actions.confirm ("Remove \"" + title + "\"",
                         (count == 1 ? juce::String ("Its preset") : "Its " + juce::String ((int) count) + " presets")
                             + " will be deleted, with the models and IRs nothing else uses. This cannot be undone.",
                         "Remove", [this, id = entry.id] { context.commands.DeleteInstalledPresetArchive (id); });
    });

    // A tap has no row bounds to hand: the menu opens at the finger.
    auto options = juce::PopupMenu::Options().withStandardItemHeight (context.touch ? 40 : 28);
    context.showMenu (menu, screenBounds.isEmpty() ? options.withMousePosition() : options.withTargetScreenArea (screenBounds));
}

void TonesPage::showTagMenu()
{
    juce::PopupMenu menu;
    const auto choose = [this] (const std::string& newTag) {
        if (newTag == tag)
            return;

        tag = newTag;
        tagButton.setText (tag.empty() ? juce::String ("All tags") : utf8 (tag));
        load (Tab::Presets, false);
    };

    menu.addItem ("All tags", true, tag.empty(), [choose] { choose ({}); });
    menu.addSeparator();

    for (const auto& standard : tones::StandardTags())
        menu.addItem (utf8 (standard), true, tag == standard, [choose, standard] { choose (standard); });

    context.showMenu (menu, &tagButton);
}

// ── Account ─────────────────────────────────────────────────────────────────────

void TonesPage::onAccountButton()
{
    const auto& user = service.user();

    if (! user)
    {
        if (actions.openSheet)
            actions.openSheet ("Sign in to Soundshed", std::make_unique<SignInContent> (context, actions), { 460, 300 });

        return;
    }

    juce::PopupMenu menu;
    menu.addSectionHeader ("Signed in as " + utf8 (user->Label()));
    menu.addItem ("Sign out", [this] { service.signOut(); });
    context.showMenu (menu, &accountButton);
}

void TonesPage::updateAccountButton()
{
    const auto& user = service.user();
    accountButton.setText (user ? utf8 (user->Label()) : juce::String ("Sign in"));
    resized();
}

void TonesPage::accountChanged()
{
    updateAccountButton();
}

void TonesPage::imagesChanged()
{
    list.updateContent();
}

void TonesPage::downloadsChanged()
{
    list.updateContent();
}

// ── Layout ──────────────────────────────────────────────────────────────────────

void TonesPage::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    const int rowHeight = context.touch ? 44 : 34;

    auto header = area.removeFromTop (rowHeight);
    const int accountWidth = juce::jlimit (90, 180, (int) juce::GlyphArrangement::getStringWidth (
                                                          context.font (NanoTheme::textLabel, FontWeight::medium), accountButton.getText())
                                                          + 36);
    accountButton.setBounds (header.removeFromRight (juce::jmin (accountWidth, header.getWidth() / 3)).reduced (2));

    const int tabWidth = juce::jmin (104, header.getWidth() / 4);

    for (auto* button : { &featuredTab, &presetsTab, &packsTab, &installedTab })
        button->setBounds (header.removeFromLeft (tabWidth).reduced (2));

    area.removeFromTop (6);

    if (tab == Tab::Presets)
    {
        auto filters = area.removeFromTop (rowHeight);
        tagButton.setBounds (filters.removeFromRight (juce::jmin (140, filters.getWidth() / 3)).reduced (2));
        filters.removeFromRight (4);
        search.setBounds (filters.reduced (2));
        area.removeFromTop (6);
    }

    list.setBounds (area);
}
} // namespace soundshed::nano
