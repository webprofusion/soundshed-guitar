#include "uiclient/Tone3000.h"

#include "controller/internal/PresetArchiveSupport.h"
#include "models/ContentHash.h"
#include "uiclient/ToneSharing.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace guitarfx::uiclient::tone3000
{
namespace
{
/// A field as text whether the API sends it as a string or a number (ids are both).
std::string Text(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);

    if (it == object.end())
    {
        return {};
    }

    if (it->is_string())
    {
        return it->get<std::string>();
    }

    if (it->is_number_integer())
    {
        return std::to_string(it->get<std::int64_t>());
    }

    if (it->is_number())
    {
        return nlohmann::json(*it).dump();
    }

    return {};
}

int Int(const nlohmann::json& object, const char* key, int fallback = 0)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<int>() : fallback;
}

std::string Meta(const LibraryResource& resource, const char* key)
{
    return Text(resource.metadata, key);
}

template <typename T, typename Parse> Page<T> ParsePage(const nlohmann::json& body, Parse parse)
{
    Page<T> page;
    const nlohmann::json* list = &body;

    if (body.is_object())
    {
        if (const auto it = body.find("data"); it != body.end())
        {
            list = &*it;
        }

        page.page = std::max(1, Int(body, "page", 1));
        // The relay and older answers name the page count in more than one way.
        int pages = Int(body, "total_pages", Int(body, "totalPages", Int(body, "pages", 0)));

        if (pages <= 0)
        {
            const int total = Int(body, "total", Int(body, "total_count", Int(body, "count", 0)));
            const int size = std::max(1, Int(body, "page_size", 1));
            pages = total > 0 ? (total + size - 1) / size : 1;
        }

        page.totalPages = std::max(1, pages);
    }

    if (list->is_array())
    {
        for (const auto& item : *list)
        {
            if (item.is_object())
            {
                page.items.push_back(parse(item));
            }
        }
    }

    return page;
}

std::string Extension(const std::string& resourceType, const std::string& zipEntry)
{
    if (!zipEntry.empty())
    {
        if (const auto dot = zipEntry.rfind('.'); dot != std::string::npos)
        {
            return zipEntry.substr(dot);
        }
    }

    return resourceType == "ir" ? ".wav" : ".nam";
}
} // namespace

std::string TonesSearchPath(const std::string& query, const std::string& gear, const std::string& sort,
                            const std::string& architecture, int page, int pageSize)
{
    std::string path =
        "/tones/search?page=" + std::to_string(std::max(1, page)) + "&page_size=" + std::to_string(pageSize);

    if (!query.empty())
    {
        path += "&query=" + tones::EncodeQueryValue(query);
    }

    if (!gear.empty())
    {
        path += "&gear=" + tones::EncodeQueryValue(gear);
    }

    if (!sort.empty())
    {
        path += "&sort=" + tones::EncodeQueryValue(sort);
    }

    if (!architecture.empty())
    {
        path += "&architecture=" + tones::EncodeQueryValue(architecture);
    }

    return path;
}

std::string ModelsPath(const std::string& toneId, int page, const std::string& architecture, int pageSize)
{
    std::string path = "/models?tone_id=" + tones::EncodeQueryValue(toneId) +
                       "&page=" + std::to_string(std::max(1, page)) + "&page_size=" + std::to_string(pageSize);

    if (!architecture.empty())
    {
        path += "&architecture=" + tones::EncodeQueryValue(architecture);
    }

    return path;
}

std::string ModelLookupPath(const std::string& contentHash)
{
    return "/models/lookup?content_hash=" + tones::EncodeQueryValue(contentHash) + "&page=1&page_size=25";
}

std::string Url(const std::string& path)
{
    return std::string(kProxyBase) + path;
}

std::string ThroughRelay(const std::string& url)
{
    static constexpr std::string_view kOfficialApi = "https://www.tone3000.com/api/v1/";

    if (url.rfind(kOfficialApi, 0) != 0)
    {
        return url;
    }

    return std::string(kProxyBase) + "/" + url.substr(kOfficialApi.size());
}

Tone ParseTone(const nlohmann::json& t)
{
    Tone tone;
    tone.id = Text(t, "id");
    tone.title = Text(t, "title");

    if (tone.title.empty())
    {
        tone.title = Text(t, "name");
    }

    tone.slug = Text(t, "slug");
    tone.description = Text(t, "description");
    tone.gear = Text(t, "gear");
    tone.platform = Text(t, "platform");
    tone.license = Text(t, "license");
    tone.modelsCount = Int(t, "models_count");
    tone.irsCount = Int(t, "irs_count");
    tone.downloads = Int(t, "downloads_count");

    // The picture: the first of the images, else whichever named field has one.
    if (const auto images = t.find("images");
        images != t.end() && images->is_array() && !images->empty() && images->front().is_string())
    {
        tone.imageUrl = images->front().get<std::string>();
    }

    for (const char* key : {"equipment_image_url", "equipment_image", "gear_image_url", "image_url", "thumbnail_url"})
    {
        if (tone.imageUrl.empty())
        {
            tone.imageUrl = Text(t, key);
        }
    }

    if (tone.imageUrl.rfind("http://", 0) != 0 && tone.imageUrl.rfind("https://", 0) != 0)
    {
        tone.imageUrl.clear();
    }

    if (const auto user = t.find("user"); user != t.end() && user->is_object())
    {
        tone.creatorId = Text(*user, "id");
        tone.creatorUsername = Text(*user, "username");
        tone.creatorUrl = Text(*user, "url");
        tone.creatorName = Text(*user, "display_name");

        if (tone.creatorName.empty())
        {
            tone.creatorName = Text(*user, "name");
        }

        if (tone.creatorName.empty())
        {
            tone.creatorName = tone.creatorUsername;
        }
    }

    if (const auto tags = t.find("tags"); tags != t.end() && tags->is_array())
    {
        for (const auto& tag : *tags)
        {
            if (const auto name = tag.is_object() ? Text(tag, "name") : std::string(); !name.empty())
            {
                tone.tags.push_back(name);
            }
        }
    }

    return tone;
}

Page<Tone> ParseTones(const nlohmann::json& body)
{
    return ParsePage<Tone>(body, ParseTone);
}

Page<Model> ParseModels(const nlohmann::json& body)
{
    return ParsePage<Model>(body, [](const nlohmann::json& m) {
        Model model;
        model.id = Text(m, "id");
        model.name = Text(m, "name");
        model.modelUrl = Text(m, "model_url");
        model.toneId = Text(m, "tone_id");
        model.architecture = Text(m, "architecture_version");
        model.contentHash = Text(m, "content_hash");
        return model;
    });
}

std::string CaptureScope(const std::string& gear)
{
    if (gear == "amp")
    {
        return "amp";
    }

    if (gear == "amp-cab" || gear == "full-rig")
    {
        return "amp+cab";
    }

    if (gear == "pedal" || gear == "outboard" || gear == "cab")
    {
        return gear;
    }

    if (gear == "space")
    {
        return "room";
    }

    return {};
}

std::string LibraryId(const Model& model, const std::string& zipEntry)
{
    return "tone3000:" + model.id + (zipEntry.empty() ? std::string() : ":" + util::SanitizeFilename(zipEntry));
}

const LibraryResource* FindImported(const ClientState& state, const std::string& resourceType, const Model& model)
{
    const auto it = state.resources.find(resourceType);

    if (it == state.resources.end())
    {
        return nullptr;
    }

    const auto id = LibraryId(model);

    for (const auto& resource : it->second)
    {
        if (resource.id == id || resource.id.rfind(id + ":", 0) == 0 ||
            (Meta(resource, "provider") == "tone3000" && Meta(resource, "modelId") == model.id))
        {
            return &resource;
        }
    }

    return nullptr;
}

nlohmann::json ImportPayload(const Tone& tone, const Model& model, const std::string& resourceType,
                             const std::string& category, std::string base64Data, const std::string& zipEntry)
{
    const auto sourceUrl = "https://www.tone3000.com/tones/" + (tone.slug.empty() ? tone.id : tone.slug);
    nlohmann::json metadata = {
        {"provider", "tone3000"},
        {"providerKey", zipEntry.empty() ? model.id : model.id + ":" + util::SanitizeFilename(zipEntry)},
        {"tone3000Category", tone.gear},
        {"toneId", tone.id},
        {"toneTitle", tone.title},
        {"groupId", tone.id},
        {"groupName", tone.title},
        {"gear", tone.gear},
        {"platform", tone.platform},
        {"modelId", model.id},
        {"modelName", model.name},
        {"imageUrl", tone.imageUrl},
        {"architectureVersion", model.architecture},
        {"sourceUrl", sourceUrl},
        {"creatorId", tone.creatorId},
        {"creatorName", tone.creatorName},
        {"authorUsername", tone.creatorUsername}};

    if (!model.modelUrl.empty())
    {
        metadata["modelUrl"] = model.modelUrl;
    }

    if (!tone.license.empty())
    {
        metadata["licence"] = tone.license;
    }

    if (!tone.creatorUrl.empty())
    {
        metadata["creatorUrl"] = tone.creatorUrl;
    }

    if (const auto scope = CaptureScope(tone.gear); !scope.empty())
    {
        metadata["captureScope"] = scope;
    }

    if (!zipEntry.empty())
    {
        metadata["entryName"] = zipEntry;
    }

    const auto name = tone.title.empty() ? model.name : tone.title + " - " + model.name;
    return {
        {"provider", "tone3000"},
        {"resourceType", resourceType},
        {"resourceId", LibraryId(model, zipEntry)},
        {"name", name},
        {"description", tone.description},
        {"category", category.empty() ? tone.gear : category},
        {"subfolder", util::SanitizeFilename(tone.gear.empty() ? std::string("models") : tone.gear) + "/" +
                          util::SanitizeFilename(tone.title.empty() ? tone.id : tone.title)},
        {"fileName", util::SanitizeFilename(zipEntry.empty() ? model.name + Extension(resourceType, {}) : zipEntry)},
        {"metadata", std::move(metadata)},
        {"data", std::move(base64Data)}};
}

// ── A shared preset's models ────────────────────────────────────────────────────

std::vector<RemoteRef> MissingRemoteRefs(const ClientState& state, const std::string& archiveBytes)
{
    std::string error;
    const std::vector<std::uint8_t> bytes(archiveBytes.begin(), archiveBytes.end());
    const auto parsed = controller_detail::ParseFactoryPresetArchive(util::PathFromUtf8("archive"), bytes, error);

    if (!parsed)
    {
        return {};
    }

    std::vector<RemoteRef> missing;

    for (const auto& reference : parsed->remoteResources)
    {
        RemoteRef ref;
        ref.raw = reference;
        ref.id = Text(reference, "id");
        ref.type = Text(reference, "type").empty() ? std::string("nam") : Text(reference, "type");
        ref.name = Text(reference, "name");
        ref.category = Text(reference, "category");
        ref.provider = Text(reference, "provider");
        ref.providerKey = Text(reference, "providerKey");
        ref.hash = Text(reference, "hash");
        ref.contentHash =
            IsContentHash(Text(reference, "contentHash")) ? Text(reference, "contentHash") : std::string{};

        if (const auto data = reference.find("providerData"); data != reference.end() && data->is_object())
        {
            ref.toneId = Text(*data, "toneId");
            ref.modelId = Text(*data, "modelId");
            ref.modelUrl = Text(*data, "modelUrl");
        }

        if (ref.modelUrl.empty())
        {
            ref.modelUrl = Text(reference, "modelUrl");
        }

        // The engine's rules (FindRemoteResource): the same id; the same provider and key, or
        // the same Tone3000 tone and model; the same file hash; the same model content hash.
        bool found = false;

        if (const auto it = state.resources.find(ref.type); it != state.resources.end())
        {
            for (const auto& resource : it->second)
            {
                const bool sameProvider =
                    !ref.provider.empty() && Meta(resource, "provider") == ref.provider &&
                    ((!ref.providerKey.empty() && Meta(resource, "providerKey") == ref.providerKey) ||
                     (ref.provider == "tone3000" && !ref.toneId.empty() && Meta(resource, "toneId") == ref.toneId &&
                      Meta(resource, "modelId") == ref.modelId));

                if ((!ref.id.empty() && resource.id == ref.id) || sameProvider ||
                    (!ref.hash.empty() && resource.hash == ref.hash) ||
                    (!ref.contentHash.empty() && Meta(resource, kContentHashMetadataKey) == ref.contentHash))
                {
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            missing.push_back(std::move(ref));
        }
    }

    return missing;
}

nlohmann::json ImportPayloadForRef(const RemoteRef& ref, std::string base64Data)
{
    nlohmann::json metadata = {{"provider", ref.provider.empty() ? std::string("tone3000") : ref.provider},
                               {"providerKey", ref.providerKey.empty() ? ref.modelId : ref.providerKey},
                               {"toneId", ref.toneId},
                               {"modelId", ref.modelId}};

    for (const char* key : {"creatorName", "creatorUrl", "licence", "sourceUrl", "captureScope"})
    {
        if (const auto value = Text(ref.raw, key); !value.empty())
        {
            metadata[key] = value;
        }
    }

    if (!ref.modelUrl.empty())
    {
        metadata["modelUrl"] = ref.modelUrl;
    }

    const auto name = ref.name.empty() ? ref.id : ref.name;
    return {{"provider", "tone3000"},
            {"resourceType", ref.type},
            {"resourceId", ref.id},
            {"name", name},
            {"description", ""},
            {"category", ref.category},
            {"subfolder", "preset-imports"},
            {"fileName", util::SanitizeFilename(name) + (ref.type == "ir" ? ".wav" : ".nam")},
            {"metadata", std::move(metadata)},
            {"data", std::move(base64Data)}};
}
} // namespace guitarfx::uiclient::tone3000
