#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

namespace soundshed::nano
{
enum class Page
{
    Chain,    // the whole signal chain, for structural edits
    Effect,   // the selected effect's controls and visual
    Presets,  // the preset library and setlists, opened from the preset's name
    Tones,    // the community's presets and packs (tone sharing)
    Settings, // appearance, audio, engine
    Device    // audio and MIDI devices (standalone), under Settings
};

/// What the views ask of the shell: navigation, selection, sheets and messages. The shell
/// implements it; views hold a reference and never reach for each other.
struct ShellActions
{
    std::function<void (Page)> showPage;
    std::function<Page()> currentPage;

    /// Selects a node; with `openEffect`, also shows its controls.
    std::function<void (const std::string& nodeId, bool openEffect)> selectNode;
    std::function<std::string()> selectedNode;

    /// The effect picker, adding its choice after `afterNodeId`.
    std::function<void (const std::string& afterNodeId)> openEffectPicker;

    /// Bypass, move, add after, remove: the menu a long press on a node opens.
    std::function<void (const std::string& nodeId, juce::Component* target)> openNodeMenu;

    std::function<void (const juce::String& title, std::unique_ptr<juce::Component> content, juce::Point<int> size)> openSheet;
    std::function<void()> closeSheet;

    /// Asks for a line of text (a preset name, a scene title); calls back only on OK.
    std::function<void (const juce::String& title, const juce::String& initial, std::function<void (const juce::String&)> onOk)> promptText;

    /// Asks before something that cannot be undone; calls back only on confirm.
    std::function<void (const juce::String& title, const juce::String& message, const juce::String& confirmText, std::function<void()> onConfirm)> confirm;
};
} // namespace soundshed::nano
