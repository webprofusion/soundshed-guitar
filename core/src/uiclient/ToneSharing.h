#pragma once

/**
 * ToneSharing.h - The Soundshed tone sharing service (https://api-guitar.soundshed.com/v1),
 * as a native UI reads it: the shapes of its answers, the paths it is asked, and what the
 * library says is already installed.
 *
 * Soundshed Guitar's web UI talks to the service from TypeScript (core/ui/ts/toneSharingPanel/);
 * this is the same contract for a UI that has no browser. It makes no requests itself - the
 * UI's HTTP client does, and hands the status and body here - so it stays testable without a
 * network. Installing what is downloaded is the engine's job ("installPresetArchives"), and the
 * record of it is the toneSharing.installedPacks setting both UIs share.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx::uiclient::tones
{
inline constexpr const char* kApiBase = "https://api-guitar.soundshed.com/v1";
inline constexpr const char* kApiOrigin = "https://api-guitar.soundshed.com";

/// App settings the web UI's tone sharing panel keeps, read and written the same way here.
inline constexpr const char* kSessionSettingKey = "toneSharing.sessionId";
inline constexpr const char* kInstalledSettingKey = "toneSharing.installedPacks";

/// Community presets are listed this many at a time, as the web UI asks for them.
inline constexpr int kItemsPageSize = 36;
inline constexpr int kPacksPageSize = 24;

/// A shared preset or pack, as a list shows it.
struct Tone
{
    enum class Kind
    {
        Preset,
        Pack
    };

    Kind kind = Kind::Preset;
    std::string id;
    std::string title;
    std::string type; // preset, blend, layout...; empty for a pack
    std::string description;
    std::vector<std::string> tags;
    std::string creatorId;
    std::string creatorName;
    std::string creatorHandle; // "@name", or empty
    std::string creatorAvatarUrl;
    std::string thumbnailUrl; // absolute, or empty
    std::string moderationStatus;
    std::int64_t downloads = -1; // -1 where the service does not say (packs)
    bool featured = false;

    [[nodiscard]] bool IsPack() const noexcept { return kind == Kind::Pack; }
};

/// One row of the featured page.
struct HomeRow
{
    std::string id;
    std::string title;
    std::vector<Tone> tones;
};

/// A pack and the presets in it, in the pack's order.
struct PackDetail
{
    struct Entry
    {
        std::string itemId;
        std::string title;
        std::string type;
        int sortOrder = 0;
    };

    Tone pack;
    std::vector<Entry> items;
};

/// The signed-in user.
struct User
{
    std::string id;
    std::string email;
    std::string displayName;
    std::string handle;
    std::string role;

    /// What the account button shows: the display name, else the email.
    [[nodiscard]] std::string Label() const { return displayName.empty() ? email : displayName; }
};

/// One answer from the service, unwrapped from its {ok, data, error} envelope.
struct Response
{
    int status = 0;
    bool ok = false;
    nlohmann::json data;  // the envelope's data, on success
    std::string error;    // the service's message, or "Request failed (<status>)"
    std::string sessionId; // a session the answer handed out (sign-in), or empty
};

/// Unwraps a JSON answer. `sessionHeader` is the response's x-session-id header, if any.
/// A status of 0 means the request never got an answer (no network).
[[nodiscard]] Response ParseResponse(int status, const std::string& body, const std::string& sessionHeader = {});

[[nodiscard]] std::vector<HomeRow> ParseHome(const nlohmann::json& data);
[[nodiscard]] std::vector<Tone> ParseItems(const nlohmann::json& data);
[[nodiscard]] std::vector<Tone> ParsePacks(const nlohmann::json& data);
[[nodiscard]] std::optional<Tone> ParseItem(const nlohmann::json& data);
[[nodiscard]] std::optional<PackDetail> ParsePackDetail(const nlohmann::json& data);
[[nodiscard]] std::optional<User> ParseUser(const nlohmann::json& data);

// ── Paths (relative to kApiBase) and links ──────────────────────────────────────

[[nodiscard]] std::string ItemsPath(int page, const std::string& query, const std::string& tag);
[[nodiscard]] std::string PacksPath(int page);
[[nodiscard]] std::string ItemPath(const std::string& itemId);
[[nodiscard]] std::string ItemDownloadPath(const std::string& itemId);
[[nodiscard]] std::string PackPath(const std::string& packId);
[[nodiscard]] std::string PackDownloadPath(const std::string& packId);

/// A path from kApiBase, or a URL the service gave ("/v1/packs/x/thumbnail"), made absolute.
[[nodiscard]] std::string ResolveUrl(const std::string& pathOrUrl);

/// The link the web UI's Share button copies, which opens the tone in the app when installed.
[[nodiscard]] std::string ShareLink(const Tone& tone);

/// Percent-encodes a query parameter value.
[[nodiscard]] std::string EncodeQueryValue(const std::string& value);

// ── What is installed (the toneSharing.installedPacks setting) ──────────────────

/// The installedPacks entry id an install of this preset or pack is recorded under.
[[nodiscard]] std::string ItemEntryId(const std::string& itemId);
[[nodiscard]] std::string PackEntryId(const std::string& packId);
[[nodiscard]] std::string EntryIdFor(const Tone& tone);

struct InstalledEntry
{
    std::string id;
    std::string title;
    std::string source; // toneSharingApi, zipImport, generatedPack
    std::string importedAt;
    std::string packId;
    std::vector<std::string> presetIds;
    std::size_t resourceCount = 0;

    [[nodiscard]] bool IsPack() const { return !packId.empty() || presetIds.size() > 1; }
};

/// The entries of the installedPacks setting, newest first as stored.
[[nodiscard]] std::vector<InstalledEntry> InstalledEntries(const nlohmann::json& appSettings);
[[nodiscard]] std::optional<InstalledEntry> FindInstalled(const nlohmann::json& appSettings, const std::string& entryId);

// ── Display ─────────────────────────────────────────────────────────────────────

/// "1.2K", "3.4M": the web UI's compact download count.
[[nodiscard]] std::string FormatCount(std::int64_t count);

/// "@name" from a handle the service gives with or without its "@"; empty if it is not one.
[[nodiscard]] std::string NormalizeHandle(const std::string& handle);

/// The tags a shared preset can carry (core/ui/ts/presetTags.ts), for the tag filter.
[[nodiscard]] const std::vector<std::string>& StandardTags();
} // namespace guitarfx::uiclient::tones
