#pragma once

#include "uiclient/ToneSharing.h"
#include "uiclient/UiClient.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace soundshed::nano
{
class NanoContext;

/// Soundshed's tone sharing service, over HTTP, for the Tones page: the requests, the
/// signed-in account, the pack pictures, and downloading a tone to hand to the engine.
///
/// The web UI talks to the same service from TypeScript (core/ui/ts/toneSharingPanel/);
/// guitarfx::uiclient::tones holds the shared contract (paths, answers, what is installed).
/// Requests run on a small pool of worker threads; every callback comes back on the message
/// thread, and none arrives once the service is gone. The session is the web UI's own
/// setting (toneSharing.sessionId), sent as the x-session-id header, so signing in to either
/// product signs in both.
class ToneSharingService
{
public:
    using Response = guitarfx::uiclient::tones::Response;
    using Callback = std::function<void (const Response&)>;

    explicit ToneSharingService (NanoContext& context);
    ~ToneSharingService();

    /// A JSON request; `path` is relative to the API base ("/items?page=1").
    void get (const std::string& path, Callback callback);
    void post (const std::string& path, const nlohmann::json& body, Callback callback);

    /// A picture by absolute URL: the one fetched already, or an invalid image while it is
    /// fetched (listeners hear imagesChanged when it arrives). A failed fetch is not retried.
    [[nodiscard]] juce::Image image (const std::string& url);

    // ── Account ─────────────────────────────────────────────────────────────────
    [[nodiscard]] const std::optional<guitarfx::uiclient::tones::User>& user() const noexcept { return account; }
    [[nodiscard]] bool hasSession() const;

    /// Asks the service who the stored session belongs to (GET /auth/me).
    void refreshAccount();
    /// Emails a sign-in code (POST /auth/start).
    void requestCode (const std::string& email, Callback callback);
    /// Signs in with the emailed code (POST /auth/verify), keeping the session it hands out.
    void verifyCode (const std::string& email, const std::string& code, Callback callback);
    void signOut();

    // ── Installing ──────────────────────────────────────────────────────────────
    /// Downloads a shared preset, or every preset of a pack, and asks the engine to install
    /// it ("installPresetArchives"). Progress: isDownloading(), then the client's toneInstalls.
    void install (const guitarfx::uiclient::tones::Tone& tone);
    [[nodiscard]] bool isDownloading (const std::string& entryId) const { return downloading.count (entryId) > 0; }

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void accountChanged() {}
        virtual void imagesChanged() {}
        virtual void downloadsChanged() {}
    };

    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

private:
    struct HttpResult
    {
        int status = 0;
        std::string body;
        std::string sessionHeader;
    };

    using BytesCallback = std::function<void (const HttpResult&)>;

    /// Runs on a worker; `json` requests retry once without a session the service refused.
    void request (const std::string& method, const std::string& url, const std::string& jsonBody, bool json, BytesCallback done);
    static HttpResult perform (const std::string& method, const std::string& url, const std::string& jsonBody,
                               const std::string& session, const std::atomic<bool>& isAlive);

    void installItem (const guitarfx::uiclient::tones::Tone& tone);
    void installPack (const guitarfx::uiclient::tones::Tone& tone);
    void sendInstall (const nlohmann::json& entry, const std::string& folder, nlohmann::json archives);
    void downloadFailed (const std::string& entryId, const juce::String& title, const juce::String& reason);
    void setDownloading (const std::string& entryId, bool isDownloading);
    void onInstallsChanged();

    [[nodiscard]] std::string sessionId() const;
    void storeSession (const std::string& session);

    NanoContext& context;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    juce::ThreadPool pool { 3 };

    std::optional<guitarfx::uiclient::tones::User> account;
    std::map<std::string, juce::Image> images;
    std::set<std::string> imagesRequested;
    std::set<std::string> downloading;
    std::map<std::string, std::string> awaiting; // entry id -> title, installs sent to the engine
    juce::ListenerList<Listener> listeners;
    guitarfx::uiclient::Subscription installSubscription;

    JUCE_DECLARE_NON_COPYABLE (ToneSharingService)
};
} // namespace soundshed::nano
