#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/IconButton.h"
#include "nativeui/widgets/LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
/// The bar that is always on screen: step presets, see which one is playing and whether it
/// has unsaved changes, and reach the tuner. Nothing used mid-song lives behind a menu.
class TopBar final : public juce::Component
{
public:
    explicit TopBar (NanoContext& context);
    ~TopBar() override;

    std::function<void()> onOpenPresets;
    std::function<void()> onOpenTuner;
    std::function<void()> onMoreMenu;

    /// Room left for the scene strip when the shell puts it inside this bar (wide windows).
    void setSceneStripArea (juce::Component* sceneStrip) { embeddedScenes = sceneStrip; resized(); }

    void refresh();
    void updateMeters (double nowSeconds);

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class PresetLabel;

    NanoContext& context;
    IconButton prevButton;
    IconButton nextButton;
    std::unique_ptr<PresetLabel> presetLabel;
    IconButton favouriteButton;
    IconButton rigsChip;
    LevelMeter inputMeter;
    LevelMeter outputMeter;
    IconButton tunerButton;
    IconButton moreButton;
    juce::Component* embeddedScenes = nullptr;

    guitarfx::uiclient::Subscription presetSubscription;
    guitarfx::uiclient::Subscription librarySubscription;
    guitarfx::uiclient::Subscription mixerSubscription;
    guitarfx::uiclient::Subscription tunerSubscription;
};
} // namespace soundshed::nano
