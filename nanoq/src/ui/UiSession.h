#pragma once

// UiSession - one open editor's connection to the engine.
//
// The engine lives on a thread of its own (Engine.h); the view lives on the host's UI thread.
// A session is the bridge: requests the views make go to the engine thread, what the engine
// sends back is collected here on a timer and applied to the UiClient's state, and the views
// that subscribed to what changed redraw. Exactly as in the JUCE product, messages are applied
// between UI events and never inside the call that sent a request.

#include "ui/debug/DevDriver.h"
#include "ui/widgets/Theme.h"
#include "ui/UiLink.h"
#include "uiclient/ToneService.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <elements.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nanoq::ui
{
class UiSession
{
  public:
    UiSession(UiLink& link, cycfi::elements::view& view);
    ~UiSession();

    UiSession(const UiSession&) = delete;
    UiSession& operator=(const UiSession&) = delete;

    [[nodiscard]] guitarfx::uiclient::UiClient& Client()
    {
        return mClient;
    }

    [[nodiscard]] guitarfx::uiclient::UiCommands& Commands()
    {
        return mCommands;
    }

    [[nodiscard]] const guitarfx::uiclient::ClientState& State() const
    {
        return mClient.State();
    }

    [[nodiscard]] Theme& GetTheme()
    {
        return mTheme;
    }

    [[nodiscard]] cycfi::elements::view& View()
    {
        return mView;
    }

    /// Redraws an element: by the rectangle it last drew into when it keeps one (Drawn.h, and
    /// nothing before its first draw), else by Elements' search for it, which redraws the whole
    /// window past four in one poll.
    void Repaint(cycfi::elements::element& element);

    /// The engine's host: Nano Q's engine, or another framework's editor (UiLink.h).
    [[nodiscard]] UiLink& Link()
    {
        return mLink;
    }

    /// Tone sharing and Tone3000 over the network (core/src/uiclient/ToneService.h).
    [[nodiscard]] guitarfx::uiclient::ToneService& Tones()
    {
        return *mTones;
    }

    /// Runs `fn` on the UI thread at the next pump. Any thread.
    void PostToUi(std::function<void()> fn);

    /// PostToUi for a thread that may outlive the session: `fn` is then dropped.
    [[nodiscard]] std::function<void(std::function<void()>)> Poster() const;

    /// Shows a transient message; the shell sets it.
    std::function<void(const std::string& title, const std::string& detail, bool isError)> onToast;

    /// The editor's zoom (1 = 100%), kept by the plugin with its state; the presenter sets both.
    std::function<float()> getZoom;
    std::function<void(float)> setZoom;

    /// Called after every batch of engine messages has been applied, on the UI thread. The
    /// shell uses it to rebuild what the state changed; views subscribe to topics instead.
    std::function<void()> onTick;

    /// Seconds on a monotonic clock, for leases and animations.
    [[nodiscard]] static double Now();

  private:
    void Schedule();
    void Pump();

    UiLink& mLink;
    cycfi::elements::view& mView;
    guitarfx::uiclient::UiClient mClient;
    guitarfx::uiclient::UiCommands mCommands{mClient};
    Theme mTheme;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    std::optional<DevDriver> mDriver = DevDriver::FromEnvironment();

    // Work handed to the UI thread from others (the tone service's answers), run at each pump.
    struct PostQueue
    {
        std::mutex mutex;
        std::vector<std::function<void()>> pending;
    };

    std::shared_ptr<PostQueue> mPosted = std::make_shared<PostQueue>();
    // Last: destroyed first, so its workers stop before anything they post to goes.
    std::unique_ptr<guitarfx::uiclient::ToneService> mTones;
};
} // namespace nanoq::ui
