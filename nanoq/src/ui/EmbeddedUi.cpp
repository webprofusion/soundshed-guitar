#include "ui/EmbeddedUi.h"

#include "ui/UiRoot.h"

#include <elements.hpp>

// macOS has its own implementation, in Objective-C++ (EmbeddedUiCocoa.mm).
#if !defined(ELEMENTS_HOST_UI_LIBRARY_COCOA)

#include <cstdint>

#if defined(ELEMENTS_HOST_UI_LIBRARY_X11)
#include <X11/Xlib.h>

#include <algorithm>
#include <vector>

// From Elements' X11 host, which this binary compiles into itself (as QPlug's Linux view does).
namespace cycfi::elements
{
Display* get_display();
void dispatch_event(XEvent& ev);
void poll_views();
} // namespace cycfi::elements
#endif

namespace nanoq::ui
{
namespace
{
#if defined(ELEMENTS_HOST_UI_LIBRARY_X11)
/// The parent's children, to find the one the view makes: Elements keeps its window to itself.
std::vector<::Window> ChildWindows(::Window parent)
{
    std::vector<::Window> out;
    ::Window root = 0;
    ::Window parentOfParent = 0;
    ::Window* children = nullptr;
    unsigned int count = 0;

    if (XQueryTree(cycfi::elements::get_display(), parent, &root, &parentOfParent, &children, &count) != 0)
    {
        out.assign(children, children + count);
    }

    if (children != nullptr)
    {
        XFree(children);
    }

    return out;
}
#endif
} // namespace

struct EmbeddedUi::Impl
{
    // The view first: the UI is dropped before the window it draws in.
    std::unique_ptr<cycfi::elements::view> view;
    std::unique_ptr<UiRoot> root;
#if defined(ELEMENTS_HOST_UI_LIBRARY_X11)
    ::Window window = 0; // the view's own child window
#endif
};

EmbeddedUi::EmbeddedUi(UiLink& link, void* parentWindow) : mImpl(std::make_unique<Impl>())
{
    // Elements makes a view on a window its child, sized to the parent's client area until the
    // host places it.
#if defined(ELEMENTS_HOST_UI_LIBRARY_X11)
    const auto parent = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(parentWindow));
    const auto before = ChildWindows(parent);
    mImpl->view = std::make_unique<cycfi::elements::view>(static_cast<unsigned long>(parent));

    for (const auto child : ChildWindows(parent))
    {
        if (std::find(before.begin(), before.end(), child) == before.end())
        {
            mImpl->window = child;
        }
    }
#else
    mImpl->view = std::make_unique<cycfi::elements::view>(static_cast<cycfi::elements::host_view_handle>(parentWindow));
#endif
    mImpl->root = std::make_unique<UiRoot>(link, *mImpl->view);
}

EmbeddedUi::~EmbeddedUi()
{
    mImpl->root.reset();
    mImpl->view.reset();
}

void EmbeddedUi::Place(int x, int y, int width, int height, bool visible)
{
#if defined(ELEMENTS_HOST_UI_LIBRARY_WIN32)
    const auto hwnd = mImpl->view->host();
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    ShowWindow(hwnd, visible ? SW_SHOWNA : SW_HIDE);
#elif defined(ELEMENTS_HOST_UI_LIBRARY_X11)
    if (mImpl->window == 0)
    {
        return;
    }

    // The view hears of the new size as it would of any resize (ConfigureNotify).
    auto* display = cycfi::elements::get_display();
    XMoveResizeWindow(display, mImpl->window, x, y, static_cast<unsigned int>(std::max(width, 1)),
                      static_cast<unsigned int>(std::max(height, 1)));

    if (visible)
    {
        XMapWindow(display, mImpl->window);
    }
    else
    {
        XUnmapWindow(display, mImpl->window);
    }

    XFlush(display);
#else
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)visible;
#endif
}

int EmbeddedUi::EventFd()
{
#if defined(ELEMENTS_HOST_UI_LIBRARY_X11)
    return ConnectionNumber(cycfi::elements::get_display());
#else
    return -1;
#endif
}

void EmbeddedUi::PumpEvents()
{
#if defined(ELEMENTS_HOST_UI_LIBRARY_X11)
    auto* display = cycfi::elements::get_display();

    while (XPending(display) != 0)
    {
        XEvent event;
        XNextEvent(display, &event);
        cycfi::elements::dispatch_event(event);
    }

    cycfi::elements::poll_views();
#endif
}
} // namespace nanoq::ui

#endif // !ELEMENTS_HOST_UI_LIBRARY_COCOA
