#pragma once

// What a screen reader sees of Nano Q: each control of the layer in front (the page, or the
// dialog or menu over it) with its role, name, value and state, as the web UI gives them through
// ARIA. Elements draws everything itself and tells the platform nothing, so this answers for it.
//
// On Windows it is a UI Automation provider for the view's window (AccessBridge.cpp): a flat list
// of the controls Focus.h walks, kept up to date each tick while a client listens, with focus and
// value changes raised as events. A screen reader's Invoke, Toggle, range value and SetFocus are
// queued and carried out on the UI thread at the next tick. Elsewhere Create() returns null:
// macOS (NSAccessibility) and iOS (UIAccessibility) are not done yet.

#include <elements.hpp>

#include <memory>

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
};
} // namespace nanoq::ui
