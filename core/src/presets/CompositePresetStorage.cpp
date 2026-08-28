#include "presets/CompositePresetStorage.h"
#include "util/PathSanitizer.h"

#include <fstream>
#include <stdexcept>

namespace guitarfx
{
std::string CompositePresetStorage::SerializeToJson(const CompositePreset& cp)
{
    const nlohmann::json j = cp;
    return j.dump(2);
}

std::optional<CompositePreset> CompositePresetStorage::DeserializeFromJson(const std::string& json)
{
    try
    {
        const auto j = nlohmann::json::parse(json);
        return j.get<CompositePreset>();
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// Returns the safe file path for a composite preset ID, or empty path if the ID is invalid.
static std::filesystem::path SafeFilePath(const std::string& id, const std::filesystem::path& dir)
{
    if (id.empty())
    {
        return {};
    }

    const std::string safe = guitarfx::util::SanitizeFilename(id);

    if (safe.empty())
    {
        return {};
    }

    // Guard against path traversal: the sanitized name must not introduce separators.
    if (safe.find('/') != std::string::npos || safe.find('\\') != std::string::npos ||
        safe.find("..") != std::string::npos)
    {
        return {};
    }

    return dir / (safe + CompositePresetStorage::kExtension);
}

bool CompositePresetStorage::SaveToFile(const CompositePreset& cp, const std::filesystem::path& dir)
{
    try
    {
        std::filesystem::create_directories(dir);
        const auto path = SafeFilePath(cp.id, dir);

        if (path.empty())
        {
            return false;
        }

        std::ofstream ofs(path, std::ios::out | std::ios::trunc);

        if (!ofs.is_open())
        {
            return false;
        }

        ofs << SerializeToJson(cp);
        return ofs.good();
    }
    catch (...)
    {
        return false;
    }
}

std::optional<CompositePreset> CompositePresetStorage::LoadById(const std::string& id, const std::filesystem::path& dir)
{
    const auto path = SafeFilePath(id, dir);

    if (path.empty() || !std::filesystem::exists(path))
    {
        return std::nullopt;
    }

    try
    {
        std::ifstream ifs(path);

        if (!ifs.is_open())
        {
            return std::nullopt;
        }

        const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        return DeserializeFromJson(content);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::vector<CompositePreset> CompositePresetStorage::ListAll(const std::filesystem::path& dir)
{
    std::vector<CompositePreset> result;

    if (!std::filesystem::exists(dir))
    {
        return result;
    }

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            if (entry.path().extension().string() != ".json")
            {
                continue;
            }

            const std::string stem = entry.path().stem().string();

            // Must end with ".composite" (stem of "id.composite.json" is "id.composite")
            if (stem.size() < 10 || stem.substr(stem.size() - 10) != ".composite")
            {
                continue;
            }

            std::ifstream ifs(entry.path());

            if (!ifs.is_open())
            {
                continue;
            }

            const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            auto cp = DeserializeFromJson(content);

            if (cp)
            {
                result.push_back(std::move(*cp));
            }
        }
    }
    catch (...)
    {
    }

    return result;
}

bool CompositePresetStorage::DeleteById(const std::string& id, const std::filesystem::path& dir)
{
    const auto path = SafeFilePath(id, dir);

    if (path.empty() || !std::filesystem::exists(path))
    {
        return false;
    }

    try
    {
        return std::filesystem::remove(path);
    }
    catch (...)
    {
        return false;
    }
}

bool CompositePresetStorage::SaveToStore(storage::JsonStore& store, const CompositePreset& cp)
{
    if (cp.id.empty())
    {
        return false;
    }

    return store.PutRaw(storage::ItemType::kCompositePreset, cp.id, SerializeToJson(cp));
}

std::optional<CompositePreset> CompositePresetStorage::LoadFromStore(const storage::JsonStore& store,
                                                                     const std::string& id)
{
    const auto raw = store.GetRaw(storage::ItemType::kCompositePreset, id);

    if (!raw)
    {
        return std::nullopt;
    }

    return DeserializeFromJson(*raw);
}

std::vector<CompositePreset> CompositePresetStorage::ListAllFromStore(const storage::JsonStore& store)
{
    std::vector<CompositePreset> result;
    const auto items = store.List(storage::ItemType::kCompositePreset);
    result.reserve(items.size());

    for (const auto& item : items)
    {
        if (auto cp = DeserializeFromJson(item.json))
        {
            result.push_back(std::move(*cp));
        }
    }

    return result;
}

bool CompositePresetStorage::DeleteFromStore(storage::JsonStore& store, const std::string& id)
{
    return store.Remove(storage::ItemType::kCompositePreset, id);
}
} // namespace guitarfx
