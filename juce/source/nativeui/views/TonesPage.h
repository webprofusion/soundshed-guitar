#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/tones/ToneSharingService.h"
#include "nativeui/widgets/IconButton.h"
#include "nativeui/widgets/TapList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace soundshed::nano
{
/// Tones: presets and packs the community shares through Soundshed's tone sharing service,
/// and the ones installed from it - Soundshed Guitar's TONES panel for a small screen.
///
/// Featured, Presets (searchable, by tag) and Packs come from the service a page at a time;
/// Installed is the toneSharing.installedPacks setting both products keep. A tap opens a tone
/// to install it or copy its link; an installed one can be loaded or removed. Signing in is
/// optional here (browsing and installing need no account) and shared with Soundshed Guitar.
class TonesPage final : public juce::Component,
                        private ToneSharingService::Listener,
                        private juce::Timer
{
public:
    TonesPage (NanoContext& context, ShellActions& actions);
    ~TonesPage() override;

    /// Called when the page is shown: fetches what the open tab lists, the first time.
    void activate();

    void resized() override;

private:
    enum class Tab
    {
        Featured,
        Presets,
        Packs,
        Installed
    };

    /// One list the service pages through.
    struct Feed
    {
        std::vector<guitarfx::uiclient::tones::Tone> tones;
        std::vector<guitarfx::uiclient::tones::HomeRow> rows; // Featured only
        int page = 0;
        bool more = false;
        bool loading = false;
        bool loaded = false;
        std::string error;
        int generation = 0; // an answer to an older request is dropped
    };

    struct Row
    {
        enum class Kind
        {
            Heading,
            Tone,
            Installed,
            Action, // "Load more", "Try again", "Browse all presets"
            Message
        };

        Kind kind = Kind::Message;
        std::string text;
        guitarfx::uiclient::tones::Tone tone;
        guitarfx::uiclient::tones::InstalledEntry installed;
        std::function<void()> action;
    };

    void setTab (Tab newTab);
    void load (Tab which, bool nextPage);
    void rebuildRows();
    void addFeedRows (Feed& feed, Tab which, const juce::String& emptyText);

    void paintRow (juce::Graphics& g, int row, juce::Rectangle<int> bounds, bool pressed);
    void paintToneRow (juce::Graphics& g, const guitarfx::uiclient::tones::Tone& tone, juce::Rectangle<int> area);
    void paintInstalledRow (juce::Graphics& g, const guitarfx::uiclient::tones::InstalledEntry& entry, juce::Rectangle<int> area);
    void tapRow (int row);
    void longPressRow (int row, juce::Rectangle<int> screenBounds);

    void openTone (const guitarfx::uiclient::tones::Tone& tone);
    void showInstalledMenu (const guitarfx::uiclient::tones::InstalledEntry& entry, juce::Rectangle<int> screenBounds);
    void showTagMenu();
    void onAccountButton();
    void updateAccountButton();

    /// "Installed", "Installing...", or empty, for a tone's row and sheet.
    [[nodiscard]] juce::String statusOf (const guitarfx::uiclient::tones::Tone& tone) const;

    void accountChanged() override;
    void imagesChanged() override;
    void downloadsChanged() override;
    void timerCallback() override;

    Feed& feedFor (Tab which) { return which == Tab::Featured ? featured : (which == Tab::Presets ? presets : packs); }

    NanoContext& context;
    ShellActions& actions;
    ToneSharingService& service;
    Tab tab = Tab::Featured;
    bool activated = false;

    IconButton featuredTab;
    IconButton presetsTab;
    IconButton packsTab;
    IconButton installedTab;
    IconButton accountButton;
    IconButton tagButton;
    juce::TextEditor search;
    TapList list { "tones-list" };
    std::vector<Row> rows;

    Feed featured;
    Feed presets;
    Feed packs;
    std::string query;
    std::string tag;

    guitarfx::uiclient::Subscription sessionSubscription;
    guitarfx::uiclient::Subscription installsSubscription;
};
} // namespace soundshed::nano
