#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/ParamControl.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace soundshed::nano
{
/// The global input and output stage, around every preset: input level and mode, the noise
/// gate, transpose, the global EQ and doubler, the output level, mute and limiter - the
/// controls of the web UI's control bar, as one sheet. Each writes the same global-chain path
/// (setGlobalChainParam) the web UI writes.
class ControlsContent final : public juce::Component
{
public:
    explicit ControlsContent (NanoContext& context);
    ~ControlsContent() override;

    void resized() override;

private:
    struct Knob
    {
        std::unique_ptr<ParamControl> control;
        std::function<double()> read;
    };

    struct Switch
    {
        std::unique_ptr<juce::ToggleButton> button;
        std::function<bool()> read;
    };

    struct Section
    {
        std::unique_ptr<juce::Label> heading;
        std::vector<juce::Component*> switches;
        std::vector<ParamControl*> knobs;
    };

    Section& addSection (const juce::String& title);
    void addKnob (Section& section, const guitarfx::uiclient::EffectParamInfo& info, std::function<double()> read,
                  std::function<void (double)> write);
    void addSwitch (Section& section, const juce::String& text, std::function<bool()> read, std::function<void (bool)> write);
    void update();
    int layoutContent (int width, bool apply);

    NanoContext& context;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<Knob> knobs;
    std::vector<Switch> switches;
    std::vector<std::unique_ptr<Section>> sections;

    guitarfx::uiclient::Subscription chainSubscription;
    guitarfx::uiclient::Subscription sessionSubscription;
};
} // namespace soundshed::nano
