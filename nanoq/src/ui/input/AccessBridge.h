#pragma once

// What a screen reader sees of Nano Q: each control of the layer in front (the page, or the
// dialog or menu over it) with its role, name, value and state, as the web UI gives them through
// ARIA. Elements draws everything itself and tells the platform nothing, so this answers for it.
//
// On Windows it is a UI Automation provider for the view's window (AccessBridge.cpp): the
// controls Focus.h walks, under the dialog or menu they are in and the lists and rows of tabs that
// hold them, kept up to date each tick while a client listens, with focus and value changes raised
// as events. A screen reader's Invoke, Toggle, range value and SetFocus are queued and carried out
// on the UI thread at the next tick. Elsewhere Create() returns null: macOS (NSAccessibility) and
// iOS (UIAccessibility) are not done yet, so UnservedScreenReader() lets the UI offer the standard
// one where a screen reader is running.

#include <elements.hpp>

#include <memory>
#include <string>

namespace nanoq::ui
{
class AccessBridge
{
  public:
    virtual ~AccessBridge() = default;

    /// For the view's window; null where there is no bridge for the platform, or no window yet.
    [[nodiscard]] static std::unique_ptr<AccessBridge> Create(cycfi::elements::view& view);

    /// Carries out what a screen reader asked for, and tells it what changed. UI thread, each tick.
    virtual void Tick() = 0;

    /// Has a screen reader say `text` (the web UI's aria-live region): a toast, a preset or scene
    /// changed from a footswitch, an effect added. `important` ones interrupt (an error, as
    /// aria-live=assertive); others wait their turn and replace one not yet read. Said at the
    /// next tick, if a screen reader is listening; nothing where there is no bridge. UI thread.
    static void Announce(std::string text, bool important = false);

    /// A screen reader is running where there is no bridge for it to read this UI through:
    /// VoiceOver on macOS or iOS, Orca on Linux. Never on Windows, which has the bridge.
    [[nodiscard]] static bool UnservedScreenReader();
};
} // namespace nanoq::ui
