#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/widgets/IconButton.h"
#include "nativeui/widgets/TapList.h"
#include "uiclient/PresetBrowse.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace soundshed::nano
{
/// The preset library ("Rigs") and the setlists.
///
/// Presets: a search, the All / Favourites / Recents views and the user's folders, filtered
/// and ordered as the web UI's library (uiclient::FilterPresets). A tap loads (asking first
/// when there are unsaved changes); a long press offers favourite and delete.
///
/// Setlists: choose the active setlist and step through its slots, as a footswitch does.
class PresetsPage final : public juce::Component
{
public:
    PresetsPage (NanoContext& context, ShellActions& actions);
    ~PresetsPage() override;

    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;

    /// Loads a preset, asking first when the one playing has unsaved changes.
    static void loadWithConfirm (NanoContext& context, ShellActions& actions, const std::string& presetId);

    /// Deletes one of the user's presets after asking. If it is the one playing, the first
    /// preset left in the library is loaded instead, as Soundshed Guitar does.
    static void deleteWithConfirm (NanoContext& context, ShellActions& actions, const std::string& presetId);

private:
    void showFolderMenu();
    void showPresetMenu (const guitarfx::uiclient::PresetSummary& preset, juce::Rectangle<int> screenBounds);
    void paintPresetRow (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed);
    void paintSlotRow (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed);
    void updateChips();
    void setTab (bool setlists);

    NanoContext& context;
    ShellActions& actions;
    bool showingSetlists = false;
    guitarfx::uiclient::PresetQuery query;

    IconButton presetsTab;
    IconButton setlistsTab;
    juce::TextEditor search;
    IconButton allChip;
    IconButton favouritesChip;
    IconButton recentsChip;
    IconButton foldersChip;
    TapList presetList { "preset-list" };
    std::vector<guitarfx::uiclient::PresetSummary> filtered;

    juce::ComboBox setlistPicker;
    TapList slotList { "setlist-slots" };
    IconButton previousSlot;
    IconButton nextSlot;

    guitarfx::uiclient::Subscription librarySubscription;
    guitarfx::uiclient::Subscription presetSubscription;
    guitarfx::uiclient::Subscription setlistSubscription;
};
} // namespace soundshed::nano
