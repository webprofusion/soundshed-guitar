#include "UiBridge.h"

#include <array>
#include <cstdlib>
#include <system_error>

namespace
{
std::filesystem::path NormalizeResourceRoot(const std::filesystem::path& candidate)
{
    if (candidate.empty())
    {
        return {};
    }

    const auto parent = candidate.parent_path();
    const auto grandParent = parent.parent_path();

    const std::array<std::filesystem::path, 7> probes = {candidate,
                                                         candidate / "resources",
                                                         candidate / "Resources",
                                                         parent / "resources",
                                                         parent / "Resources",
                                                         grandParent / "resources",
                                                         grandParent / "Resources"};

    for (const auto& probe : probes)
    {
        if (guitarfx::ui::IsValidResourceRoot(probe))
        {
            return probe;
        }
    }

    return {};
}

std::filesystem::path ResolveEnvOverride()
{
    const char* envValue = std::getenv("SOUNDSHED_RESOURCE_ROOT");
    if (!envValue || envValue[0] == '\0')
    {
        return {};
    }

    const std::filesystem::path envPath(envValue);
    if (const auto normalized = NormalizeResourceRoot(envPath); !normalized.empty())
    {
        return normalized;
    }

    const auto assetsPath = envPath / "assets";
    return NormalizeResourceRoot(assetsPath);
}
} // namespace

namespace guitarfx::ui
{

bool IsValidResourceRoot(const std::filesystem::path& root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        return false;
    }

    const auto indexPath = root / "ui" / "index.html";
    return std::filesystem::exists(indexPath, ec);
}

std::filesystem::path ResolveResourceRoot(const std::vector<std::filesystem::path>& extraCandidates)
{
    if (const auto envPath = ResolveEnvOverride(); !envPath.empty())
    {
        return envPath;
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec)
    {
        if (const auto normalized = NormalizeResourceRoot(cwd); !normalized.empty())
        {
            return normalized;
        }
    }

    for (const auto& candidate : extraCandidates)
    {
        if (const auto normalized = NormalizeResourceRoot(candidate); !normalized.empty())
        {
            return normalized;
        }
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
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += c;
            break;
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
