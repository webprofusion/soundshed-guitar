#include "resources/PluginPathUtils.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace guitarfx::pluginpath
{
namespace
{
struct FormatSuffix
{
    std::string_view suffix;
    PluginFormat format;
    /// Whether this suffix marks a *bundle* (a directory on at least one
    /// platform) rather than a plain plugin file.
    bool isBundle;
};

/// The one table of "which suffix means which format". Formats this host
/// cannot load (.clap, .aaxplugin, VST2) are listed deliberately: recognizing
/// them lets the loader say "CLAP plugins are not supported, use the VST3
/// version" instead of a confusing "no plugin found". Recognizing a format is
/// separate from offering it in the file picker — see IsOfferedInPicker.
///
/// Note ".vst" only ever matches VST2 — ".vst3" ends in '3', so a suffix
/// comparison against ".vst" cannot match it.
constexpr std::array<FormatSuffix, 8> kFormatSuffixes{{
    {".vst3", PluginFormat::VST3, true},
    {".component", PluginFormat::AudioUnit, true},
    {".appex", PluginFormat::AudioUnit, true},
    {".lv2", PluginFormat::LV2, true},
    {".clap", PluginFormat::CLAP, true},
    {".aaxplugin", PluginFormat::AAX, true},
    {".vst", PluginFormat::VST2, true},
    {".dll", PluginFormat::VST2, false},
}};

/// Extensions of the files *inside* a bundle. A user has to pick one of these
/// whenever the native dialog will not let them select the bundle directory,
/// so the picker has to offer them; ResolvePluginBundlePath maps the result
/// back to the bundle root.
#if defined(__APPLE__)
constexpr std::array<std::string_view, 2> kPayloadExtensions{".dylib", ".ttl"};
#elif defined(_WIN32)
constexpr std::array<std::string_view, 2> kPayloadExtensions{".dll", ".ttl"};
#else
constexpr std::array<std::string_view, 2> kPayloadExtensions{".so", ".ttl"};
#endif

/// Whether the browse dialog lists this format.
///
/// The picker offers what this host can load, plus VST2 — the one unhostable
/// format still worth showing, because selecting one reaches the loader and
/// earns "install the VST3 version instead", which is the advice the user
/// needs. CLAP and AAX are left out: offering them only invites a selection
/// that can never work.
///
/// Offering is separate from recognizing. PluginFormatFromPath still
/// classifies every format here, so a .clap or .aaxplugin path arriving from
/// a preset or an existing library entry still earns a specific message
/// rather than a vague scan failure.
///
/// Listed exhaustively rather than as exclusions so that adding a format
/// fails the switch and forces the decision.
constexpr bool IsOfferedInPicker(PluginFormat format)
{
    switch (format)
    {
    case PluginFormat::VST3:
    case PluginFormat::AudioUnit:
    case PluginFormat::LV2:
    case PluginFormat::VST2:
        return true;

    case PluginFormat::CLAP:
    case PluginFormat::AAX:
    case PluginFormat::Unknown:
        break;
    }
    return false;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

/// Drops trailing separators so "Foo.vst3/" reports a filename of "Foo.vst3"
/// rather than an empty one.
std::filesystem::path StripTrailingSeparator(const std::filesystem::path& path)
{
    if (!path.has_filename() && path.has_parent_path())
    {
        auto parent = path.parent_path();
        if (parent != path)
        {
            return parent;
        }
    }
    return path;
}

const FormatSuffix* MatchSuffix(const std::string& lowerName, bool bundlesOnly)
{
    for (const auto& entry : kFormatSuffixes)
    {
        if (bundlesOnly && !entry.isBundle)
        {
            continue;
        }
        if (lowerName.size() > entry.suffix.size() &&
            lowerName.compare(lowerName.size() - entry.suffix.size(), entry.suffix.size(), entry.suffix) == 0)
        {
            return &entry;
        }
    }
    return nullptr;
}

std::string LowerFilename(const std::filesystem::path& path)
{
    return ToLowerAscii(StripTrailingSeparator(path).filename().string());
}
} // namespace

bool HasPluginBundleSuffix(const std::filesystem::path& path)
{
    return MatchSuffix(LowerFilename(path), /*bundlesOnly=*/true) != nullptr;
}

std::filesystem::path ResolvePluginBundlePath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return path;
    }

    const auto trimmed = StripTrailingSeparator(path);

    // Nearest ancestor (starting with the path itself) that looks like a bundle.
    std::filesystem::path nearest;
    for (auto current = trimmed;;)
    {
        if (MatchSuffix(ToLowerAscii(current.filename().string()), /*bundlesOnly=*/true) != nullptr)
        {
            nearest = current;
            break;
        }

        auto parent = current.parent_path();
        if (parent.empty() || parent == current)
        {
            break;
        }
        current = std::move(parent);
    }

    if (nearest.empty())
    {
        return trimmed;
    }

    // Keep climbing only past ancestors that repeat the same name, which is the
    // Windows VST3 layout (Foo.vst3\Contents\x86_64-win\Foo.vst3). Anything else
    // stops here, so "Container.vst3\Foo.vst3" resolves to Foo.vst3.
    const std::string nearestName = ToLowerAscii(nearest.filename().string());
    std::filesystem::path outermost = nearest;
    for (auto current = nearest.parent_path(); !current.empty();)
    {
        if (ToLowerAscii(current.filename().string()) == nearestName)
        {
            outermost = current;
        }

        auto parent = current.parent_path();
        if (parent.empty() || parent == current)
        {
            break;
        }
        current = std::move(parent);
    }

    return outermost;
}

PluginFormat PluginFormatFromPath(const std::filesystem::path& path)
{
    const auto root = ResolvePluginBundlePath(path);
    const auto* match = MatchSuffix(LowerFilename(root), /*bundlesOnly=*/false);
    return match != nullptr ? match->format : PluginFormat::Unknown;
}

std::string_view PluginFormatId(PluginFormat format)
{
    switch (format)
    {
    case PluginFormat::VST3:
        return "vst3";
    case PluginFormat::AudioUnit:
        return "au";
    case PluginFormat::LV2:
        return "lv2";
    case PluginFormat::CLAP:
        return "clap";
    case PluginFormat::AAX:
        return "aax";
    case PluginFormat::VST2:
        return "vst2";
    case PluginFormat::Unknown:
        break;
    }
    return {};
}

std::string_view PluginFormatDisplayName(PluginFormat format)
{
    switch (format)
    {
    case PluginFormat::VST3:
        return "VST3 (.vst3)";
    case PluginFormat::AudioUnit:
        return "Audio Unit (.component)";
    case PluginFormat::LV2:
        return "LV2 (.lv2)";
    case PluginFormat::CLAP:
        return "CLAP (.clap)";
    case PluginFormat::AAX:
        return "AAX (.aaxplugin)";
    case PluginFormat::VST2:
        return "VST2 (.dll/.vst)";
    case PluginFormat::Unknown:
        break;
    }
    return {};
}

std::string PluginBrowseFilters()
{
    std::string filters;
    const auto append = [&filters](std::string_view suffix) {
        if (!filters.empty())
        {
            filters += ';';
        }
        filters += '*';
        filters += suffix;
    };

    for (const auto& entry : kFormatSuffixes)
    {
        if (entry.isBundle && IsOfferedInPicker(entry.format))
        {
            append(entry.suffix);
        }
    }
    for (const auto extension : kPayloadExtensions)
    {
        append(extension);
    }

    return filters;
}

} // namespace guitarfx::pluginpath
