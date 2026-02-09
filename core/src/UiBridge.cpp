#include "UiBridge.h"

#include <cstdlib>
#include <system_error>

namespace
{
    std::filesystem::path ResolveEnvOverride()
    {
        const char* envValue = std::getenv("SOUNDSHED_RESOURCE_ROOT");
        if (!envValue || envValue[0] == '\0')
            return {};

        const std::filesystem::path envPath(envValue);
        if (guitarfx::ui::IsValidResourceRoot(envPath))
            return envPath;

        const auto assetsPath = envPath / "assets";
        if (guitarfx::ui::IsValidResourceRoot(assetsPath))
            return assetsPath;

        const auto resourcesPath = envPath / "resources";
        if (guitarfx::ui::IsValidResourceRoot(resourcesPath))
            return resourcesPath;

        return {};
    }
}

namespace guitarfx::ui
{

bool IsValidResourceRoot(const std::filesystem::path& root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return false;

    const auto indexPath = root / "ui" / "index.html";
    return std::filesystem::exists(indexPath, ec);
}

std::filesystem::path ResolveResourceRoot(
    const std::vector<std::filesystem::path>& extraCandidates)
{
    if (const auto envPath = ResolveEnvOverride(); !envPath.empty())
        return envPath;

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec)
    {
        const auto resourcesPath = cwd / "resources";
        if (IsValidResourceRoot(resourcesPath))
            return resourcesPath;
    }

    for (const auto& candidate : extraCandidates)
    {
        if (IsValidResourceRoot(candidate))
            return candidate;
    }

    return {};
}

std::string EscapeForJavascript(std::string_view input)
{
    std::string escaped;
    escaped.reserve(input.size() + 8);
    for (const char c : input)
    {
        switch (c)
        {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string BuildIPlugReceiveScript(std::string_view jsonMessage)
{
    std::string script = "if (window.IPlugReceiveData) { window.IPlugReceiveData(\"";
    script += EscapeForJavascript(jsonMessage);
    script += "\"); }";
    return script;
}

} // namespace guitarfx::ui
