#pragma once

/**
 * Tone3000.h - Tone3000 (www.tone3000.com) as the native UIs reach it: through Soundshed's own
 * relay (https://api-guitar.soundshed.com/v1/resourcesearch), which adds the shared key, so a UI
 * sends no credentials. The shapes of its answers, the paths it is asked, what a model is called
 * once imported, and which of a shared preset's models still have to be fetched.
 *
 * The web UI's equivalent is core/ui/ts/tone3000*.ts and resourceBrowser/tone3000Tab.ts. Like
 * ToneSharing.h this makes no requests: ToneService does, and hands the answers here.
 */

#include "uiclient/ClientState.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace guitarfx::uiclient::tone3000
{
inline constexpr const char* kProxyBase = "https://api-guitar.soundshed.com/v1/resourcesearch";

/// Results a search page holds, and the models one page of a tone holds, as the web UI asks.
inline constexpr int kSearchPageSize = 20;
inline constexpr int kModelsPageSize = 100;
/// Pages of a tone's models read at most (100 each), as the web UI's fetchAllTone3000ModelPages.
inline constexpr int kMaxModelPages = 50;

/// A tone: one capture session, holding models (NAM) or IRs.
struct Tone
{
    std::string id;
    std::string title;
    std::string slug;
    std::string description;
    std::string gear;     // amp, amp-cab, pedal, outboard, cab, space, experimental
    std::string platform; // nam, ir, aida-x...
    std::string license;
    std::string imageUrl;
    std::string creatorId, creatorName, creatorUsername, creatorUrl;
    int modelsCount = 0;
    int irsCount = 0;
    std::int64_t downloads = 0;
    std::vector<std::string> tags;
};

struct Model
{
    std::string id;
    std::string name;
    std::string modelUrl;
    std::string toneId;
    std::string architecture; // "1", "2", "custom", or empty
    std::string contentHash;  // "nam-content-v1:..." / "ir-content-v1:...", or empty
};

template <typename T> struct Page
{
    std::vector<T> items;
    int page = 1;
    int totalPages = 1;
};

/// A path under kProxyBase. `gear` and `sort` as the API takes them (sort: downloads-all-time,
/// newest, trending, best-match); `architecture` "1", "2" or empty for any.
[[nodiscard]] std::string TonesSearchPath(const std::string& query, const std::string& gear, const std::string& sort,
                                          const std::string& architecture, int page, int pageSize = kSearchPageSize);
/// A tone's models. Without an architecture the API lists A1 and custom models only; A2 twins
/// need architecture "2" (and newer tones are A2 only).
[[nodiscard]] std::string ModelsPath(const std::string& toneId, int page, const std::string& architecture,
                                     int pageSize = kModelsPageSize);
/// Models whose content hash (models/ContentHash.h) is `contentHash`, oldest first: reposts of
/// one file share it.
[[nodiscard]] std::string ModelLookupPath(const std::string& contentHash);
[[nodiscard]] std::string Url(const std::string& path);

/// A URL Tone3000 gave on its own API (a model's `model_url`,
/// https://www.tone3000.com/api/v1/models/<id>/download/...), moved onto the relay, which adds
/// the key that API wants; any other URL as it is (the web UI's normalizeTone3000RequestUrl).
[[nodiscard]] std::string ThroughRelay(const std::string& url);

[[nodiscard]] Page<Tone> ParseTones(const nlohmann::json& body);
[[nodiscard]] Page<Model> ParseModels(const nlohmann::json& body);
[[nodiscard]] Tone ParseTone(const nlohmann::json& tone);

/// The capture a tone is ("amp+cab" for a full rig), recorded with an import.
[[nodiscard]] std::string CaptureScope(const std::string& gear);

/// The library id a model is imported under (`tone3000:<model>`, `tone3000:<model>:<entry>` for
/// one file of a zip), as the web UI names it, so either UI finds the other's import.
[[nodiscard]] std::string LibraryId(const Model& model, const std::string& zipEntry = {});

/// The library entry a model was imported as, by its id or the model id its metadata records.
[[nodiscard]] const LibraryResource* FindImported(const ClientState& state, const std::string& resourceType,
                                                  const Model& model);

/// The importRemoteResource payload for a model's bytes (base64 in `data`).
[[nodiscard]] nlohmann::json ImportPayload(const Tone& tone, const Model& model, const std::string& resourceType,
                                           const std::string& category, std::string base64Data,
                                           const std::string& zipEntry = {});

// ── A shared preset's models ────────────────────────────────────────────────────

/// A model or IR a preset archive names rather than carries (its `remoteResources`).
struct RemoteRef
{
    std::string id; // the library id the archive's presets use
    std::string type = "nam";
    std::string name;
    std::string category;
    std::string provider;
    std::string providerKey;
    std::string hash;
    std::string contentHash; // the model's content hash, to find it when its ids are gone
    std::string toneId, modelId, modelUrl;
    nlohmann::json raw;
};

/// The archive's remote references none of the library's entries matches (by id; by provider
/// and provider key, or a Tone3000 tone and model id; by file hash or model content hash), the engine's own rules
/// (RemoteResourceRefs.cpp). Empty for bytes that are not a preset archive.
[[nodiscard]] std::vector<RemoteRef> MissingRemoteRefs(const ClientState& state, const std::string& archiveBytes);

/// The importRemoteResource payload for a referenced Tone3000 model fetched for an install: under
/// the reference's own id, so the archive's presets resolve to it.
[[nodiscard]] nlohmann::json ImportPayloadForRef(const RemoteRef& ref, std::string base64Data);
} // namespace guitarfx::uiclient::tone3000
