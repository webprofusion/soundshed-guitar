#pragma once

/**
 * UiClient.h - The native UI's end of the UI↔engine message protocol.
 *
 * The engine talks to every UI the same way: JSON messages out through
 * IPluginHost::SendMessageToUI, JSON requests in through PluginController::HandleUIMessage
 * (core/protocol/ui-messages.json is the list). The WebView UI parses them in TypeScript;
 * Soundshed Guitar Nano parses them here, into ClientState, and views subscribe to the parts
 * they draw.
 *
 * Message thread only. Incoming messages are queued by Enqueue() and applied by
 * DrainPending(), never inside a Send(): the controller answers some requests synchronously,
 * and applying that answer while a view is half way through the handler that sent the
 * request is how a list gets rebuilt under the iterator walking it.
 *
 * Every send names its type as a literal (Send("type", ...)), and every handler is
 * registered as On("type", ...): that is what tools/check-protocol.mjs reads to hold this
 * client to the same protocol list as the WebView UI.
 */

#include "uiclient/ClientState.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace guitarfx::uiclient
{
/// What changed, so a view redraws only for the state it shows.
enum class Topic
{
    Session,        // state arrived: environment, settings, theme
    ActivePreset,   // preset, scene, dirty flag, node params
    PresetLibrary,  // list, folders, favourites, ratings, recents
    Setlists,
    Catalog,        // effect types and effect presets
    Resources,      // the resource library
    GlobalChain,    // input/output stage, gate, EQ, transpose, doubler, mute
    Mixer,
    Telemetry,      // meters and DSP load (20 Hz)
    Tuner,
    Metronome,
    MetronomeBeat,
    Device,
    DeviceLevels,
    Demo,
    Notifications,
    Tones, // tone sharing installs (what is installed is the toneSharing.installedPacks setting: Session)
    Count
};

class UiClient;

/// Unsubscribes when destroyed, so a view that goes away cannot be called back.
class Subscription
{
public:
    Subscription() = default;
    Subscription(UiClient* client, Topic topic, int id);
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription();

    void Reset();

private:
    UiClient* mClient = nullptr;
    Topic mTopic = Topic::Session;
    int mId = 0;
};

class UiClient
{
public:
    using Transport = std::function<void(const std::string& json)>;
    using Handler = std::function<void(const nlohmann::json& message)>;
    using Listener = std::function<void()>;

    explicit UiClient(Transport transport);
    ~UiClient();

    UiClient(const UiClient&) = delete;
    UiClient& operator=(const UiClient&) = delete;

    /// Announces the UI and asks for everything it draws: the full state, the preset
    /// library and its folders, favourites, ratings and recents, setlists, effect presets,
    /// and the meter feed.
    void Start();

    /// Queues one message from the engine.
    void Enqueue(std::string json);

    /// Applies everything queued, then notifies the listeners of each topic that changed
    /// (once per topic, however many messages touched it). Returns how many were applied.
    int DrainPending();

    /// Keeps the leases alive that lapse unless renewed (the spectrum watch, the device
    /// level feed). Call it from the UI's idle tick with a monotonic time in seconds.
    void Tick(double nowSeconds);

    /// Sends one request to the engine. `type` should be a literal at every call site.
    void Send(std::string_view type, nlohmann::json payload = nlohmann::json::object());

    [[nodiscard]] const ClientState& State() const
    {
        return mState;
    }

    [[nodiscard]] ClientState& MutableState()
    {
        return mState;
    }

    [[nodiscard]] Subscription Subscribe(Topic topic, Listener listener);

    /// Marks a topic changed; its listeners run at the end of the current drain, or right
    /// away when called outside one.
    void Notify(Topic topic);

    /// Takes the notifications (errors, confirmations) that arrived since the last call.
    [[nodiscard]] std::vector<Notification> TakeNotifications();

    /// Adds a notification of the UI's own (a refusal it makes before asking the engine).
    void PostNotification(Notification notification);

    // ── Leases ──────────────────────────────────────────────────────────────
    /// The audio device's input level feed (standalone), held while its page is on screen.
    void SetDeviceLevelWatch(bool enabled);

    [[nodiscard]] std::size_t MessagesApplied() const
    {
        return mMessagesApplied;
    }

    /// Time spent parsing and applying the most recent full "state", in milliseconds. The
    /// largest message the engine sends; worth watching on slow devices.
    [[nodiscard]] double LastFullStateApplyMs() const
    {
        return mLastFullStateApplyMs;
    }

private:
    friend class Subscription;

    void On(const char* type, Handler handler);
    void RegisterHandlers();
    void RegisterPresetHandlers();
    void RegisterLibraryHandlers();
    void RegisterLiveHandlers();
    void Apply(const std::string& json);
    void Unsubscribe(Topic topic, int id);

    Transport mTransport;
    ClientState mState;
    std::unordered_map<std::string, Handler> mHandlers;
    std::deque<std::string> mPending;

    struct ListenerEntry
    {
        int id = 0;
        Listener listener;
    };

    std::array<std::vector<ListenerEntry>, static_cast<std::size_t>(Topic::Count)> mListeners;
    std::array<bool, static_cast<std::size_t>(Topic::Count)> mDirty{};
    int mNextListenerId = 1;
    bool mDraining = false;
    std::vector<Notification> mNotifications;
    std::size_t mMessagesApplied = 0;
    double mLastFullStateApplyMs = 0.0;

    bool mDeviceLevelWatch = false;
    double mLastDeviceLevelRenewal = -1.0e9;
    double mNowSeconds = 0.0;

    // Keeps a sld frame's roster (the per-node key list) until the next one replaces it.
    struct RosterEntry
    {
        std::string key;
        std::string nodeId;
    };

    std::vector<RosterEntry> mRoster;
    int mRosterSeq = -1;
    double mLastRosterRequest = -1.0e9;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
};
} // namespace guitarfx::uiclient
