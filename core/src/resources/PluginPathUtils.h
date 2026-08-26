#pragma once

/**
 * PluginPathUtils — the single source of truth for plugin path shapes and formats.
 *
 * Plugin formats disagree about whether a plugin is a *file* or a *folder*, and
 * the disagreement is per-platform: VST3 is a bundle directory on Linux and
 * macOS and either a bare DLL or a bundle on Windows, LV2 is always a bundle,
 * and Audio Units are bundles that macOS presents to file dialogs as opaque
 * packages. On top of that, not every native dialog lets a directory be selected
 * while a file filter is active, so a user may legitimately arrive at a plugin by
 * picking a file buried inside its bundle.
 *
 * Everything that needs to answer "which plugin is this path?" — the browse
 * dialog, the loader, and the resource library — goes through here, so they
 * cannot drift apart.
 */

#include <filesystem>
#include <string>
#include <string_view>

namespace guitarfx::pluginpath
{

enum class PluginFormat
{
    Unknown,
    VST3,
    AudioUnit,
    LV2,
    CLAP,
    AAX,
    VST2,
};

/// True when the path's final component carries a known plugin-bundle suffix
/// (.vst3, .lv2, .component, .appex, .clap, .aaxplugin, .vst). Purely lexical:
/// the path is never touched on disk, and the entry need not exist.
[[nodiscard]] bool HasPluginBundleSuffix(const std::filesystem::path& path);

/// Normalizes any user selection to the plugin's canonical identity path.
///
/// Returns the nearest ancestor carrying a bundle suffix (or `path` unchanged
/// when none does), except that it keeps climbing past ancestors with the *same*
/// filename. That exception exists for Windows VST3, where the bundle root and
/// its payload share a name — Foo.vst3\Contents\x86_64-win\Foo.vst3 — and the
/// root is what the VST3 spec and JUCE's scanner expect. Requiring the name to
/// match means a container folder that merely ends in a bundle suffix cannot
/// swallow a plugin stored inside it.
///
/// Lexical only: nothing is stat()ed, so this is safe to call on stored paths
/// that point at a disconnected or slow volume.
[[nodiscard]] std::filesystem::path ResolvePluginBundlePath(const std::filesystem::path& path);

/// The format a path names, decided from the bundle root's suffix — or from the
/// file's own extension when the path sits outside any bundle.
[[nodiscard]] PluginFormat PluginFormatFromPath(const std::filesystem::path& path);

/// Stable lower-case id stored alongside a plugin resource ("vst3", "au", "lv2",
/// "clap", "aax", "vst2"). Empty for PluginFormat::Unknown.
[[nodiscard]] std::string_view PluginFormatId(PluginFormat format);

/// Human-readable name including the suffix, e.g. "VST3 (.vst3)".
[[nodiscard]] std::string_view PluginFormatDisplayName(PluginFormat format);

/// Filter patterns for the plugin file picker on this platform: every bundle
/// suffix, plus the payload extensions a user has to fall back to when the
/// native dialog will not let them select the bundle directory itself.
[[nodiscard]] std::string PluginBrowseFilters();

} // namespace guitarfx::pluginpath
