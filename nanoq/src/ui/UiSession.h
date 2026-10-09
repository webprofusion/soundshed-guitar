#pragma once

// UiSession - one open editor's connection to the engine.
//
// The engine lives on a thread of its own (Engine.h); the view lives on the host's UI thread.
// A session is the bridge: requests the views make go to the engine thread, what the engine
// sends back is collected here on a timer and applied to the UiClient's state, and the views
// that subscribed to what changed redraw. Exactly as in the JUCE product, messages are applied
// between UI events and never inside the call that sent a request.

#include "Engine.h"
#include "ui/Theme.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <elements.hpp>

#include <memory>

namespace nanoq::ui
{
class UiSession
{
public:
    UiSession(Engine& engine, cycfi::elements::view& view);
    ~UiSession();

    UiSession(const UiSession&) = delete;
    UiSession& operator=(const UiSession&) = delete;

    [[nodiscard]] guitarfx::uiclient::UiClient& Client() { return mClient; }
    [[nodiscard]] guitarfx::uiclient::UiCommands& Commands() { return mCommands; }
    [[nodiscard]] const guitarfx::uiclient::ClientState& State() const { return mClient.State(); }
    [[nodiscard]] Theme& GetTheme() { return mTheme; }
    [[nodiscard]] cycfi::elements::view& View() { return mView; }
    [[nodiscard]] Engine& GetEngine() { return mEngine; }

    /// Called after every batch of engine messages has been applied, on the UI thread. The
    /// shell uses it to rebuild what the state changed; views subscribe to topics instead.
    std::function<void()> onTick;

    /// Seconds on a monotonic clock, for leases and animations.
    [[nodiscard]] static double Now();

private:
    void Schedule();
    void Pump();

    Engine& mEngine;
    cycfi::elements::view& mView;
    guitarfx::uiclient::UiClient mClient;
    guitarfx::uiclient::UiCommands mCommands{mClient};
    Theme mTheme;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
};
} // namespace nanoq::ui
