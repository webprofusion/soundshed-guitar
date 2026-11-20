#pragma once

// EmbeddedUi - the native UI in a window of its own, made a child of a window another framework
// owns. For a host that can give a native parent window but knows nothing of Elements:
// Soundshed Guitar's JUCE editor (juce/source/NanoQUiEditor.cpp). The host keeps the child
// window where it wants it and Elements does the rest.
//
// Nothing here names Elements, so a host's code never includes it. The parent is the
// platform's own: an HWND on Windows, an NSView on macOS (EmbeddedUiCocoa.mm), an X11 Window
// on Linux. Windows and macOS deliver the UI's events through their own message loop; an X11
// view reads a display connection of its own, which the host has to watch (EventFd,
// PumpEvents).

#include "ui/UiLink.h"

#include <memory>

namespace nanoq::ui
{
class EmbeddedUi
{
  public:
    /// `parentWindow`: the native window to be a child of (an HWND, an NSView*, or an X11
    /// Window carried in the pointer).
    EmbeddedUi(UiLink& link, void* parentWindow);
    ~EmbeddedUi();

    EmbeddedUi(const EmbeddedUi&) = delete;
    EmbeddedUi& operator=(const EmbeddedUi&) = delete;

    /// Places the child window in its parent and shows or hides it, in the parent's units:
    /// physical pixels on Windows and X11, points on macOS. The UI lays itself out again at
    /// the new size.
    void Place(int x, int y, int width, int height, bool visible);

    /// The connection the UI's events arrive on, for a host to watch in its own loop and call
    /// PumpEvents when it can be read: X11's display connection. -1 where the platform's loop
    /// delivers them (Windows, macOS).
    [[nodiscard]] static int EventFd();

    /// Delivers whatever is waiting on EventFd() and runs the views' timers (animation,
    /// deferred redraws). A host with an EventFd() calls it when that can be read and from a
    /// timer of about 60 Hz. Does nothing elsewhere.
    static void PumpEvents();

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace nanoq::ui
