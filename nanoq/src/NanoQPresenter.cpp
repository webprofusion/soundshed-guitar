#include "NanoQPresenter.h"

#include "Log.h"
#include "WindowChrome.h"
#include "ui/UiRoot.h"

#include <elements.hpp>

#include <algorithm>

namespace nanoq
{
NanoQPresenter::NanoQPresenter(NanoQController& ctl) : q_plug::presenter(ctl), mCtl(ctl)
{
}

NanoQPresenter::~NanoQPresenter() = default;

void NanoQPresenter::on_attach(cycfi::elements::view& view)
{
    ui::UiRoot::Options options;
    options.getZoom = [this] { return zoom(); };
    // presenter::zoom snaps to its 0.1 grid, which would move the web UI's 75%, 125%, 135% and
    // 175% to another stop: the scale goes to the controller (it rides in the state, and sizes
    // the next window) and the view as it is.
    options.setZoom = [this](float scale) {
        scale = std::clamp(scale, zoom_min, zoom_max);

        if (scale == zoom())
        {
            return;
        }

        ctrl().view_scale(scale);

        if (auto* v = this->view())
        {
            v->scale(scale);
        }
    };
    mRoot = std::make_unique<ui::UiRoot>(mCtl.engine(), view, std::move(options));
    MakeStandaloneWindowResizable(static_cast<void*>(view.host()));

    const auto size = view.size();
    const auto limits = view.limits();
    Log("view attached: size " + std::to_string(size.x) + "x" + std::to_string(size.y) + ", scale " +
        std::to_string(view.scale()) + ", limits min " + std::to_string(limits.min.x) + "x" +
        std::to_string(limits.min.y) + " max " + std::to_string(limits.max.x) + "x" + std::to_string(limits.max.y));
}

void NanoQPresenter::on_detach()
{
    mRoot.reset();
}
} // namespace nanoq
