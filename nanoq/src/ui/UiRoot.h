#pragma once

// UiRoot - the native UI on one Elements view: the session that links it to the engine and the
// shell that draws it. Whatever gives the view (QPlug's presenter, EmbeddedUi for a framework
// that only gives a window) makes one of these and keeps it for as long as the view is shown.

#include "ui/UiLink.h"

#include <elements.hpp>

#include <functional>
#include <memory>

namespace nanoq::ui
{
class Shell;
class UiSession;

class UiRoot
{
  public:
    struct Options
    {
        /// The editor's zoom (1 = 100%), where the host keeps one with its state. Without them
        /// the zoom is the view's own scale. Either way the UI stores it in uiSettings.native.
        std::function<float()> getZoom;
        std::function<void(float)> setZoom;
    };

    UiRoot(UiLink& link, cycfi::elements::view& view, Options options = {});
    ~UiRoot();

    UiRoot(const UiRoot&) = delete;
    UiRoot& operator=(const UiRoot&) = delete;

  private:
    std::unique_ptr<UiSession> mSession;
    std::unique_ptr<Shell> mShell;
};
} // namespace nanoq::ui
