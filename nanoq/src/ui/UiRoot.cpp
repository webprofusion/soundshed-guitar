#include "ui/UiRoot.h"

#include "ui/shell/Shell.h"
#include "ui/UiSession.h"

namespace nanoq::ui
{
UiRoot::UiRoot(UiLink& link, cycfi::elements::view& view, Options options)
    : mSession(std::make_unique<UiSession>(link, view))
{
    mSession->getZoom = std::move(options.getZoom);
    mSession->setZoom = std::move(options.setZoom);

    // A host that keeps no zoom of its own (EmbeddedUi, in Soundshed Guitar's window): the view's
    // scale, inside the window it is given, as the web UI's zoom is.
    if (!mSession->getZoom || !mSession->setZoom)
    {
        mSession->getZoom = [&view] { return view.scale(); };
        mSession->setZoom = [&view](float scale) { view.scale(scale); };
    }

    mShell = std::make_unique<Shell>(*mSession);
    mSession->onTick = [this] {
        if (mShell)
        {
            mShell->OnTick();
        }
    };

    view.content(mShell->Root());
}

UiRoot::~UiRoot()
{
    // The shell holds elements the view still draws; drop the callbacks first, then the
    // shell, then the session (which tells the engine no view is showing).
    mSession->onTick = nullptr;
    mSession->getZoom = nullptr;
    mSession->setZoom = nullptr;
    mShell.reset();
    mSession.reset();
}
} // namespace nanoq::ui
