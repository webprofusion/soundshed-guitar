#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "uiclient/LayoutMode.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace soundshed::nano
{
class ChainPanel;
class DevicePage;
class EffectPage;
class NavBar;
class PresetsPage;
class SceneStrip;
class SettingsPage;
class Sheet;
class ToastOverlay;
class TopBar;
class TransportBar;

/// The frame every page sits in, and the ShellActions the views call.
///
/// Landscape (rail): navigation down the left; the top bar, the scenes, the page and the
/// transport to its right. Portrait (stack): the same bars stacked, with the navigation as a
/// tab bar along the bottom. The chain strip sits above the effect page so the chain stays
/// in view while an effect is edited. One sheet at a time floats over everything.
class NanoShell final : public juce::Component
{
public:
    explicit NanoShell (NanoContext& context);
    ~NanoShell() override;

    void idleTick (double nowSeconds);

    /// The user's UI scale (nativeUi.scale); the editor lays the shell out at 1/scale and
    /// scales it back up, so every view works in logical pixels.
    [[nodiscard]] float uiScale() const noexcept { return scale; }
    std::function<void()> onScaleChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void buildViews();
    void installActions();
    void onSession();
    void onActivePreset();

    void showPage (Page page);
    void selectNode (const std::string& nodeId, bool openEffect);
    void refreshSelection();
    void ensureSelection();

    void openSheet (const juce::String& title, std::unique_ptr<juce::Component> content, juce::Point<int> size);
    void closeSheet();
    void openEffectPicker (const std::string& afterNodeId);
    void openTuner();
    void openMetronome();
    void openControls();

    // NanoShellMenus.cpp
    void showNodeMenu (const std::string& nodeId, juce::Component* target);
    void showMoreMenu();
    void saveActivePreset (bool asNew);
    void moveNode (const std::string& nodeId, int direction);
    [[nodiscard]] std::string moveTarget (const std::string& nodeId, int direction) const;

    NanoContext& context;
    ShellActions actions;
    guitarfx::uiclient::LayoutMetrics metrics;

    std::unique_ptr<TopBar> topBar;
    std::unique_ptr<SceneStrip> sceneStrip;
    std::unique_ptr<ChainPanel> chainStrip;
    std::unique_ptr<ChainPanel> chainPage;
    std::unique_ptr<EffectPage> effectPage;
    std::unique_ptr<PresetsPage> presetsPage;
    std::unique_ptr<SettingsPage> settingsPage;
    std::unique_ptr<DevicePage> devicePage;
    std::unique_ptr<TransportBar> transport;
    std::unique_ptr<NavBar> navBar;
    std::unique_ptr<ToastOverlay> toasts;
    std::unique_ptr<Sheet> sheet;
    std::vector<std::unique_ptr<Sheet>> retiredSheets;

    Page page = Page::Effect;
    bool pageRestored = false;
    std::string selectedNodeId;

    // After adding an effect, the node that appears next is selected and shown.
    std::set<std::string> nodeIdsBeforeAdd;
    double selectAddedUntil = 0.0;

    std::string appliedTheme;
    float scale = 1.0f;
    bool scenesInTopBar = false;
    std::size_t sceneCount = 0;

    guitarfx::uiclient::Subscription sessionSubscription;
    guitarfx::uiclient::Subscription presetSubscription;
    guitarfx::uiclient::Subscription notificationSubscription;

    JUCE_DECLARE_WEAK_REFERENCEABLE (NanoShell)
};
} // namespace soundshed::nano
