#pragma once

#include "PluginProcessorAdapter.h"
#include "nativeui/theme/NanoLookAndFeel.h"
#include "nativeui/theme/NanoTheme.h"
#include "nativeui/widgets/IconCache.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace soundshed::nano
{
/// What every Nano view needs: the engine client and its commands, the theme, the icons and
/// the effect presentation table. Owned by the editor and outlives every view.
class NanoContext
{
public:
    explicit NanoContext (PluginProcessorAdapter& processor);

    PluginProcessorAdapter& processor;
    guitarfx::uiclient::UiClient client;
    guitarfx::uiclient::UiCommands commands { client };
    guitarfx::uiclient::EffectPresentation presentation;
    NanoTheme theme;
    juce::File uiRoot; // resources/ui
    std::unique_ptr<NanoLookAndFeel> lookAndFeel;
    std::unique_ptr<IconCache> icons;

    /// Whether the primary input is touch (phones, tablets, touch screens): bigger targets.
    bool touch = false;

    [[nodiscard]] const guitarfx::uiclient::ClientState& state() const { return client.State(); }

    [[nodiscard]] juce::Font font (float height, bool bold = false) const { return lookAndFeel->font (height, bold); }

    /// A monotonic clock in seconds, for leases, tap tempo and animations.
    [[nodiscard]] static double now();

    /// Shows a transient message at the bottom of the window (the editor installs this).
    std::function<void (const juce::String& title, const juce::String& detail, bool isError)> showToast;

    /// Shows a popup menu at `target` (or at the mouse) with rows sized for touch. Every Nano
    /// menu goes through here and the last one is kept: JUCE closes a menu as soon as its app
    /// is not in front, so the debug server chooses from this copy (NanoDebugServer "menu").
    void showMenu (juce::PopupMenu menu, juce::Component* target);
    void showMenu (juce::PopupMenu menu, juce::PopupMenu::Options options);
    juce::PopupMenu lastMenu;
};
} // namespace soundshed::nano
