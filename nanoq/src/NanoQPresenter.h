#pragma once

#include "NanoQController.h"

#include <q_plug/presenter.hpp>

#include <memory>

namespace nanoq
{
namespace ui
{
class Shell;
class UiSession;
} // namespace ui

/// The editor. Exists only while the host has one open: attaching builds the engine session
/// and the shell, detaching drops them, and the engine goes on running without a view.
class NanoQPresenter final : public q_plug::presenter
{
public:
    explicit NanoQPresenter(NanoQController& ctl);
    ~NanoQPresenter() override;

protected:
    void on_attach(cycfi::elements::view& view) override;
    void on_detach() override;

private:
    NanoQController& mCtl;
    std::unique_ptr<ui::UiSession> mSession;
    std::unique_ptr<ui::Shell> mShell;
};
} // namespace nanoq
