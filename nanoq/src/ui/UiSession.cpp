#include "ui/UiSession.h"

#include <chrono>

namespace nanoq::ui
{
namespace
{
constexpr auto kPumpInterval = std::chrono::milliseconds(16);
}

UiSession::UiSession(Engine& engine, cycfi::elements::view& view)
    : mEngine(engine), mView(view), mClient([&engine](const std::string& json) { engine.SendRequest(json); })
{
    mEngine.SetViewAttached(true);

    // Tells the engine a UI is showing, which is what turns the meter and diagnostics feeds on.
    mEngine.SendRequest(R"({"type":"uiVisibility","visible":true})");

    mClient.Start();
    Schedule();
}

UiSession::~UiSession()
{
    *mAlive = false;
    mEngine.SendRequest(R"({"type":"uiVisibility","visible":false})");
    mEngine.SetViewAttached(false);
}

double UiSession::Now()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void UiSession::Schedule()
{
    // Elements' timers are one-shot: each tick schedules the next.
    mView.post(kPumpInterval, [this, alive = mAlive] {
        if (!*alive)
            return;
        Pump();
        if (*alive)
            Schedule();
    });
}

void UiSession::Pump()
{
    for (auto& message : mEngine.TakeMessagesForView())
        mClient.Enqueue(std::move(message));

    mClient.Tick(Now());
    mClient.DrainPending();

    if (onTick)
        onTick();
}
} // namespace nanoq::ui
