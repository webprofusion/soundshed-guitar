#include "uiclient/ResourceIndex.h"

#include "uiclient/UiCommands.h"

#include <algorithm>
#include <cctype>

namespace guitarfx::uiclient
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// normalizeFilterValue: trimmed, inner runs of space made one, lower case.
std::string NormalizeFilterValue(const std::string& value)
{
    std::string out;
    bool space = false;

    for (const char c : Trim(value))
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            space = true;
            continue;
        }

        if (space && !out.empty())
        {
            out += ' ';
        }

        space = false;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return out;
}

std::string MetaString(const LibraryResource& r, const char* key)
{
    const auto it = r.metadata.find(key);
    return it != r.metadata.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// The first of these metadata fields that is set (as the TS `??` chain of strings, where an
/// empty string still counts as set).
std::string FirstMeta(const LibraryResource& r, std::initializer_list<const char*> keys)
{
    for (const auto* key : keys)
    {
        const auto it = r.metadata.find(key);

        if (it != r.metadata.end() && it->is_string())
        {
            return it->get<std::string>();
        }

        if (it != r.metadata.end() && it->is_number())
        {
            return it->dump();
        }
    }

    return {};
}

std::string NormalizeArchitectureBadge(const std::string& raw)
{
    const auto value = Lower(Trim(raw));

    if (value == "2" || value == "a2")
    {
        return "A2";
    }

    if (value == "1" || value == "a1")
    {
        return "A1";
    }

    if (value == "custom")
    {
        return "Custom";
    }

    if (value.find("slimmable") != std::string::npos)
    {
        return "A2";
    }

    if (value.find("wavenet") != std::string::npos)
    {
        return "A1";
    }

    return {};
}

/// isCanonicalCandidate: whether `candidate` should stand for its group over `current`.
bool IsCanonicalCandidate(const LibraryResource& candidate, const LibraryResource& current)
{
    if (candidate.fileMissing != current.fileMissing)
    {
        return !candidate.fileMissing;
    }

    const auto name = [](const LibraryResource& r) { return Lower(Trim(r.name.empty() ? r.id : r.name)); };

    if (name(candidate) == name(current))
    {
        const auto a = ResourceArchitecture(candidate), b = ResourceArchitecture(current);

        if (a != b)
        {
            if (a == "A2" && b == "A1")
            {
                return true;
            }

            if (a == "A1" && b == "A2")
            {
                return false;
            }
        }
    }

    const auto richness = [](const LibraryResource& r) { return r.tags.size() + r.description.size(); };

    if (richness(candidate) != richness(current))
    {
        return richness(candidate) > richness(current);
    }

    return candidate.id < current.id;
}

/// localeCompare, near enough: case-insensitive first, then exact.
int CompareText(const std::string& a, const std::string& b)
{
    const auto la = Lower(a), lb = Lower(b);

    if (la != lb)
    {
        return la < lb ? -1 : 1;
    }

    return a == b ? 0 : (a < b ? -1 : 1);
}
} // namespace

ResourceDedup DeduplicateResources(const std::vector<LibraryResource>& resources,
                                   const std::set<std::string>& preferredIds)
{
    ResourceDedup result;
    std::vector<std::string> order;
    std::map<std::string, std::vector<const LibraryResource*>> groups;

    for (const auto& r : resources)
    {
        std::string key;

        if (!r.hash.empty())
        {
            key = "hash:" + Lower(r.hash);
        }
        else
        {
            auto path = Lower(r.filePath);
            std::replace(path.begin(), path.end(), '\\', '/');
            key = "path:" + path;
        }

        auto& group = groups[key];

        if (group.empty())
        {
            order.push_back(key);
        }

        group.push_back(&r);
    }

    for (const auto& key : order)
    {
        const auto& group = groups[key];
        std::vector<const LibraryResource*> pool;

        for (const auto* r : group)
        {
            if (preferredIds.count(r->id) > 0)
            {
                pool.push_back(r);
            }
        }

        if (pool.empty())
        {
            pool = group;
        }

        const LibraryResource* canonical = pool.front();

        for (std::size_t i = 1; i < pool.size(); ++i)
        {
            if (IsCanonicalCandidate(*pool[i], *canonical))
            {
                canonical = pool[i];
            }
        }

        result.deduped.push_back(canonical);

        for (const auto* r : group)
        {
            if (r->id != canonical->id)
            {
                result.aliasById[r->id] = canonical->id;
            }
        }
    }

    return result;
}

std::string ResourceArchitecture(const LibraryResource& resource)
{
    return NormalizeArchitectureBadge(
        FirstMeta(resource, {"architectureVersion", "architecture_version", "architecture"}));
}

std::vector<std::string> ResourceTags(const LibraryResource& resource)
{
    std::vector<std::string> tags;
    const auto add = [&tags](const std::string& raw) {
        const auto tag = Trim(raw);

        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end())
        {
            tags.push_back(tag);
        }
    };

    for (const auto& t : resource.tags)
    {
        add(t);
    }

    // splitTagValues: commas or semicolons.
    const auto meta = MetaString(resource, "tags");
    std::string part;

    for (const char c : meta)
    {
        if (c == ',' || c == ';')
        {
            add(part);
            part.clear();
        }
        else
        {
            part += c;
        }
    }

    add(part);
    return tags;
}

std::string ResourceCreator(const LibraryResource& resource)
{
    return Trim(FirstMeta(resource, {"creatorName", "authorUsername", "modeledBy", "creator", "provider"}));
}

std::string FormatGearDescription(const std::string& make, const std::string& model)
{
    const auto gearMake = Trim(make);
    const auto gearModel = Trim(model);
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };

    if (!gearMake.empty() && lower(gearModel).rfind(lower(gearMake), 0) == 0)
    {
        return gearModel;
    }

    return gearMake.empty() ? gearModel : gearModel.empty() ? gearMake : gearMake + " " + gearModel;
}

std::string ResourceMetaLine(const LibraryResource& resource, const std::string& resourceType, bool withCategory)
{
    std::vector<std::string> parts;

    if (withCategory)
    {
        const auto category = Trim(resource.category);
        parts.push_back(category.empty() ? std::string("Uncategorized") : category);
    }

    if (const auto creator = ResourceCreator(resource); !creator.empty())
    {
        parts.push_back("by " + creator);
    }

    if (resourceType == "nam")
    {
        if (auto arch = ResourceArchitecture(resource); !arch.empty())
        {
            parts.push_back(std::move(arch));
        }
    }

    if (auto gear = FormatGearDescription(FirstMeta(resource, {"gearMake"}), FirstMeta(resource, {"gearModel"}));
        !gear.empty())
    {
        parts.push_back(std::move(gear));
    }

    auto toneType = Trim(FirstMeta(resource, {"toneType"}));
    std::replace(toneType.begin(), toneType.end(), '_', ' ');

    if (!toneType.empty())
    {
        parts.push_back(std::move(toneType));
    }

    std::string line;

    for (const auto& p : parts)
    {
        line += (line.empty() ? "" : " \xC2\xB7 ") + p;
    }

    return line;
}

ResourceFacets LibraryFacets(const std::vector<const LibraryResource*>& resources)
{
    std::set<std::string> tags, creators;

    for (const auto* r : resources)
    {
        for (const auto& t : ResourceTags(*r))
        {
            tags.insert(t);
        }

        if (auto creator = ResourceCreator(*r); !creator.empty())
        {
            creators.insert(std::move(creator));
        }
    }

    ResourceFacets facets{{tags.begin(), tags.end()}, {creators.begin(), creators.end()}};
    const auto byText = [](const std::string& a, const std::string& b) { return CompareText(a, b) < 0; };
    std::sort(facets.tags.begin(), facets.tags.end(), byText);
    std::sort(facets.creators.begin(), facets.creators.end(), byText);
    return facets;
}

bool IsResourceFavorite(const ClientState& state, const std::string& resourceId)
{
    const auto it = state.appSettings.find("resources.favorites");

    if (it == state.appSettings.end() || !it->is_array())
    {
        return false;
    }

    return std::find(it->begin(), it->end(), nlohmann::json(resourceId)) != it->end();
}

void SetResourceFavorite(UiCommands& commands, const std::string& resourceId, bool favorite)
{
    commands.SetResourceFavorite(resourceId, favorite);
}

std::vector<const LibraryResource*> FilterLibrary(const ClientState& state, const std::string& resourceType,
                                                  const ResourceFilter& filter, const std::string& selectedId,
                                                  std::map<std::string, std::string>* aliases)
{
    const auto it = state.resources.find(resourceType);

    if (it == state.resources.end())
    {
        return {};
    }

    std::set<std::string> preferred;

    if (!selectedId.empty())
    {
        preferred.insert(selectedId);
    }

    auto dedup = DeduplicateResources(it->second, preferred);

    if (aliases != nullptr)
    {
        *aliases = dedup.aliasById;
    }

    const auto query = Lower(Trim(filter.query));
    const auto creator = NormalizeFilterValue(filter.creator);
    std::vector<std::string> wantedTags;

    for (const auto& t : filter.tags)
    {
        wantedTags.push_back(NormalizeFilterValue(t));
    }

    std::vector<const LibraryResource*> list;

    for (const auto* r : dedup.deduped)
    {
        if (r->fileMissing)
        {
            continue;
        }

        if (filter.category != "all")
        {
            const auto category = Trim(r->category).empty() ? std::string("Uncategorized") : Trim(r->category);

            if (category != filter.category)
            {
                continue;
            }
        }

        if (resourceType == "nam" && filter.architecture != "all" && ResourceArchitecture(*r) != filter.architecture)
        {
            continue;
        }

        if (filter.creator != "all" && NormalizeFilterValue(ResourceCreator(*r)) != creator)
        {
            continue;
        }

        if (!wantedTags.empty())
        {
            std::vector<std::string> tags;

            for (const auto& t : ResourceTags(*r))
            {
                tags.push_back(NormalizeFilterValue(t));
            }

            if (!std::all_of(wantedTags.begin(), wantedTags.end(), [&tags](const std::string& t) {
                    return std::find(tags.begin(), tags.end(), t) != tags.end();
                }))
            {
                continue;
            }
        }

        if (filter.favouritesOnly && !IsResourceFavorite(state, r->id))
        {
            continue;
        }

        if (!query.empty())
        {
            std::string haystack =
                r->name + " " + r->id + " " + r->category + " " + r->description + " " + ResourceCreator(*r);

            for (const auto& t : ResourceTags(*r))
            {
                haystack += " " + t;
            }

            if (Lower(haystack).find(query) == std::string::npos)
            {
                continue;
            }
        }

        list.push_back(r);
    }

    std::stable_sort(list.begin(), list.end(), [&state](const LibraryResource* a, const LibraryResource* b) {
        const bool fa = IsResourceFavorite(state, a->id), fb = IsResourceFavorite(state, b->id);

        if (fa != fb)
        {
            return fa;
        }

        const int byName = CompareText(a->name.empty() ? a->id : a->name, b->name.empty() ? b->id : b->name);

        if (byName != 0)
        {
            return byName < 0;
        }

        return CompareText(a->filePath, b->filePath) < 0;
    });
    return list;
}

std::vector<std::string> LibraryCategories(const ClientState& state, const std::string& resourceType)
{
    std::set<std::string> categories;

    if (const auto it = state.resources.find(resourceType); it != state.resources.end())
    {
        for (const auto& r : it->second)
        {
            categories.insert(Trim(r.category).empty() ? std::string("Uncategorized") : Trim(r.category));
        }
    }

    std::vector<std::string> list(categories.begin(), categories.end());
    std::sort(list.begin(), list.end(),
              [](const std::string& a, const std::string& b) { return CompareText(a, b) < 0; });
    return list;
}

std::optional<std::string> ResolveLibraryCategoryFromHint(const std::string& hint,
                                                          const std::vector<std::string>& available)
{
    auto wanted = Trim(hint);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    wanted = wanted == "ir" ? std::string("cab") : wanted;

    if (wanted.empty())
    {
        return std::nullopt;
    }

    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };

    for (const auto& category : available)
    {
        if (lower(category) == wanted)
        {
            return category;
        }
    }

    for (const auto& category : available)
    {
        const auto name = lower(category);
        const auto has = [&name](const char* word) { return name.find(word) != std::string::npos; };

        if ((wanted == "pedal" && has("pedal")) ||
            (wanted == "full-rig" && (has("full-rig") || has("full rig") || has("fullrig"))) ||
            (wanted == "amp" && has("amp") && !has("full")) || (wanted == "cab" && has("cab")) ||
            (wanted == "reverb" && has("reverb")))
        {
            return category;
        }
    }

    return std::nullopt;
}

std::optional<LibraryPack> LibraryPackOf(const LibraryResource& resource)
{
    auto toneId = Trim(MetaString(resource, "toneId"));

    if (toneId.empty())
    {
        toneId = Trim(MetaString(resource, "groupId"));
    }

    if (const auto it = resource.metadata.find("toneId");
        toneId.empty() && it != resource.metadata.end() && it->is_number())
    {
        toneId = it->dump();
    }

    if (!toneId.empty())
    {
        auto title = Trim(MetaString(resource, "toneTitle"));

        if (title.empty())
        {
            title = Trim(MetaString(resource, "groupName"));
        }

        return LibraryPack{"tone:" + toneId, title.empty() ? "Tone3000 pack " + toneId : title};
    }

    const auto archive = Trim(MetaString(resource, "factoryArchiveKey"));

    if (!archive.empty())
    {
        std::string title;
        bool gap = false;

        for (const char c : archive)
        {
            if (c == '-' || c == '_')
            {
                gap = true;
                continue;
            }

            if (gap)
            {
                title += ' ';
            }

            gap = false;
            title += c;
        }

        return LibraryPack{"archive:" + archive, title};
    }

    return std::nullopt;
}

std::string NameWithinPack(const std::string& name, const LibraryPack& pack)
{
    if (pack.title.empty() || Lower(name).rfind(Lower(pack.title), 0) != 0)
    {
        return name;
    }

    auto rest = name.substr(pack.title.size());
    // Separators after the title: space, - _ | : , and the middle dot and en dash (UTF-8).
    std::size_t i = 0;

    while (i < rest.size())
    {
        const char c = rest[i];

        if (c == ' ' || c == '\t' || c == '-' || c == '_' || c == '|' || c == ':' || c == ',')
        {
            ++i;
            continue;
        }

        if (rest.compare(i, 2, "\xC2\xB7") == 0)
        {
            i += 2;
            continue;
        }

        if (rest.compare(i, 3, "\xE2\x80\x93") == 0)
        {
            i += 3;
            continue;
        }

        break;
    }

    rest = Trim(rest.substr(i));
    return rest.empty() ? name : rest;
}

std::vector<LibraryEntry> GroupLibraryByPack(const ClientState& state,
                                             const std::vector<const LibraryResource*>& resources)
{
    std::vector<LibraryEntry> entries;
    std::map<std::string, std::size_t> byKey;

    for (const auto* r : resources)
    {
        auto pack = LibraryPackOf(*r);

        if (pack)
        {
            if (const auto it = byKey.find(pack->key); it != byKey.end())
            {
                entries[it->second].items.push_back(r);
                continue;
            }
        }

        if (pack)
        {
            byKey[pack->key] = entries.size();
        }

        entries.push_back({std::move(pack), {r}});
    }

    for (auto& e : entries)
    {
        if (e.items.size() < 2)
        {
            e.pack.reset();
        }
    }

    const auto label = [](const LibraryEntry& e) {
        return e.pack ? e.pack->title : (e.items.front()->name.empty() ? e.items.front()->id : e.items.front()->name);
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const LibraryEntry& a, const LibraryEntry& b) {
        const auto fav = [&state](const LibraryEntry& e) {
            return std::any_of(e.items.begin(), e.items.end(),
                               [&state](const LibraryResource* r) { return IsResourceFavorite(state, r->id); });
        };
        const bool fa = fav(a), fb = fav(b);

        if (fa != fb)
        {
            return fa;
        }

        return CompareText(label(a), label(b)) < 0;
    });
    return entries;
}

bool IsPackGroupingEnabled(const ClientState& state)
{
    const auto it = state.appSettings.find("resources.library.groupPacks");
    return it == state.appSettings.end() || !it->is_boolean() || it->get<bool>();
}

std::vector<const LibraryResource*> NavigationOrder(const ClientState& state,
                                                    const std::vector<const LibraryResource*>& resources)
{
    if (!IsPackGroupingEnabled(state))
    {
        return resources;
    }

    std::vector<const LibraryResource*> order;

    for (const auto& entry : GroupLibraryByPack(state, resources))
    {
        order.insert(order.end(), entry.items.begin(), entry.items.end());
    }

    return order;
}
} // namespace guitarfx::uiclient
