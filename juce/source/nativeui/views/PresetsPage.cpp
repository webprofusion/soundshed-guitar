#include "nativeui/views/PresetsPage.h"

namespace soundshed::nano
{
using namespace guitarfx::uiclient;

namespace
{
juce::String utf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

void addFolderItems (juce::PopupMenu& menu, const std::vector<PresetFolder>& folders, const std::string& activeId,
                     std::function<void (const std::string&)> choose, int depth = 0)
{
    for (const auto& folder : folders)
    {
        const auto id = folder.id;
        menu.addItem (juce::String::repeatedString ("   ", depth) + utf8 (folder.name), true, folder.id == activeId,
                      [choose, id] { choose (id); });
        addFolderItems (menu, folder.children, activeId, choose, depth + 1);
    }
}
} // namespace

void PresetsPage::loadWithConfirm (NanoContext& context, ShellActions& actions, const std::string& presetId)
{
    const auto& state = context.state();

    if (state.activePresetDirty && presetId != state.activePresetId && actions.confirm)
    {
        actions.confirm ("Unsaved changes",
                         "\"" + utf8 (state.activePreset ? state.activePreset->name : std::string {})
                             + "\" has changes that are not saved. Load another preset anyway?",
                         "Discard changes", [&context, presetId] { context.commands.LoadPreset (presetId); });
        return;
    }

    context.commands.LoadPreset (presetId);
}

void PresetsPage::deleteWithConfirm (NanoContext& context, ShellActions& actions, const std::string& presetId)
{
    const auto* summary = context.state().FindPresetSummary (presetId);

    if (summary == nullptr || summary->source != "user" || ! actions.confirm)
        return;

    actions.confirm ("Delete preset", "Delete \"" + utf8 (summary->name) + "\"? This cannot be undone.", "Delete", [&context, presetId] {
        const auto& state = context.state();
        const bool wasPlaying = presetId == state.activePresetId;
        std::string next;

        for (const auto& preset : state.presetList)
        {
            if (preset.id != presetId)
            {
                next = preset.id;
                break;
            }
        }

        context.commands.DeletePreset (presetId);

        // Its unsaved changes went with it, so there is nothing to confirm.
        if (wasPlaying && ! next.empty())
            context.commands.LoadPreset (next);
    });
}

PresetsPage::PresetsPage (NanoContext& contextIn, ShellActions& actionsIn)
    : context (contextIn),
      actions (actionsIn),
      presetsTab (contextIn, "presets-tab-library", {}, "Presets"),
      setlistsTab (contextIn, "presets-tab-setlists", {}, "Setlists"),
      allChip (contextIn, "presets-all", {}, "All"),
      favouritesChip (contextIn, "presets-favourites", "heart", "Favourites"),
      recentsChip (contextIn, "presets-recents", {}, "Recent"),
      foldersChip (contextIn, "presets-folders", "folder", "Folders"),
      previousSlot (contextIn, "setlist-prev", "chevron-left", "Previous"),
      nextSlot (contextIn, "setlist-next", "chevron-right", "Next")
{
    setComponentID ("presets-page");

    for (auto* component : std::initializer_list<juce::Component*> { &presetsTab, &setlistsTab, &search, &allChip, &favouritesChip,
                                                                      &recentsChip, &foldersChip, &presetList, &setlistPicker,
                                                                      &slotList, &previousSlot, &nextSlot })
        addAndMakeVisible (component);

    search.setComponentID ("preset-search");
    search.setTextToShowWhenEmpty ("Search presets", context.theme.textMuted());
    search.setFont (context.font (NanoTheme::textBody));
    search.setIndents (12, 0);
    search.setJustification (juce::Justification::centredLeft);
    presetsTab.setFlat (true);
    setlistsTab.setFlat (true);
    search.onTextChange = [this] {
        query.text = search.getText().toStdString();
        refresh();
    };

    presetsTab.onClick = [this] { setTab (false); };
    setlistsTab.onClick = [this] { setTab (true); };
    allChip.onClick = [this] { query.folderId = kPresetFolderAll; refresh(); };
    favouritesChip.onClick = [this] { query.folderId = kPresetFolderFavorites; refresh(); };
    recentsChip.onClick = [this] { query.folderId = kPresetFolderRecents; refresh(); };
    foldersChip.onClick = [this] { showFolderMenu(); };

    const int rowHeight = context.touch ? 56 : 44;
    presetList.setRowHeight (rowHeight);
    presetList.rowCount = [this] { return (int) filtered.size(); };
    presetList.paintRow = [this] (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed) {
        paintPresetRow (g, row, bounds, pressed);
    };
    presetList.rowName = [this] (int row) { return "preset:" + juce::String::fromUTF8 (filtered[(std::size_t) row].name.c_str()); };
    presetList.onTap = [this] (int row) {
        if (row >= 0 && row < (int) filtered.size())
            loadWithConfirm (context, actions, filtered[(std::size_t) row].id);
    };
    presetList.onLongPress = [this] (int row, juce::Rectangle<int> screen) {
        if (row >= 0 && row < (int) filtered.size())
            showPresetMenu (filtered[(std::size_t) row], screen);
    };

    setlistPicker.setComponentID ("setlist-picker");
    setlistPicker.setTextWhenNoChoicesAvailable ("No setlists yet - make them in Soundshed Guitar");
    setlistPicker.onChange = [this] {
        const auto index = setlistPicker.getSelectedItemIndex();
        const auto& setlists = context.state().setlists;

        if (index >= 0 && index < (int) setlists.size() && setlists[(std::size_t) index].id != context.state().activeSetlistId)
            context.commands.SelectSetlist (setlists[(std::size_t) index].id);
    };

    slotList.setRowHeight (rowHeight);
    slotList.rowCount = [this] {
        for (const auto& setlist : context.state().setlists)
            if (setlist.id == context.state().activeSetlistId)
                return (int) setlist.presetIds.size();

        return 0;
    };
    slotList.paintRow = [this] (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed) {
        paintSlotRow (g, row, bounds, pressed);
    };
    slotList.rowName = [] (int row) { return "slot:" + juce::String (row + 1); };
    slotList.onTap = [this] (int row) { context.commands.SetSetlistCursor (row); };

    const auto step = [this] (int delta) {
        const int count = slotList.rowCount();

        if (count > 0)
            context.commands.SetSetlistCursor ((context.state().setlistCursor + delta + count) % count);
    };
    previousSlot.onClick = [step] { step (-1); };
    nextSlot.onClick = [step] { step (+1); };

    librarySubscription = context.client.Subscribe (Topic::PresetLibrary, [this] { refresh(); });
    presetSubscription = context.client.Subscribe (Topic::ActivePreset, [this] { presetList.updateContent(); slotList.updateContent(); });
    setlistSubscription = context.client.Subscribe (Topic::Setlists, [this] { refresh(); });
    setTab (false);
}

PresetsPage::~PresetsPage() = default;

void PresetsPage::setTab (bool setlists)
{
    showingSetlists = setlists;
    presetsTab.setToggleState (! setlists, juce::dontSendNotification);
    setlistsTab.setToggleState (setlists, juce::dontSendNotification);

    for (auto* component : std::initializer_list<juce::Component*> { &search, &allChip, &favouritesChip, &recentsChip, &foldersChip, &presetList })
        component->setVisible (! setlists);

    for (auto* component : std::initializer_list<juce::Component*> { &setlistPicker, &slotList, &previousSlot, &nextSlot })
        component->setVisible (setlists);

    refresh();
}

void PresetsPage::updateChips()
{
    allChip.setToggleState (query.folderId == kPresetFolderAll, juce::dontSendNotification);
    favouritesChip.setToggleState (query.folderId == kPresetFolderFavorites, juce::dontSendNotification);
    recentsChip.setToggleState (query.folderId == kPresetFolderRecents, juce::dontSendNotification);

    const auto* folder = FindPresetFolder (context.state().presetFolders, query.folderId);
    foldersChip.setToggleState (folder != nullptr, juce::dontSendNotification);
    foldersChip.setText (folder != nullptr ? utf8 (folder->name) : juce::String ("Folders"));
    foldersChip.setVisible (! showingSetlists && ! context.state().presetFolders.empty());
}

void PresetsPage::refresh()
{
    const auto& state = context.state();

    // A folder that has gone (deleted in the other app) falls back to All.
    if (query.folderId != kPresetFolderAll && query.folderId != kPresetFolderFavorites && query.folderId != kPresetFolderRecents
        && FindPresetFolder (state.presetFolders, query.folderId) == nullptr)
        query.folderId = kPresetFolderAll;

    filtered = FilterPresets (state, query);
    updateChips();
    presetList.updateContent();

    setlistPicker.clear (juce::dontSendNotification);

    for (std::size_t i = 0; i < state.setlists.size(); ++i)
    {
        const auto& setlist = state.setlists[i];
        juce::String name = utf8 (setlist.name);

        if (setlist.bank)
            name = "Bank " + juce::String (*setlist.bank) + ": " + name;

        setlistPicker.addItem (name, (int) i + 1);

        if (setlist.id == state.activeSetlistId)
            setlistPicker.setSelectedItemIndex ((int) i, juce::dontSendNotification);
    }

    slotList.updateContent();
    resized();
}

void PresetsPage::paintPresetRow (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed)
{
    if (row < 0 || row >= (int) filtered.size())
        return;

    const auto& theme = context.theme;
    const auto& preset = filtered[(std::size_t) row];
    const bool active = preset.id == context.state().activePresetId;
    paintListRowBackground (g, bounds, active, pressed, row + 1 < (int) filtered.size());
    auto area = bounds.reduced (16, 5);

    if (IsPresetFavorite (context.state(), preset.id))
        context.icons->draw (g, "heart-filled", area.removeFromRight (22).toFloat().withSizeKeepingCentre (14.0f, 14.0f), theme.accent());

    g.setColour (theme.text());
    g.setFont (context.font (15.0f, active ? FontWeight::semibold : FontWeight::regular));
    g.drawFittedText (utf8 (preset.name.empty() ? preset.id : preset.name), area.removeFromTop (area.getHeight() * 11 / 20),
                      juce::Justification::bottomLeft, 1);

    g.setColour (theme.textMuted());
    g.setFont (context.font (NanoTheme::textCaption));
    // "Factory" once, when the category already says so.
    const auto category = utf8 (preset.category);
    const bool markFactory = preset.source == "factory" && ! category.equalsIgnoreCase ("factory");
    const auto source = markFactory ? juce::String::fromUTF8 ("  \xc2\xb7  Factory") : juce::String();
    g.drawFittedText (category + source, area, juce::Justification::topLeft, 1);
}

void PresetsPage::paintSlotRow (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed)
{
    const auto& state = context.state();
    const Setlist* active = nullptr;

    for (const auto& setlist : state.setlists)
        if (setlist.id == state.activeSetlistId)
            active = &setlist;

    if (active == nullptr || row >= (int) active->presetIds.size())
        return;

    const auto& theme = context.theme;
    const auto& presetId = active->presetIds[(std::size_t) row];
    const bool atCursor = row == state.setlistCursor;
    paintListRowBackground (g, bounds, atCursor, pressed, row + 1 < (int) active->presetIds.size());
    auto area = bounds.reduced (16, 0);

    g.setColour (theme.textMuted());
    g.setFont (context.font (NanoTheme::textLabel, FontWeight::medium));
    g.drawText (juce::String (row + 1), area.removeFromLeft (32), juce::Justification::centredLeft);

    const auto* summary = state.FindPresetSummary (presetId);
    g.setColour (theme.text());
    g.setFont (context.font (15.0f, atCursor ? FontWeight::semibold : FontWeight::regular));
    g.drawFittedText (summary != nullptr ? utf8 (summary->name) : (presetId.empty() ? juce::String ("(empty)") : utf8 (presetId)),
                      area, juce::Justification::centredLeft, 1);
}

void PresetsPage::paintListRowBackground (juce::Graphics& g, juce::Rectangle<int> bounds, bool current, bool pressed, bool divider)
{
    // The loaded preset (or the setlist's cursor): a lifted row with an accent mark at its
    // edge. Other rows are divided by a hairline, inset to the text.
    const auto& theme = context.theme;
    const auto row = bounds.toFloat().reduced (4.0f, 1.0f);

    if (current || pressed)
    {
        g.setColour (pressed ? theme.pressedFill() : theme.selectedFill());
        g.fillRoundedRectangle (row, (float) NanoTheme::controlRadius);
    }

    if (current)
    {
        g.setColour (theme.accent());
        g.fillRoundedRectangle (row.withWidth (3.0f).reduced (0.0f, 8.0f), 1.5f);
    }
    else if (divider)
    {
        g.setColour (theme.border().withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (row.getX() + 12.0f, (float) bounds.getBottom() - 1.0f, row.getWidth() - 24.0f, 1.0f));
    }
}

void PresetsPage::showFolderMenu()
{
    juce::PopupMenu menu;
    addFolderItems (menu, context.state().presetFolders, query.folderId, [this] (const std::string& id) {
        query.folderId = id;
        refresh();
    });
    context.showMenu (menu, &foldersChip);
}

void PresetsPage::showPresetMenu (const PresetSummary& preset, juce::Rectangle<int> screenBounds)
{
    const auto id = preset.id;
    const auto name = utf8 (preset.name);
    const bool favourite = IsPresetFavorite (context.state(), id);
    juce::PopupMenu menu;
    menu.addSectionHeader (name);
    menu.addItem ("Load", [this, id] { loadWithConfirm (context, actions, id); });
    menu.addItem (favourite ? "Remove from favourites" : "Add to favourites",
                  [this, id, favourite] { context.commands.SetPresetFavorite (id, ! favourite); });

    // Factory presets are part of the app; only the user's own can go.
    menu.addItem ("Delete...", preset.source == "user", false, [this, id] { deleteWithConfirm (context, actions, id); });

    context.showMenu (menu, juce::PopupMenu::Options().withTargetScreenArea (screenBounds).withStandardItemHeight (context.touch ? 40 : 28));
}

void PresetsPage::paint (juce::Graphics& g)
{
    if (showingSetlists && context.state().setlists.empty())
    {
        g.setColour (context.theme.textMuted());
        g.setFont (context.font (14.0f));
        g.drawFittedText ("Setlists you make in Soundshed Guitar appear here, ready to step through.",
                          getLocalBounds().reduced (24).withTrimmedTop (120), juce::Justification::centredTop, 3);
    }
}

void PresetsPage::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    const int rowHeight = context.touch ? 44 : 34;

    auto tabs = area.removeFromTop (rowHeight);
    presetsTab.setBounds (tabs.removeFromLeft (110).reduced (2));
    setlistsTab.setBounds (tabs.removeFromLeft (110).reduced (2));
    area.removeFromTop (6);

    if (showingSetlists)
    {
        setlistPicker.setBounds (area.removeFromTop (rowHeight).reduced (2));
        area.removeFromTop (6);
        auto steps = area.removeFromBottom (context.touch ? 56 : 44);
        previousSlot.setBounds (steps.removeFromLeft (steps.getWidth() / 2).reduced (4));
        nextSlot.setBounds (steps.reduced (4));
        area.removeFromBottom (6);
        slotList.setBounds (area);
        return;
    }

    search.setBounds (area.removeFromTop (rowHeight).reduced (2));
    area.removeFromTop (6);

    auto chips = area.removeFromTop (rowHeight);
    const int chipWidth = juce::jlimit (70, 130, chips.getWidth() / 4);
    allChip.setBounds (chips.removeFromLeft (chipWidth).reduced (2));
    favouritesChip.setBounds (chips.removeFromLeft (chipWidth + 20).reduced (2));
    recentsChip.setBounds (chips.removeFromLeft (chipWidth).reduced (2));

    if (foldersChip.isVisible())
        foldersChip.setBounds (chips.removeFromLeft (chipWidth + 30).reduced (2));

    area.removeFromTop (6);
    presetList.setBounds (area);
}
} // namespace soundshed::nano
