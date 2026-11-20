// EmbeddedUi on macOS (EmbeddedUi.h): the view is an NSView added to the host's. Objective-C++
// for the frame; everything else is as on the other platforms.

#import <Cocoa/Cocoa.h>

#include "ui/EmbeddedUi.h"

#include "ui/UiRoot.h"

#include <elements.hpp>

namespace nanoq::ui
{
struct EmbeddedUi::Impl
{
    // The view first: the UI is dropped before the window it draws in.
    std::unique_ptr<cycfi::elements::view> view;
    std::unique_ptr<UiRoot> root;
};

EmbeddedUi::EmbeddedUi(UiLink& link, void* parentWindow) : mImpl(std::make_unique<Impl>())
{
    // Elements adds its view to the parent, filling it until the host places it.
    mImpl->view = std::make_unique<cycfi::elements::view>(static_cast<cycfi::elements::host_view_handle>(parentWindow));
    mImpl->root = std::make_unique<UiRoot>(link, *mImpl->view);
}

EmbeddedUi::~EmbeddedUi()
{
    mImpl->root.reset();
    mImpl->view.reset();
}

void EmbeddedUi::Place(int x, int y, int width, int height, bool visible)
{
    NSView* view = (__bridge NSView*)mImpl->view->host();
    NSView* parent = view.superview;

    // The host places it, so it no longer follows the parent's size by itself.
    view.autoresizingMask = NSViewNotSizable;

    // In the parent's points, from its top left; an unflipped parent counts from the bottom.
    CGFloat top = y;

    if (parent != nil && !parent.isFlipped)
    {
        top = parent.bounds.size.height - y - height;
    }

    view.frame = NSMakeRect(x, top, width, height);
    view.hidden = !visible;
}

int EmbeddedUi::EventFd()
{
    // Events arrive through the host's own loop.
    return -1;
}

void EmbeddedUi::PumpEvents()
{
}
} // namespace nanoq::ui
