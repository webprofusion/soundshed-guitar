#include "nativeui/views/TransportBar.h"

namespace soundshed::nano
{
namespace
{
constexpr const char* kSelectedClipSetting = "demoAudio.selectedId";
constexpr const char* kRenderRateSetting = "demoAudio.renderSampleRate";
constexpr const char* kRepeatSetting = "nativeUi.demoRepeat";
} // namespace

TransportBar::TransportBar (NanoContext& contextIn, ShellActions& actionsIn)
    : context (contextIn),
      actions (actionsIn),
      clipButton (contextIn, "demo-clip", {}, "Demo"),
      playButton (contextIn, "demo-play", "play"),
      repeatButton (contextIn, "demo-repeat", {}, "Loop"),
      bpmButton (contextIn, "metronome-open", {}, "120"),
      metronomeButton (contextIn, "metronome-toggle", "play"),
      controlsButton (contextIn, "controls-open", "sliders"),
      muteButton (contextIn, "output-mute", "mute")
{
    setComponentID ("transport");

    for (auto* button : { &clipButton, &playButton, &repeatButton, &bpmButton, &metronomeButton, &controlsButton, &muteButton })
    {
        button->setFlat (true);
        button->setClickingTogglesState (false);
        addAndMakeVisible (*button);
    }

    playButton.setTooltip ("Play the demo clip through this preset");
    repeatButton.setTooltip ("Repeat");
    bpmButton.setTooltip ("Metronome");
    metronomeButton.setTooltip ("Start or stop the metronome");
    controlsButton.setTooltip ("Input and output");
    muteButton.setTooltip ("Mute the output");

    clipButton.onClick = [this] { showClipMenu(); };
    playButton.onClick = [this] {
        if (context.state().demo.playing)
        {
            context.commands.StopDemo();
            return;
        }

        if (const auto* clip = selectedClip())
        {
            if (clip->takeId.empty())
                context.commands.PreviewDemoClip (clip->id, repeat);
            else
                context.commands.PreviewRiff (clip->takeId);
        }
    };
    repeatButton.onClick = [this] {
        repeat = ! repeat;
        context.commands.SetSetting (kRepeatSetting, repeat);
        refresh();
    };
    bpmButton.onClick = [this] { if (onOpenMetronome) onOpenMetronome(); };
    metronomeButton.onClick = [this] { context.commands.SetMetronomeEnabled (! context.state().metronome.enabled); };
    controlsButton.onClick = [this] { if (onOpenControls) onOpenControls(); };
    muteButton.onClick = [this] { context.commands.SetOutputMuted (! context.state().outputMuted); };

    demoSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Demo, [this] { refresh(); });
    metronomeSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Metronome, [this] { refresh(); });
    chainSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::GlobalChain, [this] { refresh(); });
    sessionSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Session, [this] {
        repeat = context.state().appSettings.value (kRepeatSetting, false);
        refresh();
    });
    refresh();
}

TransportBar::~TransportBar() = default;

const guitarfx::uiclient::DemoClip* TransportBar::selectedClip() const
{
    const auto& demo = context.state().demo;
    const auto selected = context.state().appSettings.value (kSelectedClipSetting, std::string {});

    for (const auto* list : { &demo.clips, &demo.riffClips })
        for (const auto& clip : *list)
            if (clip.id == selected)
                return &clip;

    return demo.clips.empty() ? nullptr : &demo.clips.front();
}

void TransportBar::showClipMenu()
{
    const auto& demo = context.state().demo;
    const auto* current = selectedClip();
    juce::PopupMenu menu;

    for (const auto* list : { &demo.clips, &demo.riffClips })
    {
        if (list == &demo.riffClips && ! list->empty())
            menu.addSeparator();

        for (const auto& clip : *list)
            menu.addItem (juce::String::fromUTF8 (clip.title.c_str()), true, current != nullptr && current->id == clip.id,
                          [this, id = clip.id] {
                              context.commands.SetSetting (kSelectedClipSetting, id);
                              refresh();
                          });
    }

    menu.addSeparator();
    menu.addItem ("Render this clip through the preset...", current != nullptr, false, [this] {
        if (const auto* clip = selectedClip())
        {
            const int rate = context.state().appSettings.value (kRenderRateSetting, 0);

            if (clip->takeId.empty())
                context.commands.RenderDemoClip (clip->id, rate);
            else
                context.commands.RenderRiffTake (clip->takeId, rate);
        }
    });

    context.showMenu (menu, &clipButton);
}

void TransportBar::refresh()
{
    const auto& state = context.state();
    const auto* clip = selectedClip();
    clipButton.setText (clip != nullptr ? juce::String::fromUTF8 (clip->title.c_str()) : juce::String ("Demo"));

    playButton.setIcon (state.demo.playing ? "stop" : "play");
    playButton.setToggleState (state.demo.playing, juce::dontSendNotification);
    repeatButton.setToggleState (repeat, juce::dontSendNotification);

    const auto bpm = state.metronome.bpm;
    bpmButton.setText (juce::String (bpm, bpm == std::round (bpm) ? 0 : 1) + " BPM");
    metronomeButton.setIcon (state.metronome.enabled ? "stop" : "play");
    metronomeButton.setToggleState (state.metronome.enabled, juce::dontSendNotification);

    muteButton.setToggleState (state.outputMuted, juce::dontSendNotification);
    muteButton.setIconColour (state.outputMuted ? std::optional<juce::Colour> (juce::Colours::white) : std::nullopt);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.setColour (context.theme.bar());
    g.fillRect (getLocalBounds());
    g.setColour (context.theme.border());
    g.fillRect (getLocalBounds().removeFromTop (1));
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (4, 2);
    const int button = juce::jmin (area.getHeight(), context.touch ? 48 : 40);

    muteButton.setBounds (area.removeFromRight (button));
    controlsButton.setBounds (area.removeFromRight (button));
    metronomeButton.setBounds (area.removeFromRight (button));
    bpmButton.setBounds (area.removeFromRight (juce::jmin (100, area.getWidth() / 3)));
    area.removeFromRight (6);

    playButton.setBounds (area.removeFromLeft (button));
    repeatButton.setBounds (area.removeFromLeft (button + 16));
    clipButton.setBounds (area);
}
} // namespace soundshed::nano
