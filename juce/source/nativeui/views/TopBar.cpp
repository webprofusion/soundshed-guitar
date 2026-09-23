#include "nativeui/views/TopBar.h"

#include "uiclient/PresetBrowse.h"

namespace soundshed::nano
{
using guitarfx::uiclient::Topic;

/// The preset's name, what it is and which scene is playing; a tap opens the preset browser.
class TopBar::PresetLabel final : public juce::Component
{
public:
    explicit PresetLabel (NanoContext& contextIn) : context (contextIn)
    {
        setComponentID ("preset-name");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle ("Preset");
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        const auto& theme = context.theme;
        const auto& state = context.state();
        auto bounds = getLocalBounds().toFloat().reduced (4.0f, 2.0f);

        if (isMouseOverOrDragging())
        {
            g.setColour (theme.text().withAlpha (0.06f));
            g.fillRoundedRectangle (bounds, (float) NanoTheme::radius);
        }

        const auto name = state.activePreset ? juce::String::fromUTF8 (state.activePreset->name.c_str()) : juce::String ("No preset");
        juce::String subtitle;

        if (state.activePreset)
        {
            subtitle = juce::String::fromUTF8 (state.activePreset->category.c_str());

            if (state.activePreset->scenes.size() > 1)
                if (const auto* scene = state.ActiveScene())
                    subtitle << (subtitle.isEmpty() ? juce::String() : juce::String::fromUTF8 ("  \xc2\xb7  ")) << juce::String::fromUTF8 (scene->title.c_str());
        }

        const bool showSubtitle = subtitle.isNotEmpty() && bounds.getHeight() >= 34.0f;
        auto nameArea = showSubtitle ? bounds.removeFromTop (bounds.getHeight() * 0.58f) : bounds;

        if (state.activePresetDirty)
        {
            // The web UI's unsaved-changes dot.
            const auto dot = nameArea.removeFromRight (12.0f).withSizeKeepingCentre (8.0f, 8.0f);
            g.setColour (theme.accent());
            g.fillEllipse (dot);
        }

        g.setColour (theme.text());
        g.setFont (context.font (juce::jmin (19.0f, nameArea.getHeight() * 0.72f)));
        g.drawFittedText (name, nameArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);

        if (showSubtitle)
        {
            g.setColour (theme.textMuted());
            g.setFont (context.font (juce::jmin (13.0f, bounds.getHeight() * 0.8f)));
            g.drawFittedText (subtitle, bounds.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && onClick)
            onClick();
    }

private:
    NanoContext& context;
};

TopBar::TopBar (NanoContext& contextIn)
    : context (contextIn),
      prevButton (contextIn, "preset-prev", "chevron-left"),
      nextButton (contextIn, "preset-next", "chevron-right"),
      presetLabel (std::make_unique<PresetLabel> (contextIn)),
      favouriteButton (contextIn, "preset-favourite", "heart"),
      rigsChip (contextIn, "rigs-chip", {}, "Rigs"),
      inputMeter (contextIn, "IN"),
      outputMeter (contextIn, "OUT"),
      tunerButton (contextIn, "tuner-button", "note"),
      moreButton (contextIn, "more-button", {}, juce::String::fromUTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"))
{
    for (auto* button : { &prevButton, &nextButton, &favouriteButton, &tunerButton, &moreButton })
    {
        button->setFlat (true);
        addAndMakeVisible (*button);
    }

    prevButton.setTooltip ("Previous preset");
    nextButton.setTooltip ("Next preset");
    favouriteButton.setTooltip ("Favourite");
    tunerButton.setTooltip ("Tuner");
    moreButton.setTooltip ("More");
    tunerButton.setClickingTogglesState (false);

    addAndMakeVisible (*presetLabel);
    addChildComponent (rigsChip);
    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);

    prevButton.onClick = [this] { context.commands.StepPreset (-1); };
    nextButton.onClick = [this] { context.commands.StepPreset (+1); };
    presetLabel->onClick = [this] { if (onOpenPresets) onOpenPresets(); };
    tunerButton.onClick = [this] { if (onOpenTuner) onOpenTuner(); };
    moreButton.onClick = [this] { if (onMoreMenu) onMoreMenu(); };

    favouriteButton.onClick = [this] {
        const auto& state = context.state();

        if (! state.activePresetId.empty())
            context.commands.SetPresetFavorite (state.activePresetId,
                                                ! guitarfx::uiclient::IsPresetFavorite (state, state.activePresetId));
    };

    rigsChip.onClick = [this] {
        // Multi-Rig is built in Soundshed Guitar; here the rigs can be switched between, or
        // the mix reduced to the one being played.
        const auto& state = context.state();
        juce::PopupMenu menu;

        for (const auto& id : state.mixer.activePresetIds)
        {
            const auto it = state.mixer.presetNames.find (id);
            const auto name = juce::String::fromUTF8 ((it != state.mixer.presetNames.end() ? it->second : id).c_str());
            menu.addItem ("Edit " + name, id != state.activePresetId, id == state.activePresetId,
                          [this, id] { context.commands.FocusMixerPreset (id); });
        }

        menu.addSeparator();
        menu.addItem ("Keep only this rig", [this] {
            const auto& current = context.state();

            for (const auto& id : current.mixer.activePresetIds)
                if (id != current.activePresetId)
                    context.commands.RemoveActivePreset (id);
        });

        context.showMenu (menu, &rigsChip);
    };

    presetSubscription = context.client.Subscribe (Topic::ActivePreset, [this] { refresh(); });
    librarySubscription = context.client.Subscribe (Topic::PresetLibrary, [this] { refresh(); });
    mixerSubscription = context.client.Subscribe (Topic::Mixer, [this] { refresh(); });
    tunerSubscription = context.client.Subscribe (Topic::Tuner, [this] { refresh(); });
    refresh();
}

TopBar::~TopBar() = default;

void TopBar::refresh()
{
    const auto& state = context.state();
    const bool favourite = ! state.activePresetId.empty() && guitarfx::uiclient::IsPresetFavorite (state, state.activePresetId);
    favouriteButton.setIcon (favourite ? "heart-filled" : "heart");
    favouriteButton.setIconColour (favourite ? std::optional<juce::Colour> (context.theme.accent()) : std::nullopt);

    const auto rigs = state.mixer.activePresetIds.size();
    rigsChip.setVisible (rigs > 1);
    rigsChip.setText (juce::String ((int) rigs) + " rigs");

    tunerButton.setToggleState (state.tuner.active, juce::dontSendNotification);
    presetLabel->repaint();
    resized();
}

void TopBar::updateMeters (double nowSeconds)
{
    const auto& telemetry = context.state().telemetry;
    inputMeter.setLevel (telemetry.rawInput.peakDb, telemetry.rawInput.clipped, nowSeconds);
    outputMeter.setLevel (telemetry.output.peakDb, telemetry.output.clipped, nowSeconds);
}

void TopBar::resized()
{
    auto area = getLocalBounds().reduced (4, 2);
    const int button = juce::jmin (area.getHeight(), context.touch ? 48 : 40);

    prevButton.setBounds (area.removeFromLeft (button));
    moreButton.setBounds (area.removeFromRight (button));
    tunerButton.setBounds (area.removeFromRight (button));

    // Meters only where they fit without squeezing the name.
    const bool showMeters = area.getWidth() > 420;
    inputMeter.setVisible (showMeters);
    outputMeter.setVisible (showMeters);

    if (showMeters)
    {
        auto meters = area.removeFromRight (juce::jmin (150, area.getWidth() / 4)).reduced (4, 6);
        inputMeter.setBounds (meters.removeFromTop (meters.getHeight() / 2).reduced (0, 1));
        outputMeter.setBounds (meters.reduced (0, 1));
    }

    if (rigsChip.isVisible())
        rigsChip.setBounds (area.removeFromRight (80).reduced (2, 6));

    if (embeddedScenes != nullptr)
        embeddedScenes->setBounds (area.removeFromRight (juce::jmax (0, area.getWidth() - 300)).reduced (0, 4));

    nextButton.setBounds (area.removeFromRight (button));
    favouriteButton.setBounds (area.removeFromRight (button));
    presetLabel->setBounds (area);
}

void TopBar::paint (juce::Graphics& g)
{
    g.setColour (context.theme.bar());
    g.fillRect (getLocalBounds());
    g.setColour (context.theme.border());
    g.fillRect (getLocalBounds().removeFromBottom (1));
}
} // namespace soundshed::nano
