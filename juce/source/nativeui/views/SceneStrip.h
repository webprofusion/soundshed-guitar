#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/widgets/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace soundshed::nano
{
/// The active preset's scenes as tabs: a tap switches scene (engine-owned: selectScene), "+"
/// adds one (a copy of the scene playing), and a long press offers rename and remove. Always
/// on screen on the play pages, since switching scene is a mid-song action.
class SceneStrip final : public juce::Component
{
public:
    SceneStrip (NanoContext& context, ShellActions& actions);
    ~SceneStrip() override;

    void refresh();
    void resized() override;

private:
    void showSceneMenu (const std::string& sceneId, juce::Component* target);

    NanoContext& context;
    ShellActions& actions;
    std::vector<std::unique_ptr<IconButton>> tabs;
    IconButton addButton;
    guitarfx::uiclient::Subscription presetSubscription;
};
} // namespace soundshed::nano
