#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/widgets/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
/// The bottom bar: demo audio (a clip, play/stop, repeat, render), the metronome (tempo, start
/// and stop, and the sheet), the input/output controls sheet, and the output mute.
class TransportBar final : public juce::Component
{
public:
    TransportBar (NanoContext& context, ShellActions& actions);
    ~TransportBar() override;

    std::function<void()> onOpenMetronome;
    std::function<void()> onOpenControls;

    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void showClipMenu();
    [[nodiscard]] const guitarfx::uiclient::DemoClip* selectedClip() const;

    NanoContext& context;
    ShellActions& actions;
    IconButton clipButton;
    IconButton playButton;
    IconButton repeatButton;
    IconButton bpmButton;
    IconButton metronomeButton;
    IconButton controlsButton;
    IconButton muteButton;
    bool repeat = false;

    guitarfx::uiclient::Subscription demoSubscription;
    guitarfx::uiclient::Subscription metronomeSubscription;
    guitarfx::uiclient::Subscription chainSubscription;
    guitarfx::uiclient::Subscription sessionSubscription;
};
} // namespace soundshed::nano
