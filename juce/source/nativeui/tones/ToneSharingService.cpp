#include "nativeui/tones/ToneSharingService.h"

#include "nativeui/NanoContext.h"

namespace soundshed::nano
{
namespace tones = guitarfx::uiclient::tones;

namespace
{
constexpr int kConnectionTimeoutMs = 15000;

juce::String utf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

/// Every preset of a pack that has no archive of its own, downloaded one by one.
struct PackDownload
{
    std::vector<nlohmann::json> archives; // in the pack's order
    std::size_t remaining = 0;
    bool failed = false;
};
} // namespace

ToneSharingService::ToneSharingService (NanoContext& contextIn) : context (contextIn)
{
    installSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Tones, [this] { onInstallsChanged(); });
}

ToneSharingService::~ToneSharingService()
{
    // Callbacks already queued see this and do nothing; a download in progress stops reading.
    alive->store (false);
    pool.removeAllJobs (true, kConnectionTimeoutMs + 2000);
}

// ── Requests ────────────────────────────────────────────────────────────────────

ToneSharingService::HttpResult ToneSharingService::perform (const std::string& method, const std::string& url,
                                                            const std::string& jsonBody, const std::string& session,
                                                            const std::atomic<bool>& isAlive)
{
    HttpResult result;
    juce::URL target (utf8 (url));
    auto handling = juce::URL::ParameterHandling::inAddress;
    juce::String headers = "Accept: application/json\r\n";

    if (method != "GET")
    {
        // POST, PUT and DELETE all carry a JSON body; the service reads "{}" as no fields.
        target = target.withPOSTData (utf8 (jsonBody.empty() ? std::string ("{}") : jsonBody));
        handling = juce::URL::ParameterHandling::inPostData;
        headers << "Content-Type: application/json\r\n";
    }

    if (! session.empty())
        headers << "x-session-id: " << utf8 (session) << "\r\n";

    juce::StringPairArray responseHeaders;
    const auto common = juce::URL::InputStreamOptions (handling)
                            .withExtraHeaders (headers)
                            .withConnectionTimeoutMs (kConnectionTimeoutMs)
                            .withStatusCode (&result.status)
                            .withResponseHeaders (&responseHeaders)
                            .withNumRedirectsToFollow (5);

    const auto stream = target.createInputStream (method != "GET" ? common.withHttpRequestCmd (utf8 (method)) : common);

    if (stream == nullptr)
        return result;

    // Read in pieces so a download stops as soon as the service is torn down.
    std::vector<char> buffer (64 * 1024);

    while (isAlive.load())
    {
        const auto read = stream->read (buffer.data(), (int) buffer.size());

        if (read <= 0)
            break;

        result.body.append (buffer.data(), (std::size_t) read);
    }

    result.sessionHeader = responseHeaders.getValue ("x-session-id", {}).toStdString();
    return result;
}

void ToneSharingService::request (const std::string& method, const std::string& url, const std::string& jsonBody,
                                  bool json, BytesCallback done)
{
    const auto session = sessionId();
    auto flag = alive;

    pool.addJob ([this, flag, method, url, jsonBody, session, json, done = std::move (done)] {
        auto result = perform (method, url, jsonBody, session, *flag);

        // As the web UI does: a stored session the service no longer knows is dropped, and the
        // request asked again without it.
        const bool retried = json && result.status == 401 && ! session.empty() && flag->load();

        if (retried)
            result = perform (method, url, jsonBody, {}, *flag);

        juce::MessageManager::callAsync ([this, flag, result = std::move (result), retried, done] {
            if (! flag->load())
                return;

            if (retried && result.status != 401)
                storeSession ({});

            done (result);
        });
    });
}

void ToneSharingService::get (const std::string& path, Callback callback)
{
    request ("GET", tones::ResolveUrl (path), {}, true, [this, callback = std::move (callback)] (const HttpResult& result) {
        const auto response = tones::ParseResponse (result.status, result.body, result.sessionHeader);

        if (! response.sessionId.empty())
            storeSession (response.sessionId);

        callback (response);
    });
}

void ToneSharingService::post (const std::string& path, const nlohmann::json& body, Callback callback)
{
    request ("POST", tones::ResolveUrl (path), body.dump(), true, [this, callback = std::move (callback)] (const HttpResult& result) {
        const auto response = tones::ParseResponse (result.status, result.body, result.sessionHeader);

        if (! response.sessionId.empty())
            storeSession (response.sessionId);

        callback (response);
    });
}

juce::Image ToneSharingService::image (const std::string& url)
{
    if (url.empty())
        return {};

    if (const auto it = images.find (url); it != images.end())
        return it->second;

    if (imagesRequested.insert (url).second)
    {
        request ("GET", url, {}, false, [this, url] (const HttpResult& result) {
            juce::Image picture;

            // PNG and JPEG; JUCE reads no WEBP, and such a pack simply shows its icon.
            if (result.status == 200 && ! result.body.empty())
                picture = juce::ImageFileFormat::loadFrom (result.body.data(), result.body.size());

            images[url] = picture;
            listeners.call (&Listener::imagesChanged);
        });
    }

    return {};
}

// ── Account ─────────────────────────────────────────────────────────────────────

std::string ToneSharingService::sessionId() const
{
    const auto& settings = context.state().appSettings;
    const auto it = settings.find (tones::kSessionSettingKey);
    return it != settings.end() && it->is_string() ? it->get<std::string>() : std::string {};
}

bool ToneSharingService::hasSession() const
{
    return ! sessionId().empty();
}

void ToneSharingService::storeSession (const std::string& session)
{
    if (session == sessionId())
        return;

    // The web UI's own setting: a sign-in here is a sign-in there. Null unsets it.
    context.commands.SetSetting (tones::kSessionSettingKey, session.empty() ? nlohmann::json() : nlohmann::json (session));

    if (session.empty() && account.has_value())
    {
        account.reset();
        listeners.call (&Listener::accountChanged);
    }
}

void ToneSharingService::refreshAccount()
{
    if (! hasSession())
    {
        if (account.has_value())
        {
            account.reset();
            listeners.call (&Listener::accountChanged);
        }

        return;
    }

    get ("/auth/me", [this] (const Response& response) {
        account = response.ok ? tones::ParseUser (response.data) : std::nullopt;
        listeners.call (&Listener::accountChanged);
    });
}

void ToneSharingService::requestCode (const std::string& email, Callback callback)
{
    post ("/auth/start", { { "email", email } }, std::move (callback));
}

void ToneSharingService::verifyCode (const std::string& email, const std::string& code, Callback callback)
{
    post ("/auth/verify", { { "email", email }, { "code", code } }, [this, callback = std::move (callback)] (const Response& response) {
        if (response.ok)
        {
            account = tones::ParseUser (response.data);
            listeners.call (&Listener::accountChanged);
        }

        callback (response);
    });
}

void ToneSharingService::signOut()
{
    // Asked with the session still set, then forgotten here whatever the answer.
    post ("/auth/logout", nlohmann::json::object(), [] (const Response&) {});
    storeSession ({});
    account.reset();
    listeners.call (&Listener::accountChanged);
}

// ── Installing ──────────────────────────────────────────────────────────────────

void ToneSharingService::setDownloading (const std::string& entryId, bool isDownloading)
{
    if (isDownloading ? downloading.insert (entryId).second : downloading.erase (entryId) > 0)
        listeners.call (&Listener::downloadsChanged);
}

void ToneSharingService::downloadFailed (const std::string& entryId, const juce::String& title, const juce::String& reason)
{
    setDownloading (entryId, false);

    if (context.showToast)
        context.showToast ("Could not download \"" + title + "\"", reason, true);
}

void ToneSharingService::install (const tones::Tone& tone)
{
    if (tone.id.empty() || isDownloading (tones::EntryIdFor (tone)))
        return;

    if (tone.IsPack())
        installPack (tone);
    else
        installItem (tone);
}

void ToneSharingService::sendInstall (const nlohmann::json& entry, const std::string& folder, nlohmann::json archives)
{
    const auto entryId = entry.value ("id", std::string {});
    setDownloading (entryId, false);
    awaiting[entryId] = entry.value ("title", std::string {});
    context.commands.InstallPresetArchives (entry, folder, std::move (archives));
}

void ToneSharingService::installItem (const tones::Tone& tone)
{
    const auto entryId = tones::ItemEntryId (tone.id);
    setDownloading (entryId, true);

    request ("GET", tones::ResolveUrl (tones::ItemDownloadPath (tone.id)), {}, false, [this, tone, entryId] (const HttpResult& result) {
        if (result.status != 200 || result.body.empty())
        {
            downloadFailed (entryId, utf8 (tone.title), utf8 (tones::ParseResponse (result.status, result.body).error));
            return;
        }

        const auto data = juce::Base64::toBase64 (result.body.data(), result.body.size()).toStdString();
        sendInstall ({ { "id", entryId }, { "title", tone.title }, { "source", "toneSharingApi" } }, {},
                     nlohmann::json::array ({ { { "title", tone.title }, { "data", data } } }));
    });
}

void ToneSharingService::installPack (const tones::Tone& tone)
{
    const auto entryId = tones::PackEntryId (tone.id);
    setDownloading (entryId, true);

    get (tones::PackPath (tone.id), [this, tone, entryId] (const Response& response) {
        const auto detail = response.ok ? tones::ParsePackDetail (response.data) : std::nullopt;

        if (! detail)
        {
            downloadFailed (entryId, utf8 (tone.title), utf8 (response.error));
            return;
        }

        const auto title = detail->pack.title.empty() ? tone.title : detail->pack.title;
        const nlohmann::json entry = { { "id", entryId }, { "title", title }, { "source", "toneSharingApi" }, { "packId", tone.id } };

        request ("GET", tones::ResolveUrl (tones::PackDownloadPath (tone.id)), {}, false,
                 [this, detail = *detail, entry, entryId, title] (const HttpResult& archive) {
                     // A pack with an archive of its own installs from it.
                     if (archive.status == 200 && ! archive.body.empty())
                     {
                         const auto data = juce::Base64::toBase64 (archive.body.data(), archive.body.size()).toStdString();
                         sendInstall (entry, title, nlohmann::json::array ({ { { "title", title }, { "data", data } } }));
                         return;
                     }

                     // Most have none (409): its presets are fetched one by one, as the web UI does.
                     if (archive.status != 409)
                     {
                         downloadFailed (entryId, utf8 (title), utf8 (tones::ParseResponse (archive.status, archive.body).error));
                         return;
                     }

                     if (detail.items.empty())
                     {
                         downloadFailed (entryId, utf8 (title), "This pack has no presets yet.");
                         return;
                     }

                     auto pack = std::make_shared<PackDownload>();
                     pack->archives.resize (detail.items.size());
                     pack->remaining = detail.items.size();

                     for (std::size_t i = 0; i < detail.items.size(); ++i)
                     {
                         const auto& item = detail.items[i];

                         request ("GET", tones::ResolveUrl (tones::ItemDownloadPath (item.itemId)), {}, false,
                                  [this, pack, i, item, entry, entryId, title] (const HttpResult& result) {
                                      if (pack->failed)
                                          return;

                                      if (result.status != 200 || result.body.empty())
                                      {
                                          pack->failed = true;
                                          downloadFailed (entryId, utf8 (title),
                                                          "\"" + utf8 (item.title) + "\": "
                                                              + utf8 (tones::ParseResponse (result.status, result.body).error));
                                          return;
                                      }

                                      pack->archives[i] = { { "title", item.title },
                                                            { "data", juce::Base64::toBase64 (result.body.data(), result.body.size())
                                                                          .toStdString() } };

                                      if (--pack->remaining == 0)
                                          sendInstall (entry, title, nlohmann::json (pack->archives));
                                  });
                     }
                 });
    });
}

void ToneSharingService::onInstallsChanged()
{
    // The engine's answer to an install this service asked for: say so once. A failure is
    // already on screen, as the engine's own error.
    const auto& installs = context.state().toneInstalls;

    for (auto it = awaiting.begin(); it != awaiting.end();)
    {
        const auto install = installs.find (it->first);

        if (install == installs.end() || install->second.status == guitarfx::uiclient::ToneInstall::Status::Installing)
        {
            ++it;
            continue;
        }

        if (install->second.status == guitarfx::uiclient::ToneInstall::Status::Installed && context.showToast)
        {
            const auto count = install->second.presetIds.size();
            context.showToast ("Installed \"" + utf8 (it->second) + "\"",
                               juce::String ((int) count) + (count == 1 ? " preset" : " presets") + " added to your library", false);
        }

        it = awaiting.erase (it);
    }
}
} // namespace soundshed::nano
