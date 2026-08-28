#include "controller/internal/LayoutSupport.h"

#include "util/FileIO.h"
#include "util/FileSystem.h"
#include "util/PathSanitizer.h"

#include <fstream>

namespace guitarfx::controller_detail
{
std::filesystem::path ResolveEffectLayoutsSettingsPath(const FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / "layouts" / "indexes" / "effect-layouts.json";
}

nlohmann::json LoadEffectLayoutsSettings(const FileSystem& fileSystem)
{
    const auto path = ResolveEffectLayoutsSettingsPath(fileSystem);
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    root["associations"] = nlohmann::json::object();

    try
    {
        if (path.empty() || !std::filesystem::exists(path))
        {
            return root;
        }

        std::ifstream input(path);

        if (!input)
        {
            return root;
        }

        nlohmann::json parsed;
        input >> parsed;

        if (!parsed.is_object())
        {
            return root;
        }

        if (!parsed.contains("associations") || !parsed["associations"].is_object())
        {
            parsed["associations"] = nlohmann::json::object();
        }

        if (!parsed.contains("version") || !parsed["version"].is_number())
        {
            parsed["version"] = 1;
        }

        return parsed;
    }
    catch (...)
    {
        return root;
    }
}

void SaveEffectLayoutsSettings(const FileSystem& fileSystem, const nlohmann::json& root)
{
    const auto path = ResolveEffectLayoutsSettingsPath(fileSystem);

    if (path.empty())
    {
        return;
    }

    try
    {
        const auto dir = path.parent_path();
        [[maybe_unused]] const auto ensured = fileSystem.EnsureDirectory(dir);
        std::ofstream output(path);

        if (output)
        {
            output << root.dump(2);
        }
    }
    catch (...)
    {
    }
}

std::filesystem::path ResolveLayoutDir(const FileSystem& fileSystem, const std::string& layoutId)
{
    const auto settingsDir = fileSystem.ResolveSettingsDirectory();
    const std::string safeStem = util::SanitizeFilename(layoutId);
    return settingsDir / "layouts" / "content" / safeStem;
}

std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId)
{
    return ResolveLayoutDir(fileSystem, layoutId) / "layout.json";
}
} // namespace guitarfx::controller_detail
