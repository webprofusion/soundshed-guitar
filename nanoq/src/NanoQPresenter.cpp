#include "NanoQPresenter.h"

#include "Log.h"
#include "ui/Shell.h"
#include "ui/UiSession.h"

#include <elements.hpp>

namespace nanoq
{
NanoQPresenter::NanoQPresenter(NanoQController& ctl) : q_plug::presenter(ctl), mCtl(ctl) {}

NanoQPresenter::~NanoQPresenter() = default;

void NanoQPresenter::on_attach(cycfi::elements::view& view)
{
    mSession = std::make_unique<ui::UiSession>(mCtl.engine(), view);
    mShell = std::make_unique<ui::Shell>(*mSession);
    mSession->onTick = [this] {
        if (mShell)
            mShell->OnTick();
    };

    view.content(mShell->Root());

    const auto size = view.size();
    const auto limits = view.limits();
    Log("view attached: size " + std::to_string(size.x) + "x" + std::to_string(size.y) + ", scale " +
        std::to_string(view.scale()) + ", limits min " + std::to_string(limits.min.x) + "x" + std::to_string(limits.min.y) +
        " max " + std::to_string(limits.max.x) + "x" + std::to_string(limits.max.y));
}

void NanoQPresenter::on_detach()
{
    // The shell holds elements the view still draws; drop the callback first, then the
    // shell, then the session (which tells the engine no view is showing).
    if (mSession)
        mSession->onTick = nullptr;
    mShell.reset();
    mSession.reset();
}
} // namespace nanoq
