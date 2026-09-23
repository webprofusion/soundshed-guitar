/**
 * UiClient.cpp - Transport, dispatch and change notification for the native UI.
 *
 * The message handlers themselves are grouped by area in UiClientPresetHandlers.cpp,
 * UiClientLibraryHandlers.cpp and UiClientLiveHandlers.cpp.
 */

#include "uiclient/UiClient.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace guitarfx::uiclient
{
namespace
{
/// Leases the engine drops 5 s after the last renewal; renewed every 2 s, as the web UI does.
constexpr double kLeaseRenewalSeconds = 2.0;
} // namespace

// ── ClientState lookups ─────────────────────────────────────────────────────

const EffectParamInfo* EffectTypeInfo::FindParam(const std::string& key) const
{
    for (const auto& param : parameters)
    {
        if (param.key == key)
        {
            return &param;
        }
    }

    return nullptr;
}

const EffectTypeInfo* ClientState::FindEffectType(const std::string& type) const
{
    for (const auto& info : catalog)
    {
        if (info.type == type)
        {
            return &info;
        }
    }

    return nullptr;
}

const PresetSummary* ClientState::FindPresetSummary(const std::string& id) const
{
    for (const auto& summary : presetList)
    {
        if (summary.id == id)
        {
            return &summary;
        }
    }

    return nullptr;
}

const LibraryResource* ClientState::FindLibraryResource(const std::string& type, const std::string& id) const
{
    const auto it = resources.find(type);

    if (it == resources.end())
    {
        return nullptr;
    }

    for (const auto& resource : it->second)
    {
        if (resource.id == id)
        {
            return &resource;
        }
    }

    return nullptr;
}

const PresetScene* ClientState::ActiveScene() const
{
    if (!activePreset)
    {
        return nullptr;
    }

    for (const auto& scene : activePreset->scenes)
    {
        if (scene.id == activeSceneId)
        {
            return &scene;
        }
    }

    return activePreset->scenes.empty() ? nullptr : &activePreset->scenes.front();
}

// ── Subscription ───────────────────────────────────────────────────────────

Subscription::Subscription(UiClient* client, Topic topic, int id) : mClient(client), mTopic(topic), mId(id)
{
}

Subscription::Subscription(Subscription&& other) noexcept
    : mClient(std::exchange(other.mClient, nullptr)), mTopic(other.mTopic), mId(std::exchange(other.mId, 0))
{
}

Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        mClient = std::exchange(other.mClient, nullptr);
        mTopic = other.mTopic;
        mId = std::exchange(other.mId, 0);
    }

    return *this;
}

Subscription::~Subscription()
{
    Reset();
}

void Subscription::Reset()
{
    if (mClient != nullptr && mId != 0)
    {
        mClient->Unsubscribe(mTopic, mId);
    }

    mClient = nullptr;
    mId = 0;
}

// ── UiClient ───────────────────────────────────────────────────────────────

UiClient::UiClient(Transport transport) : mTransport(std::move(transport))
{
    RegisterHandlers();
}

UiClient::~UiClient()
{
    *mAlive = false;
}

void UiClient::On(const char* type, Handler handler)
{
    mHandlers[type] = std::move(handler);
}

void UiClient::RegisterHandlers()
{
    RegisterPresetHandlers();
    RegisterLibraryHandlers();
    RegisterLiveHandlers();
}

void UiClient::Start()
{
    // uiReady queues the full state broadcast (and marks the UI ready for every later one).
    Send("uiReady");
    Send("requestState");
    Send("getPresetList");
    Send("getPresetFolders");
    Send("getPresetFavorites");
    Send("getPresetRatings");
    Send("getPresetRecents");
    Send("getSetlists");
    Send("getEffectPresets");
    Send("getTheme");

    // The level meters and clip LEDs ride on the signal diagnostics feed, which the engine
    // only runs while a UI asks for it.
    Send("setSignalDiagnosticsEnabled", {{"enabled", true}});
}

void UiClient::Send(std::string_view type, nlohmann::json payload)
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["type"] = std::string(type);

    if (mTransport)
    {
        mTransport(payload.dump());
    }
}

void UiClient::Enqueue(std::string json)
{
    mPending.push_back(std::move(json));
}

int UiClient::DrainPending()
{
    if (mDraining)
    {
        return 0;
    }

    mDraining = true;
    int applied = 0;

    // A handler may send, and the engine may answer at once; that answer is queued behind
    // what is already here and applied in this same drain.
    while (!mPending.empty())
    {
        std::string json = std::move(mPending.front());
        mPending.pop_front();
        Apply(json);
        ++applied;
    }

    mDraining = false;

    const auto alive = mAlive;

    for (std::size_t i = 0; i < mDirty.size(); ++i)
    {
        if (!mDirty[i])
        {
            continue;
        }

        mDirty[i] = false;

        // Copied: a listener may subscribe or unsubscribe while being called.
        const auto listeners = mListeners[i];

        for (const auto& entry : listeners)
        {
            if (!*alive)
            {
                return applied;
            }

            if (entry.listener)
            {
                entry.listener();
            }
        }
    }

    return applied;
}

void UiClient::Apply(const std::string& json)
{
    const auto message = nlohmann::json::parse(json, nullptr, false);

    if (!message.is_object())
    {
        return;
    }

    const auto type = message.value("type", std::string{});
    const auto it = mHandlers.find(type);

    if (it == mHandlers.end())
    {
        return;
    }

    try
    {
        it->second(message);
        ++mMessagesApplied;
    }
    catch (const std::exception&)
    {
        // A payload of an unexpected shape costs that one message, never the UI.
    }
}

void UiClient::Tick(double nowSeconds)
{
    mNowSeconds = nowSeconds;

    if (mDeviceLevelWatch && nowSeconds - mLastDeviceLevelRenewal >= kLeaseRenewalSeconds)
    {
        mLastDeviceLevelRenewal = nowSeconds;
        Send("audioDevice", {{"action", "watchLevels"}, {"enabled", true}});
    }
}

void UiClient::SetDeviceLevelWatch(bool enabled)
{
    if (enabled == mDeviceLevelWatch)
    {
        return;
    }

    mDeviceLevelWatch = enabled;

    if (enabled)
    {
        mLastDeviceLevelRenewal = mNowSeconds;
    }

    Send("audioDevice", {{"action", "watchLevels"}, {"enabled", enabled}});
}

Subscription UiClient::Subscribe(Topic topic, Listener listener)
{
    const int id = mNextListenerId++;
    mListeners[static_cast<std::size_t>(topic)].push_back(ListenerEntry{id, std::move(listener)});
    return Subscription(this, topic, id);
}

void UiClient::Unsubscribe(Topic topic, int id)
{
    auto& listeners = mListeners[static_cast<std::size_t>(topic)];
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                   [id](const ListenerEntry& entry) { return entry.id == id; }),
                    listeners.end());
}

void UiClient::Notify(Topic topic)
{
    const auto index = static_cast<std::size_t>(topic);

    if (mDraining)
    {
        mDirty[index] = true;
        return;
    }

    const auto listeners = mListeners[index];
    const auto alive = mAlive;

    for (const auto& entry : listeners)
    {
        if (!*alive)
        {
            return;
        }

        if (entry.listener)
        {
            entry.listener();
        }
    }
}

std::vector<Notification> UiClient::TakeNotifications()
{
    return std::exchange(mNotifications, {});
}

void UiClient::PostNotification(Notification notification)
{
    mNotifications.push_back(std::move(notification));
    Notify(Topic::Notifications);
}
} // namespace guitarfx::uiclient
