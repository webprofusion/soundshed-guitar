#pragma once

/**
 * ToneService - the native UIs' network side, shared by every product that has no browser:
 * Soundshed's tone sharing service (browse, account, installing shared presets and packs) and
 * Tone3000 through Soundshed's relay (search, a tone's models, importing a model into the
 * library). It makes the requests (net::HttpWorkers on a transport the product chooses), hands
 * the answers to the pure helpers in ToneSharing.h and Tone3000.h, and sends the engine what to
 * store (importRemoteResource, installPresetArchives).
 *
 * Everything public is called on the UI thread, and every callback comes back on it, through the
 * Host's `post`; none arrives once the service is gone. The account is the web UI's own setting
 * (toneSharing.sessionId), so signing in in either product signs in both.
 *
 * Installing: the engine refuses an archive whose Tone3000 models are not in the library, and
 * has no network to fetch them, so the service fetches what is missing first (as the web UI's
 * importPresetArchive does), imports each, waits for the engine to store them, then installs.
 */

#include "net/Http.h"
#include "uiclient/Jam.h"
#include "uiclient/Tone3000.h"
#include "uiclient/ToneSharing.h"
#include "uiclient/UpdateCheck.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class ToneService
{
  public:
    struct Host
    {
        /// Runs `fn` on the UI thread, soon. Called from worker threads.
        std::function<void(std::function<void()>)> post;
        /// Shows a transient message.
        std::function<void(const std::string& title, const std::string& detail, bool isError)> toast;
    };

    using Response = tones::Response;
    using Callback = std::function<void(const Response&)>;

    /// One downloaded archive: its title, its bytes, and the item it is (recorded with the install,
    /// so a favourite or rating of its preset can reach the service).
    struct Download
    {
        std::string title;
        std::string bytes;
        std::string itemId;
    };

    ToneService(UiClient& client, UiCommands& commands, Host host,
                std::shared_ptr<net::HttpTransport> transport = net::MakePlatformTransport());
    ~ToneService();

    ToneService(const ToneService&) = delete;
    ToneService& operator=(const ToneService&) = delete;

    /// Called whenever the account, a picture, or a download's progress changes.
    int AddListener(std::function<void()> listener);
    void RemoveListener(int id);

    // ── Software updates ────────────────────────────────────────────────────────
    /// Asks the Soundshed service for its newest release (POST /app/updatecheck, without the
    /// account's session: the web UI sends none either). The callback gets the update when it is
    /// newer than `currentVersion`, else nothing; a failed request is nothing too.
    void CheckForUpdate(const std::string& currentVersion, const std::string& os, const std::string& cpu,
                        bool standalone, const std::string& instanceId,
                        std::function<void(const std::optional<AvailableUpdate>&)> callback);

    // ── Jam ─────────────────────────────────────────────────────────────────────
    /// YouTube's search for embeddable backing tracks (the web UI's jam/search.ts), with the
    /// engine's bundled key: the videos, or the API's own message.
    using BackingTracksCallback = std::function<void(const std::vector<jam::Video>& videos, const std::string& error)>;
    void SearchBackingTracks(const std::string& normalizedQuery, const std::string& apiKey,
                             BackingTracksCallback callback);

    // ── Tone sharing ────────────────────────────────────────────────────────────
    void Get(const std::string& path, Callback callback);
    void Post(const std::string& path, const nlohmann::json& body, Callback callback);

    /// A picture's bytes by absolute URL: null while it is being fetched (listeners hear when it
    /// arrives), empty when the fetch failed. A failed fetch is not retried.
    [[nodiscard]] const std::string* Image(const std::string& url);

    [[nodiscard]] const std::optional<tones::User>& Account() const noexcept
    {
        return mAccount;
    }

    [[nodiscard]] bool HasSession() const;
    void RefreshAccount();
    void RequestCode(const std::string& email, Callback callback);
    void VerifyCode(const std::string& email, const std::string& code, Callback callback);
    void SignOut();

    /// Downloads a shared preset, or every preset of a pack, fetches the Tone3000 models it
    /// names that the library lacks, and asks the engine to install it.
    void Install(const tones::Tone& tone);

    [[nodiscard]] bool IsDownloading(const std::string& entryId) const
    {
        return mDownloading.count(entryId) > 0;
    }

    /// Removes an install: its presets, and resources nothing else uses.
    void Remove(const std::string& entryId);

    // ── Previewing (ToneServicePreview.cpp) ─────────────────────────────────────
    /// Auditions a shared preset without installing it: downloads it, fetches the Tone3000 models
    /// it names that the library lacks (found by their tone and model ids, never by a URL the
    /// archive gives), and has the engine play it (previewPresetArchive), which writes nothing.
    /// The preset playing before the first preview is remembered for StopPreview. A pack's
    /// presets are previewed one by one.
    void Preview(const tones::Tone& item);
    /// Loads the preset that was playing before the first preview, and ends the preview.
    void StopPreview();

    /// The item previewed, or being fetched for a preview; empty when none.
    [[nodiscard]] const std::string& PreviewItemId() const noexcept
    {
        return mPreviewItemId;
    }

    [[nodiscard]] const std::string& PreviewTitle() const noexcept
    {
        return mPreviewTitle;
    }

    /// Still downloading, or waiting for the engine to play it.
    [[nodiscard]] bool IsPreviewLoading() const noexcept
    {
        return mPreviewLoading;
    }

    // ── Tone3000 ────────────────────────────────────────────────────────────────
    using TonesCallback = std::function<void(const tone3000::Page<tone3000::Tone>& page, const std::string& error)>;
    using ModelsCallback = std::function<void(const std::vector<tone3000::Model>& models, const std::string& error)>;
    /// The library id the model was stored under, or an error.
    using ImportCallback = std::function<void(const std::string& resourceId, const std::string& error)>;

    void SearchTone3000(const std::string& query, const std::string& gear, const std::string& sort,
                        const std::string& architecture, int page, TonesCallback callback);
    /// Every model of a tone, both the default listing (A1 and custom) and the A2 twins, in the
    /// creator's order with A2 after.
    void Tone3000Models(const std::string& toneId, ModelsCallback callback);
    /// Tone3000's models whose content hash is `contentHash` (reposts share one), oldest first.
    void Tone3000ModelsByContentHash(const std::string& contentHash, ModelsCallback callback);
    /// Downloads a model and stores it in the library (an IR zip: its first .wav), then answers
    /// with its library id; a model already imported answers at once.
    void ImportTone3000(const tone3000::Tone& tone, const tone3000::Model& model, const std::string& resourceType,
                        const std::string& category, ImportCallback callback);
    /// The model's file (an IR zip: its first .wav), base64, without importing it: what an
    /// audition plays (UiCommands::PreviewRemoteResource), as the web UI's preview fetches it.
    using DownloadCallback = std::function<void(const std::string& base64, const std::string& error)>;
    void DownloadTone3000(const tone3000::Model& model, const std::string& resourceType, DownloadCallback callback);

    [[nodiscard]] bool IsImporting(const std::string& modelId) const
    {
        return mImporting.count(modelId) > 0;
    }

  private:
    struct HttpResult
    {
        int status = 0;
        std::string body;
        std::string sessionHeader;
        std::string error;
        bool droppedSession = false; // asked again without a session the service refused
    };

    using BytesCallback = std::function<void(const HttpResult&)>;

    void Request(const std::string& method, const std::string& url, const std::string& jsonBody, bool json,
                 BytesCallback done);
    void Changed();

    void InstallItem(const tones::Tone& tone);
    void InstallPack(const tones::Tone& tone);
    /// Fetches and imports what the archives lack, then sends the install.
    void ResolveAndInstall(const nlohmann::json& entry, const std::string& folder, std::vector<Download> archives);
    void SendInstall(const nlohmann::json& entry, const std::string& folder, const std::vector<Download>& archives);
    void DownloadFailed(const std::string& entryId, const std::string& title, const std::string& reason);
    void SetDownloading(const std::string& entryId, bool downloading);
    void OnInstallsChanged();

    /// Downloads a referenced model: its own URL when the archive gave one, else found among its
    /// tone's models. Answers with the bytes, or an error.
    void FetchRemoteRef(const tone3000::RemoteRef& ref,
                        std::function<void(const std::string& bytes, const std::string& error)> done);
    /// Sends an import and calls `done` once the engine answers it.
    void ImportAndWait(nlohmann::json payload, ImportCallback done);
    void OnResourcesChanged();

    // ToneServicePreview.cpp
    void SendPreview(int generation, const tones::Tone& item, const std::string& archive,
                     const nlohmann::json& fetched);
    void PreviewFailed(int generation, const std::string& reason);
    void OnActivePresetChanged();
    /// A favourite or rating given to a preset installed from tone sharing, sent to the service
    /// when signed in (PUT or DELETE /items/<id>/favorite and /rating), as the web UI's preset
    /// links send them. Others are local only.
    void SyncFavorite(const std::string& presetId, bool favorite);
    void SyncRating(const std::string& presetId, int rating);

    [[nodiscard]] std::string SessionId() const;
    void StoreSession(const std::string& session);

    UiClient& mClient;
    UiCommands& mCommands;
    Host mHost;
    std::shared_ptr<std::atomic<bool>> mAlive = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<net::HttpWorkers> mWorkers;

    std::optional<tones::User> mAccount;
    std::map<std::string, std::string> mImages; // url -> bytes ("" when the fetch failed)
    std::set<std::string> mImagesRequested;
    std::set<std::string> mDownloading;
    std::map<std::string, std::string> mAwaiting;          // entry id -> title: installs sent to the engine
    std::set<std::string> mImporting;                      // Tone3000 model ids being imported
    std::map<std::string, ImportCallback> mPendingImports; // request id -> callback
    std::map<int, std::function<void()>> mListeners;
    int mNextListener = 1;
    Subscription mInstallSubscription, mResourceSubscription;

    // The preview (ToneServicePreview.cpp).
    std::string mPreviewItemId, mPreviewTitle;
    std::string mPriorPresetId; // what played before the first preview, put back by StopPreview
    bool mPreviewLoading = false;
    int mPreviewGeneration = 0; // a newer preview or a stop drops what an older one fetches
    Subscription mActivePresetSubscription;
};
} // namespace guitarfx::uiclient
