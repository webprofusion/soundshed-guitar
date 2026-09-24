#include "uiclient/ToneSharing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient::tones
{
namespace
{
std::string Text(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// The service's JSON is read field by field with its type checked: nlohmann's value() throws
// when a key is there with another type (a null), and one odd field must not lose a page.
bool Flag(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

int Integer(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<int>() : 0;
}

const nlohmann::json& Array(const nlohmann::json& object, const char* key)
{
    static const nlohmann::json empty = nlohmann::json::array();

    if (!object.is_object())
    {
        return empty;
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? *it : empty;
}

std::string Trimmed(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

/// The download count under any of the names the service has used (format.ts).
std::int64_t Downloads(const nlohmann::json& object)
{
    for (const char* key : {"downloadCount", "downloadsCount", "downloads_count"})
    {
        const auto it = object.find(key);

        if (it != object.end() && it->is_number() && it->get<double>() >= 0.0)
        {
            return static_cast<std::int64_t>(std::floor(it->get<double>()));
        }
    }

    return -1;
}

std::string CreatorHandle(const nlohmann::json& object)
{
    for (const char* key : {"creatorProfileHandle", "creatorHandle", "profileHandle", "handle"})
    {
        if (auto handle = NormalizeHandle(Text(object, key)); !handle.empty())
        {
            return handle;
        }
    }

    const auto displayName = Trimmed(Text(object, "creatorDisplayName"));
    return displayName.rfind('@', 0) == 0 ? NormalizeHandle(displayName) : std::string{};
}

Tone ParseTone(const nlohmann::json& object, Tone::Kind kind)
{
    Tone tone;
    tone.kind = kind;
    tone.id = Text(object, "id");
    tone.title = Trimmed(Text(object, "title"));
    tone.type = kind == Tone::Kind::Pack ? std::string{} : Text(object, "type");
    tone.description = Trimmed(Text(object, "description"));
    tone.creatorId = Text(object, "creatorUserId");
    tone.creatorName = Trimmed(Text(object, "creatorDisplayName"));
    tone.creatorHandle = CreatorHandle(object);
    tone.moderationStatus = Text(object, "moderationStatus");
    tone.featured = Flag(object, "featured");
    tone.downloads = kind == Tone::Kind::Pack ? -1 : Downloads(object);

    for (const char* key : {"creatorAvatarUrl", "avatarUrl"})
    {
        if (const auto url = Trimmed(Text(object, key)); !url.empty())
        {
            tone.creatorAvatarUrl = ResolveUrl(url);
            break;
        }
    }

    if (const auto tags = object.find("tags"); tags != object.end() && tags->is_array())
    {
        for (const auto& tag : *tags)
        {
            // "preset" is the type, not a tag worth showing (getToneSharingDisplayTags).
            if (tag.is_string() && !tag.get<std::string>().empty() && tag.get<std::string>() != "preset")
            {
                tone.tags.push_back(tag.get<std::string>());
            }
        }
    }

    // A pack's picture: the URL the service gives, or the one every pack answers on.
    if (kind == Tone::Kind::Pack && !tone.id.empty())
    {
        const auto url = Text(object, "thumbnailUrl");
        const auto asset = object.find("thumbnailAssetId");
        const bool hasAsset = asset != object.end() && asset->is_string() && !asset->get<std::string>().empty();

        if (!url.empty())
        {
            tone.thumbnailUrl = ResolveUrl(url);
        }
        else if (hasAsset)
        {
            tone.thumbnailUrl = ResolveUrl("/packs/" + tone.id + "/thumbnail");
        }
    }

    return tone;
}

std::vector<Tone> ParseList(const nlohmann::json& data, const char* key, Tone::Kind kind)
{
    std::vector<Tone> tones;

    if (!data.is_object())
    {
        return tones;
    }

    const auto list = data.find(key);

    if (list == data.end() || !list->is_array())
    {
        return tones;
    }

    for (const auto& entry : *list)
    {
        if (entry.is_object())
        {
            auto tone = ParseTone(entry, kind);

            if (!tone.id.empty())
            {
                tones.push_back(std::move(tone));
            }
        }
    }

    return tones;
}
} // namespace

Response ParseResponse(int status, const std::string& body, const std::string& sessionHeader)
{
    Response response;
    response.status = status;
    const auto payload = nlohmann::json::parse(body, nullptr, false);

    if (payload.is_object())
    {
        if (const auto data = payload.find("data"); data != payload.end())
        {
            response.data = *data;
        }

        if (const auto error = payload.find("error"); error != payload.end() && error->is_object())
        {
            response.error = Trimmed(Text(*error, "message"));
        }
    }

    const auto okField = payload.is_object() ? payload.find("ok") : payload.end();
    const bool refused = payload.is_object() && okField != payload.end() && okField->is_boolean() && !okField->get<bool>();
    response.ok = status >= 200 && status < 300 && payload.is_object() && !refused;

    if (!response.ok && response.error.empty())
    {
        response.error = status == 0 ? "Could not reach Soundshed. Check your connection."
                                     : "Request failed (" + std::to_string(status) + ")";
    }

    // Sign-in hands the session out in the body; the header takes precedence if both do.
    response.sessionId = Trimmed(sessionHeader);

    if (response.sessionId.empty() && payload.is_object())
    {
        response.sessionId = Trimmed(Text(payload, "sessionId"));

        if (response.sessionId.empty() && response.data.is_object())
        {
            response.sessionId = Trimmed(Text(response.data, "sessionId"));
        }
    }

    return response;
}

std::vector<HomeRow> ParseHome(const nlohmann::json& data)
{
    std::vector<HomeRow> rows;
    const auto list = data.is_object() ? data.find("rows") : data.end();

    if (!data.is_object() || list == data.end() || !list->is_array())
    {
        return rows;
    }

    for (const auto& row : *list)
    {
        if (!row.is_object())
        {
            continue;
        }

        HomeRow parsed;
        parsed.id = Text(row, "id");
        parsed.title = Trimmed(Text(row, "title"));

        for (const auto& entry : Array(row, "items"))
        {
            if (!entry.is_object())
            {
                continue;
            }

            auto tone = ParseTone(entry, Text(entry, "kind") == "pack" ? Tone::Kind::Pack : Tone::Kind::Preset);

            if (!tone.id.empty())
            {
                parsed.tones.push_back(std::move(tone));
            }
        }

        if (!parsed.tones.empty())
        {
            rows.push_back(std::move(parsed));
        }
    }

    return rows;
}

std::vector<Tone> ParseItems(const nlohmann::json& data)
{
    return ParseList(data, "items", Tone::Kind::Preset);
}

std::vector<Tone> ParsePacks(const nlohmann::json& data)
{
    return ParseList(data, "packs", Tone::Kind::Pack);
}

std::optional<Tone> ParseItem(const nlohmann::json& data)
{
    const auto item = data.is_object() ? data.find("item") : data.end();

    if (!data.is_object() || item == data.end() || !item->is_object())
    {
        return std::nullopt;
    }

    auto tone = ParseTone(*item, Tone::Kind::Preset);
    return tone.id.empty() ? std::nullopt : std::optional<Tone>(std::move(tone));
}

std::optional<PackDetail> ParsePackDetail(const nlohmann::json& data)
{
    const auto pack = data.is_object() ? data.find("pack") : data.end();

    if (!data.is_object() || pack == data.end() || !pack->is_object())
    {
        return std::nullopt;
    }

    PackDetail detail;
    detail.pack = ParseTone(*pack, Tone::Kind::Pack);

    if (detail.pack.id.empty())
    {
        return std::nullopt;
    }

    for (const auto& entry : Array(data, "items"))
    {
        if (!entry.is_object() || Text(entry, "itemId").empty())
        {
            continue;
        }

        detail.items.push_back({Text(entry, "itemId"), Trimmed(Text(entry, "title")), Text(entry, "type"),
                                Integer(entry, "sortOrder")});
    }

    std::stable_sort(detail.items.begin(), detail.items.end(),
                     [](const auto& a, const auto& b) { return a.sortOrder < b.sortOrder; });
    return detail;
}

std::optional<User> ParseUser(const nlohmann::json& data)
{
    const auto user = data.is_object() ? data.find("user") : data.end();

    if (!data.is_object() || user == data.end() || !user->is_object() || Text(*user, "id").empty())
    {
        return std::nullopt;
    }

    User parsed;
    parsed.id = Text(*user, "id");
    parsed.email = Trimmed(Text(*user, "email"));
    parsed.displayName = Trimmed(Text(*user, "displayName"));
    parsed.handle = NormalizeHandle(Text(*user, "creatorProfileHandle"));
    parsed.role = Text(*user, "role");
    return parsed;
}

// ── Paths and links ─────────────────────────────────────────────────────────────

std::string EncodeQueryValue(const std::string& value)
{
    std::string encoded;

    for (const unsigned char ch : value)
    {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            encoded += static_cast<char>(ch);
        }
        else
        {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", ch);
            encoded += escaped;
        }
    }

    return encoded;
}

std::string ItemsPath(int page, const std::string& query, const std::string& tag)
{
    std::string path = "/items?page=" + std::to_string(std::max(1, page)) + "&pageSize=" + std::to_string(kItemsPageSize);

    if (const auto q = Trimmed(query); !q.empty())
    {
        path += "&q=" + EncodeQueryValue(q);
    }

    if (!tag.empty())
    {
        path += "&tag=" + EncodeQueryValue(tag);
    }

    return path;
}

std::string PacksPath(int page)
{
    return "/packs?page=" + std::to_string(std::max(1, page)) + "&pageSize=" + std::to_string(kPacksPageSize);
}

std::string ItemPath(const std::string& itemId)
{
    return "/items/" + EncodeQueryValue(itemId);
}

std::string ItemDownloadPath(const std::string& itemId)
{
    return ItemPath(itemId) + "/download";
}

std::string PackPath(const std::string& packId)
{
    return "/packs/" + EncodeQueryValue(packId);
}

std::string PackDownloadPath(const std::string& packId)
{
    return PackPath(packId) + "/download";
}

std::string ResolveUrl(const std::string& pathOrUrl)
{
    if (pathOrUrl.rfind("http://", 0) == 0 || pathOrUrl.rfind("https://", 0) == 0)
    {
        return pathOrUrl;
    }

    const std::string path = pathOrUrl.empty() || pathOrUrl.front() != '/' ? "/" + pathOrUrl : pathOrUrl;

    // The service's own URLs already carry the version ("/v1/packs/..."): not twice.
    if (path == "/v1" || path.rfind("/v1/", 0) == 0)
    {
        return std::string(kApiOrigin) + path;
    }

    return std::string(kApiBase) + path;
}

std::string ShareLink(const Tone& tone)
{
    return std::string(kApiOrigin) + (tone.IsPack() ? "/share/pack/" : "/share/item/") + EncodeQueryValue(tone.id);
}

// ── What is installed ───────────────────────────────────────────────────────────

std::string ItemEntryId(const std::string& itemId)
{
    return "tone-sharing-api:item:" + itemId;
}

std::string PackEntryId(const std::string& packId)
{
    return "tone-sharing-api:" + packId;
}

std::string EntryIdFor(const Tone& tone)
{
    return tone.IsPack() ? PackEntryId(tone.id) : ItemEntryId(tone.id);
}

std::vector<InstalledEntry> InstalledEntries(const nlohmann::json& appSettings)
{
    std::vector<InstalledEntry> entries;

    if (!appSettings.is_object())
    {
        return entries;
    }

    const auto list = appSettings.find(kInstalledSettingKey);

    if (list == appSettings.end() || !list->is_array())
    {
        return entries;
    }

    for (const auto& value : *list)
    {
        if (!value.is_object() || Text(value, "id").empty())
        {
            continue;
        }

        InstalledEntry entry;
        entry.id = Text(value, "id");
        entry.title = Text(value, "title");
        entry.source = Text(value, "source");
        entry.importedAt = Text(value, "importedAt");
        entry.packId = Text(value, "packId");

        for (const auto& id : Array(value, "presetIds"))
        {
            if (id.is_string())
            {
                entry.presetIds.push_back(id.get<std::string>());
            }
        }

        entry.resourceCount = Array(value, "resources").size();
        entries.push_back(std::move(entry));
    }

    return entries;
}

std::optional<InstalledEntry> FindInstalled(const nlohmann::json& appSettings, const std::string& entryId)
{
    for (auto& entry : InstalledEntries(appSettings))
    {
        if (entry.id == entryId)
        {
            return entry;
        }
    }

    return std::nullopt;
}

// ── Display ─────────────────────────────────────────────────────────────────────

std::string FormatCount(std::int64_t count)
{
    if (count < 0)
    {
        return "0";
    }

    const auto scaled = [](double value, const char* suffix) {
        // Half up, as the web UI's toFixed does; printf may round a half to even.
        const bool whole = value >= 10.0;
        const double rounded = whole ? std::floor(value + 0.5) : std::floor(value * 10.0 + 0.5) / 10.0;
        char text[32];
        std::snprintf(text, sizeof(text), whole ? "%.0f" : "%.1f", rounded);
        std::string formatted = text;

        if (formatted.size() > 2 && formatted.compare(formatted.size() - 2, 2, ".0") == 0)
        {
            formatted.resize(formatted.size() - 2);
        }

        return formatted + suffix;
    };

    if (count >= 1'000'000)
    {
        return scaled(static_cast<double>(count) / 1'000'000.0, "M");
    }

    if (count >= 1'000)
    {
        return scaled(static_cast<double>(count) / 1'000.0, "K");
    }

    return std::to_string(count);
}

std::string NormalizeHandle(const std::string& handle)
{
    auto value = Trimmed(handle);
    value.erase(0, value.find_first_not_of('@') == std::string::npos ? value.size() : value.find_first_not_of('@'));

    if (value.size() < 2 || value.size() > 64)
    {
        return {};
    }

    for (const unsigned char ch : value)
    {
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-'))
        {
            return {};
        }
    }

    return "@" + value;
}

const std::vector<std::string>& StandardTags()
{
    static const std::vector<std::string> tags = {"lead",    "rhythm",      "bass", "clean",  "crunch", "high-gain",
                                                  "ambient", "atmospheric", "live", "studio", "stereo", "dual-amp"};
    return tags;
}
} // namespace guitarfx::uiclient::tones
