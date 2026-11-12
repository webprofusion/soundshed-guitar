#include "uiclient/ToneService.h"

#include "controller/internal/PresetArchiveSupport.h"
#include "util/Base64.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace guitarfx::uiclient
{
namespace
{
constexpr int kConnectionTimeoutMs = 15000;
constexpr int kDownloadTimeoutMs = 60000; // a model can be tens of megabytes

std::string Base64(const std::string& bytes)
{
    return util::EncodeBase64(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

bool IsZip(const std::string& bytes)
{
    return bytes.size() > 4 && bytes[0] == 'P' && bytes[1] == 'K';
}

/// The model or IR file in a zip a provider serves: its first .nam/.json, or .wav/.ir.
std::optional<std::pair<std::string, std::string>> UnzipModel(const std::string& bytes, const std::string& resourceType)
{
    static constexpr std::array<std::string_view, 2> kNam{".nam", ".json"};
    static constexpr std::array<std::string_view, 2> kIr{".wav", ".ir"};
    const std::vector<std::uint8_t> zip(bytes.begin(), bytes.end());
    auto entry = controller_detail::ExtractFirstZipEntryWithExtension(
        zip, resourceType == "ir" ? std::span<const std::string_view>(kIr) : std::span<const std::string_view>(kNam));

    if (!entry)
    {
        return std::nullopt;
    }

    return std::make_pair(entry->name, std::string(entry->bytes.begin(), entry->bytes.end()));
}

/// Every preset of a pack that has no archive of its own, downloaded one by one.
struct PackDownload
{
    std::vector<ToneService::Download> archives; // in the pack's order
    std::size_t remaining = 0;
    bool failed = false;
};
} // namespace

ToneService::ToneService(UiClient& client, UiCommands& commands, Host host,
                         std::shared_ptr<net::HttpTransport> transport)
    : mClient(client), mCommands(commands), mHost(std::move(host)),
      mWorkers(std::make_unique<net::HttpWorkers>(std::move(transport), 3))
{
    mInstallSubscription = mClient.Subscribe(Topic::Tones, [this] { OnInstallsChanged(); });
    mResourceSubscription = mClient.Subscribe(Topic::Resources, [this] { OnResourcesChanged(); });
    mActivePresetSubscription = mClient.Subscribe(Topic::ActivePreset, [this] { OnActivePresetChanged(); });
    mCommands.onPresetFavoriteSet = [this](const std::string& presetId, bool favorite) {
        SyncFavorite(presetId, favorite);
    };
    mCommands.onPresetRatingSet = [this](const std::string& presetId, int rating) { SyncRating(presetId, rating); };
}

ToneService::~ToneService()
{
    // Callbacks already posted see this and do nothing; the workers, destroyed next, cut short
    // whatever is still in flight rather than waiting for the network to time out.
    mAlive->store(false);
    mCommands.onPresetFavoriteSet = nullptr;
    mCommands.onPresetRatingSet = nullptr;
    mWorkers.reset();
}

int ToneService::AddListener(std::function<void()> listener)
{
    mListeners[mNextListener] = std::move(listener);
    return mNextListener++;
}

void ToneService::RemoveListener(int id)
{
    mListeners.erase(id);
}

void ToneService::Changed()
{
    const auto listeners = mListeners; // a listener may remove itself

    for (const auto& [id, listener] : listeners)
    {
        if (listener)
        {
            listener();
        }
    }
}

// ── Requests ────────────────────────────────────────────────────────────────────

void ToneService::Request(const std::string& method, const std::string& url, const std::string& jsonBody, bool json,
                          BytesCallback done)
{
    const auto session = json ? SessionId() : std::string();
    auto alive = mAlive;
    auto* workers = mWorkers.get();
    auto post = mHost.post;

    mWorkers->Run([alive, workers, post, method, url, jsonBody, session, json, done = std::move(done)] {
        const auto perform = [&](const std::string& withSession) {
            net::HttpRequest request;
            request.method = method;
            request.url = url;
            request.timeoutMs = json ? kConnectionTimeoutMs : kDownloadTimeoutMs;

            if (json)
            {
                request.headers.emplace_back("Accept", "application/json");
            }

            if (method != "GET")
            {
                // POST, PUT and DELETE all carry a JSON body; the service reads "{}" as no fields.
                request.body = jsonBody.empty() ? std::string("{}") : jsonBody;
                request.headers.emplace_back("Content-Type", "application/json");
            }

            if (!withSession.empty())
            {
                request.headers.emplace_back("x-session-id", withSession);
            }

            return workers->Perform(request);
        };

        auto response = perform(session);
        // As the web UI does: a stored session the service no longer knows is dropped, and the
        // request asked again without it.
        const bool retried = json && response.status == 401 && !session.empty() && !response.cancelled;

        if (retried)
        {
            response = perform({});
        }

        if (response.cancelled || !alive->load())
        {
            return;
        }

        HttpResult result{response.status, std::move(response.body), response.Header("x-session-id"), response.error,
                          retried && response.status != 401};
        post([alive, result = std::move(result), done] {
            if (alive->load())
            {
                done(result);
            }
        });
    });
}

void ToneService::CheckForUpdate(const std::string& currentVersion, const std::string& os, const std::string& cpu,
                                 bool standalone, const std::string& instanceId,
                                 std::function<void(const std::optional<AvailableUpdate>&)> callback)
{
    const nlohmann::json body = {{"current_version", currentVersion},
                                 {"os", os},
                                 {"cpu", cpu},
                                 {"is_standalone", standalone},
                                 {"instance_id", instanceId}};
    // json=false: no session header. The body still goes as JSON (every POST carries one).
    Request("POST", tones::ResolveUrl("/app/updatecheck"), body.dump(), false,
            [currentVersion, callback = std::move(callback)](const HttpResult& result) {
                const auto response = tones::ParseResponse(result.status, result.body);

                if (!response.ok || !response.data.is_object())
                {
                    callback(std::nullopt);
                    return;
                }

                const auto& data = response.data;
                const auto latest = data.value("latest_version", std::string());

                if (!IsVersionNewer(latest, currentVersion))
                {
                    callback(std::nullopt);
                    return;
                }

                AvailableUpdate update;
                update.version = latest;
                const auto url = data.value("download_url", std::string());
                update.downloadUrl =
                    url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 ? url : std::string();
                update.releaseNotes = data.value("release_notes", std::string());
                callback(update);
            });
}

void ToneService::SearchBackingTracks(const std::string& normalizedQuery, const std::string& apiKey,
                                      BackingTracksCallback callback)
{
    // json=false: no session header, which is the Soundshed service's, not Google's.
    Request("GET", jam::SearchUrl(normalizedQuery, apiKey), {}, false,
            [callback = std::move(callback)](const HttpResult& result) {
                const auto payload = nlohmann::json::parse(result.body, nullptr, false);

                if (result.status < 200 || result.status >= 300)
                {
                    auto error = result.status == 0 ? result.error : jam::DescribeError(payload);
                    callback({}, error.empty() ? std::string("Request failed.") : error);
                    return;
                }

                callback(jam::ParseSearchResults(payload), {});
            });
}

void ToneService::Get(const std::string& path, Callback callback)
{
    Request("GET", tones::ResolveUrl(path), {}, true, [this, callback = std::move(callback)](const HttpResult& result) {
        auto response = tones::ParseResponse(result.status, result.body, result.sessionHeader);

        if (result.status == 0 && !result.error.empty())
        {
            response.error = result.error;
        }

        if (result.droppedSession)
        {
            StoreSession({});
        }

        if (!response.sessionId.empty())
        {
            StoreSession(response.sessionId);
        }

        callback(response);
    });
}

void ToneService::Post(const std::string& path, const nlohmann::json& body, Callback callback)
{
    Request("POST", tones::ResolveUrl(path), body.dump(), true,
            [this, callback = std::move(callback)](const HttpResult& result) {
                auto response = tones::ParseResponse(result.status, result.body, result.sessionHeader);

                if (result.status == 0 && !result.error.empty())
                {
                    response.error = result.error;
                }

                if (result.droppedSession)
                {
                    StoreSession({});
                }

                if (!response.sessionId.empty())
                {
                    StoreSession(response.sessionId);
                }

                callback(response);
            });
}

const std::string* ToneService::Image(const std::string& url)
{
    if (url.empty())
    {
        return nullptr;
    }

    if (const auto it = mImages.find(url); it != mImages.end())
    {
        return &it->second;
    }

    if (mImagesRequested.insert(url).second)
    {
        Request("GET", url, {}, false, [this, url](const HttpResult& result) {
            mImages[url] = result.status == 200 ? result.body : std::string();
            Changed();
        });
    }

    return nullptr;
}

// ── Account ─────────────────────────────────────────────────────────────────────

std::string ToneService::SessionId() const
{
    const auto& settings = mClient.State().appSettings;
    const auto it = settings.find(tones::kSessionSettingKey);
    return it != settings.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool ToneService::HasSession() const
{
    return !SessionId().empty();
}

void ToneService::StoreSession(const std::string& session)
{
    if (session == SessionId())
    {
        return;
    }

    // The web UI's own setting: a sign-in here is a sign-in there. Null unsets it.
    mCommands.SetSetting(tones::kSessionSettingKey, session.empty() ? nlohmann::json() : nlohmann::json(session));

    if (session.empty() && mAccount)
    {
        mAccount.reset();
        Changed();
    }
}

void ToneService::RefreshAccount()
{
    if (!HasSession())
    {
        if (mAccount)
        {
            mAccount.reset();
            Changed();
        }

        return;
    }

    Get("/auth/me", [this](const Response& response) {
        mAccount = response.ok ? tones::ParseUser(response.data) : std::nullopt;
        Changed();
    });
}

void ToneService::RequestCode(const std::string& email, Callback callback)
{
    Post("/auth/start", {{"email", email}}, std::move(callback));
}

void ToneService::VerifyCode(const std::string& email, const std::string& code, Callback callback)
{
    Post("/auth/verify", {{"email", email}, {"code", code}},
         [this, callback = std::move(callback)](const Response& response) {
             if (response.ok)
             {
                 mAccount = tones::ParseUser(response.data);
                 Changed();
             }

             callback(response);
         });
}

void ToneService::SignOut()
{
    // Asked with the session still set, then forgotten here whatever the answer.
    Post("/auth/logout", nlohmann::json::object(), [](const Response&) {});
    StoreSession({});
    mAccount.reset();
    Changed();
}

// ── Installing ──────────────────────────────────────────────────────────────────

void ToneService::SetDownloading(const std::string& entryId, bool downloading)
{
    if (downloading ? mDownloading.insert(entryId).second : mDownloading.erase(entryId) > 0)
    {
        Changed();
    }
}

void ToneService::DownloadFailed(const std::string& entryId, const std::string& title, const std::string& reason)
{
    SetDownloading(entryId, false);

    if (mHost.toast)
    {
        mHost.toast("Could not download \"" + title + "\"", reason, true);
    }
}

void ToneService::Install(const tones::Tone& tone)
{
    if (tone.id.empty() || IsDownloading(tones::EntryIdFor(tone)))
    {
        return;
    }

    if (tone.IsPack())
    {
        InstallPack(tone);
    }
    else
    {
        InstallItem(tone);
    }
}

void ToneService::Remove(const std::string& entryId)
{
    mCommands.DeleteInstalledPresetArchive(entryId);
}

void ToneService::InstallItem(const tones::Tone& tone)
{
    const auto entryId = tones::ItemEntryId(tone.id);
    SetDownloading(entryId, true);
    Request("GET", tones::ResolveUrl(tones::ItemDownloadPath(tone.id)), {}, false,
            [this, tone, entryId](const HttpResult& result) {
                if (result.status != 200 || result.body.empty())
                {
                    DownloadFailed(entryId, tone.title,
                                   result.status == 0 ? result.error
                                                      : tones::ParseResponse(result.status, result.body).error);
                    return;
                }

                ResolveAndInstall({{"id", entryId}, {"title", tone.title}, {"source", "toneSharingApi"}}, {},
                                  {{tone.title, result.body, tone.id}});
            });
}

void ToneService::InstallPack(const tones::Tone& tone)
{
    const auto entryId = tones::PackEntryId(tone.id);
    SetDownloading(entryId, true);

    Get(tones::PackPath(tone.id), [this, tone, entryId](const Response& response) {
        const auto detail = response.ok ? tones::ParsePackDetail(response.data) : std::nullopt;

        if (!detail)
        {
            DownloadFailed(entryId, tone.title, response.error);
            return;
        }

        const auto title = detail->pack.title.empty() ? tone.title : detail->pack.title;
        const nlohmann::json entry = {
            {"id", entryId}, {"title", title}, {"source", "toneSharingApi"}, {"packId", tone.id}};

        Request("GET", tones::ResolveUrl(tones::PackDownloadPath(tone.id)), {}, false,
                [this, detail = *detail, entry, entryId, title](const HttpResult& archive) {
                    // A pack with an archive of its own installs from it.
                    if (archive.status == 200 && !archive.body.empty())
                    {
                        ResolveAndInstall(entry, title, {{title, archive.body, {}}});
                        return;
                    }

                    // Most have none (409): its presets are fetched one by one, as the web UI does.
                    if (archive.status != 409)
                    {
                        DownloadFailed(entryId, title,
                                       archive.status == 0 ? archive.error
                                                           : tones::ParseResponse(archive.status, archive.body).error);
                        return;
                    }

                    if (detail.items.empty())
                    {
                        DownloadFailed(entryId, title, "This pack has no presets yet.");
                        return;
                    }

                    auto pack = std::make_shared<PackDownload>();
                    pack->archives.resize(detail.items.size());
                    pack->remaining = detail.items.size();

                    for (std::size_t i = 0; i < detail.items.size(); ++i)
                    {
                        const auto item = detail.items[i];
                        Request("GET", tones::ResolveUrl(tones::ItemDownloadPath(item.itemId)), {}, false,
                                [this, pack, i, item, entry, entryId, title](const HttpResult& result) {
                                    if (pack->failed)
                                    {
                                        return;
                                    }

                                    if (result.status != 200 || result.body.empty())
                                    {
                                        pack->failed = true;
                                        DownloadFailed(
                                            entryId, title,
                                            "\"" + item.title + "\": " +
                                                (result.status == 0
                                                     ? result.error
                                                     : tones::ParseResponse(result.status, result.body).error));
                                        return;
                                    }

                                    pack->archives[i] = {item.title, result.body, item.itemId};

                                    if (--pack->remaining == 0)
                                    {
                                        ResolveAndInstall(entry, title, pack->archives);
                                    }
                                });
                    }
                });
    });
}

void ToneService::ResolveAndInstall(const nlohmann::json& entry, const std::string& folder,
                                    std::vector<Download> archives)
{
    // The models the archives name and the library lacks, once each.
    std::vector<tone3000::RemoteRef> missing;
    std::set<std::string> seen;

    for (const auto& archive : archives)
    {
        for (auto& ref : tone3000::MissingRemoteRefs(mClient.State(), archive.bytes))
        {
            if (ref.provider == "tone3000" && seen.insert(ref.id).second)
            {
                missing.push_back(std::move(ref));
            }
        }
    }

    if (missing.empty())
    {
        SendInstall(entry, folder, archives);
        return;
    }

    // Each fetched and imported; the install goes once every one has answered. One that cannot
    // be had leaves the engine to refuse the install, saying which.
    struct Pending
    {
        std::size_t remaining = 0;
        std::vector<std::string> failed;
    };

    auto pending = std::make_shared<Pending>();
    pending->remaining = missing.size();
    auto shared = std::make_shared<std::vector<Download>>(std::move(archives));
    const auto title = entry.value("title", std::string());

    const auto finishOne = [this, pending, shared, entry, folder, title](const std::string& name,
                                                                         const std::string& error) {
        if (!error.empty())
        {
            pending->failed.push_back(name + " (" + error + ")");
        }

        if (--pending->remaining > 0)
        {
            return;
        }

        if (!pending->failed.empty() && mHost.toast)
        {
            std::string list;

            for (const auto& f : pending->failed)
            {
                list += (list.empty() ? "" : ", ") + f;
            }

            mHost.toast("Some Tone3000 models could not be fetched", list, true);
        }

        SendInstall(entry, folder, *shared);
    };

    for (const auto& ref : missing)
    {
        FetchRemoteRef(ref, [this, ref, finishOne](const std::string& bytes, const std::string& error) {
            const auto name = ref.name.empty() ? ref.id : ref.name;

            if (!error.empty())
            {
                finishOne(name, error);
                return;
            }

            std::string file = bytes;

            if (IsZip(bytes))
            {
                const auto entry = UnzipModel(bytes, ref.type);

                if (!entry)
                {
                    finishOne(name, "no model in the download");
                    return;
                }

                file = entry->second;
            }

            ImportAndWait(tone3000::ImportPayloadForRef(ref, Base64(file)),
                          [finishOne, name](const std::string&, const std::string& importError) {
                              finishOne(name, importError);
                          });
        });
    }
}

void ToneService::SendInstall(const nlohmann::json& entry, const std::string& folder,
                              const std::vector<Download>& archives)
{
    const auto entryId = entry.value("id", std::string());
    SetDownloading(entryId, false);
    mAwaiting[entryId] = entry.value("title", std::string());
    auto list = nlohmann::json::array();

    for (const auto& archive : archives)
    {
        nlohmann::json item = {{"title", archive.title}, {"data", Base64(archive.bytes)}};

        if (!archive.itemId.empty())
        {
            item["itemId"] = archive.itemId;
        }

        list.push_back(std::move(item));
    }

    mCommands.InstallPresetArchives(entry, folder, std::move(list));
}

void ToneService::OnInstallsChanged()
{
    // The engine's answer to an install this service asked for: said once. A failure is on
    // screen already, as the engine's own error.
    const auto& installs = mClient.State().toneInstalls;

    for (auto it = mAwaiting.begin(); it != mAwaiting.end();)
    {
        const auto install = installs.find(it->first);

        if (install == installs.end() || install->second.status == ToneInstall::Status::Installing)
        {
            ++it;
            continue;
        }

        if (install->second.status == ToneInstall::Status::Installed && mHost.toast)
        {
            const auto count = install->second.presetIds.size();
            mHost.toast("Installed \"" + it->second + "\"",
                        std::to_string(count) + (count == 1 ? " preset" : " presets") + " added to your library",
                        false);
        }

        it = mAwaiting.erase(it);
    }

    Changed();
}

// ── Tone3000 ────────────────────────────────────────────────────────────────────

void ToneService::SearchTone3000(const std::string& query, const std::string& gear, const std::string& sort,
                                 const std::string& architecture, int page, TonesCallback callback)
{
    const auto url = tone3000::Url(tone3000::TonesSearchPath(query, gear, sort, architecture, page));
    Request("GET", url, {}, false, [callback = std::move(callback)](const HttpResult& result) {
        if (result.status != 200)
        {
            callback({}, result.status == 0 ? result.error
                                            : "Tone3000 search failed (" + std::to_string(result.status) + ")");
            return;
        }

        const auto body = nlohmann::json::parse(result.body, nullptr, false);

        if (body.is_discarded())
        {
            callback({}, "Tone3000 sent an answer that could not be read");
            return;
        }

        callback(tone3000::ParseTones(body), {});
    });
}

void ToneService::Tone3000Models(const std::string& toneId, ModelsCallback callback)
{
    // The default listing (A1 and custom, in the creator's order) page by page, then the A2 twins.
    struct Walk
    {
        std::vector<tone3000::Model> models;
        std::set<std::string> ids;
        std::string architecture;
        int page = 1;
    };

    auto walk = std::make_shared<Walk>();
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, walk, next, toneId, callback] {
        const auto url = tone3000::Url(tone3000::ModelsPath(toneId, walk->page, walk->architecture));
        Request("GET", url, {}, false, [walk, next, callback](const HttpResult& result) {
            const auto body =
                result.status == 200 ? nlohmann::json::parse(result.body, nullptr, false) : nlohmann::json();

            if (result.status == 0)
            {
                callback(walk->models, result.error);
                *next = nullptr;
                return;
            }

            // Past the last page the API answers 500; anything unreadable ends this listing.
            tone3000::Page<tone3000::Model> page;

            if (!body.is_discarded() && !body.is_null())
            {
                page = tone3000::ParseModels(body);
            }

            for (auto& model : page.items)
            {
                if (walk->ids.insert(model.id).second)
                {
                    walk->models.push_back(std::move(model));
                }
            }

            if (!page.items.empty() && walk->page < page.totalPages && walk->page < tone3000::kMaxModelPages)
            {
                ++walk->page;
                (*next)();
                return;
            }

            if (walk->architecture.empty())
            {
                walk->architecture = "2";
                walk->page = 1;
                (*next)();
                return;
            }

            callback(walk->models, {});
            *next = nullptr; // breaks the cycle
        });
    };
    (*next)();
}

void ToneService::ImportTone3000(const tone3000::Tone& tone, const tone3000::Model& model,
                                 const std::string& resourceType, const std::string& category, ImportCallback callback)
{
    if (const auto* existing = tone3000::FindImported(mClient.State(), resourceType, model))
    {
        callback(existing->id, {});
        return;
    }

    if (model.modelUrl.empty())
    {
        callback({}, "Tone3000 gave no download for this model");
        return;
    }

    if (!mImporting.insert(model.id).second)
    {
        return;
    }

    Changed();

    Request("GET", tone3000::ThroughRelay(model.modelUrl), {}, false,
            [this, tone, model, resourceType, category, callback](const HttpResult& result) {
                const auto finish = [this, model, callback](const std::string& id, const std::string& error) {
                    mImporting.erase(model.id);
                    Changed();
                    callback(id, error);
                };

                if (result.status != 200 || result.body.empty())
                {
                    finish({}, result.status == 0 ? result.error
                                                  : "The download failed (" + std::to_string(result.status) + ")");
                    return;
                }

                std::string bytes = result.body;
                std::string entry;

                if (IsZip(bytes))
                {
                    const auto unzipped = UnzipModel(bytes, resourceType);

                    if (!unzipped)
                    {
                        finish({}, "The download holds no model");
                        return;
                    }

                    entry = unzipped->first;
                    bytes = unzipped->second;
                }

                ImportAndWait(tone3000::ImportPayload(tone, model, resourceType, category, Base64(bytes), entry),
                              finish);
            });
}

void ToneService::DownloadTone3000(const tone3000::Model& model, const std::string& resourceType,
                                   DownloadCallback callback)
{
    if (model.modelUrl.empty())
    {
        callback({}, "Tone3000 gave no download for this model");
        return;
    }

    Request("GET", tone3000::ThroughRelay(model.modelUrl), {}, false,
            [resourceType, callback](const HttpResult& result) {
                if (result.status != 200 || result.body.empty())
                {
                    callback({}, result.status == 0 ? result.error
                                                    : "The download failed (" + std::to_string(result.status) + ")");
                    return;
                }

                std::string bytes = result.body;

                if (IsZip(bytes))
                {
                    const auto unzipped = UnzipModel(bytes, resourceType);

                    if (!unzipped)
                    {
                        callback({}, "The download holds no model");
                        return;
                    }

                    bytes = unzipped->second;
                }

                callback(Base64(bytes), {});
            });
}

void ToneService::Tone3000ModelsByContentHash(const std::string& contentHash, ModelsCallback callback)
{
    Request("GET", tone3000::Url(tone3000::ModelLookupPath(contentHash)), {}, false,
            [callback](const HttpResult& result) {
                if (result.status != 200)
                {
                    callback({}, result.status == 0 ? result.error
                                                    : "lookup failed (" + std::to_string(result.status) + ")");
                    return;
                }

                const auto body = nlohmann::json::parse(result.body, nullptr, false);
                callback(body.is_discarded() ? std::vector<tone3000::Model>{} : tone3000::ParseModels(body).items, {});
            });
}

void ToneService::FetchRemoteRef(const tone3000::RemoteRef& ref,
                                 std::function<void(const std::string&, const std::string&)> done)
{
    const auto download = [this](const std::string& url,
                                 std::function<void(const std::string&, const std::string&)> finished) {
        Request("GET", tone3000::ThroughRelay(url), {}, false, [finished](const HttpResult& result) {
            if (result.status != 200 || result.body.empty())
            {
                finished({},
                         result.status == 0 ? result.error : "download failed (" + std::to_string(result.status) + ")");
            }
            else
            {
                finished(result.body, {});
            }
        });
    };

    // When its link fails (the model re-uploaded under a new id, its tone moved or made private),
    // a model with the same content hash is the same file, wherever it is now.
    const auto byContentHash = [this, ref, done, download](const std::string& error) {
        if (ref.contentHash.empty())
        {
            done({}, error);
            return;
        }

        Tone3000ModelsByContentHash(
            ref.contentHash,
            [ref, done, download, error](const std::vector<tone3000::Model>& models, const std::string&) {
                const auto sameTone = std::find_if(models.begin(), models.end(), [&](const tone3000::Model& model) {
                    return !ref.toneId.empty() && model.toneId == ref.toneId && !model.modelUrl.empty();
                });
                const auto any = std::find_if(models.begin(), models.end(),
                                              [](const tone3000::Model& model) { return !model.modelUrl.empty(); });
                const auto match = sameTone != models.end() ? sameTone : any;

                if (match == models.end())
                {
                    done({}, error);
                    return;
                }

                download(match->modelUrl, done);
            });
    };

    if (!ref.modelUrl.empty())
    {
        download(ref.modelUrl, [done, byContentHash](const std::string& bytes, const std::string& error) {
            if (error.empty())
            {
                done(bytes, {});
            }
            else
            {
                byContentHash(error);
            }
        });
        return;
    }

    if (ref.toneId.empty() || ref.modelId.empty())
    {
        byContentHash("the preset does not say where it came from");
        return;
    }

    // Its URL from its tone's models: download URLs expire, so they are asked for now.
    Tone3000Models(ref.toneId, [ref, done, download, byContentHash](const std::vector<tone3000::Model>& models,
                                                                    const std::string& error) {
        for (const auto& model : models)
        {
            if (model.id == ref.modelId && !model.modelUrl.empty())
            {
                download(model.modelUrl, done);
                return;
            }
        }

        byContentHash(error.empty() ? "no longer on Tone3000" : error);
    });
}

void ToneService::ImportAndWait(nlohmann::json payload, ImportCallback done)
{
    const auto requestId = mCommands.ImportRemoteResource(std::move(payload));
    mPendingImports[requestId] = std::move(done);
}

void ToneService::OnResourcesChanged()
{
    auto& imports = mClient.MutableState().resourceImports;

    for (auto it = mPendingImports.begin(); it != mPendingImports.end();)
    {
        const auto found = imports.find(it->first);

        if (found == imports.end() || found->second.status == ResourceImport::Status::Importing)
        {
            ++it;
            continue;
        }

        auto callback = std::move(it->second);
        const auto result = found->second;
        imports.erase(found);
        it = mPendingImports.erase(it);

        if (callback)
        {
            callback(result.status == ResourceImport::Status::Imported ? result.resourceId : std::string(),
                     result.status == ResourceImport::Status::Failed
                         ? (result.detail.empty() ? "the import failed" : result.detail)
                         : std::string());
        }
    }
}
} // namespace guitarfx::uiclient
