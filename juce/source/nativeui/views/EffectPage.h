#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/widgets/IconButton.h"
#include "nativeui/widgets/LevelMeter.h"
#include "nativeui/widgets/ParamControl.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>
#include <vector>

namespace soundshed::nano
{
/// The selected effect: its name and what it plays, a bypass switch, its presets, a panel
/// that shows what kind of effect it is and how hot it runs, and its controls - built from
/// the effect catalog's parameter list (groups, Main/Advanced, ranges, tapers, labels) as the
/// web UI's node panel builds them.
class EffectPage final : public juce::Component
{
public:
    EffectPage (NanoContext& context, ShellActions& actions);
    ~EffectPage() override;

    /// Knobs in a grid (landscape) or a list of slider rows (portrait).
    void setUseSliderList (bool useSliders);

    void refresh();
    void updateMeters (double nowSeconds);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class Visual;
    class Controls;

    void rebuildControls();
    void syncValues();
    void showPresetMenu();
    [[nodiscard]] const guitarfx::GraphNode* selectedNode() const;

    NanoContext& context;
    ShellActions& actions;
    bool sliderList = false;
    std::string builtForNodeId;
    std::string builtForType;
    bool showAdvanced = false;
    bool visualFits = true; // resized() decides; refresh() must not override it

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::ToggleButton bypassSwitch;
    IconButton presetsButton;
    IconButton menuButton;
    IconButton mainTab;
    IconButton advancedTab;
    std::unique_ptr<Visual> visual;
    juce::Viewport controlsViewport;
    std::unique_ptr<Controls> controls;

    guitarfx::uiclient::Subscription presetSubscription;
    guitarfx::uiclient::Subscription catalogSubscription;
    guitarfx::uiclient::Subscription resourceSubscription;
};
} // namespace soundshed::nano
