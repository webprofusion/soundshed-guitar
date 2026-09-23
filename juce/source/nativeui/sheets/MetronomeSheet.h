#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/IconButton.h"
#include "uiclient/Instruments.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace soundshed::nano
{
/// The metronome: tempo (steps, a slider and tap tempo), start and stop, the meter, an accent
/// per beat (a tap cycles accent, medium, normal, silent, as the web UI's beat grid does), the
/// subdivision, the click sound and its level. In a plugin the tempo is the host's and only
/// shows. The current beat lights as the engine reports it.
class MetronomeContent final : public juce::Component
{
public:
    explicit MetronomeContent (NanoContext& context);
    ~MetronomeContent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void update();
    void rebuildBeats();
    void showMeterMenu();
    void showSubdivisionMenu();
    void showClickMenu();

    NanoContext& context;
    guitarfx::uiclient::TapTempo tapTempo;

    IconButton slower;
    IconButton faster;
    IconButton tap;
    IconButton startStop;
    juce::Slider bpmSlider;
    IconButton meterButton;
    IconButton subdivisionButton;
    IconButton clickButton;
    juce::Slider volumeSlider;
    std::vector<std::unique_ptr<IconButton>> beats;
    juce::Rectangle<int> bpmArea;

    guitarfx::uiclient::Subscription stateSubscription;
    guitarfx::uiclient::Subscription beatSubscription;
};
} // namespace soundshed::nano
