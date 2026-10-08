#pragma once

/**
 * IrResourceCategory.h — Which slot an IR in the resource library is for.
 *
 * A library resource carries two things that are easy to confuse. Its type ("ir") says
 * what the file is; its category says what it is for. IRs are filed under exactly two
 * categories, "cab" and "reverb", because both the IR Cabinet and the IR Reverb load
 * type "ir" files and the category is the only thing that tells their pickers apart.
 *
 * An import from a picker is filed under that picker's slot. Anything else — Tone3000's
 * gear ("ir", "space", "pedal"), a folder name, "Local" — is mapped when it names a slot
 * unambiguously, and otherwise settled by measuring how long the file rings.
 */

#include <filesystem>
#include <optional>
#include <string>

namespace guitarfx
{
struct LibraryResource;
}

namespace guitarfx::controller_detail
{
inline constexpr const char* kIrCategoryCab = "cab";
inline constexpr const char* kIrCategoryReverb = "reverb";

/// True for the two categories an IR can be filed under.
[[nodiscard]] bool IsIrLibraryCategory(const std::string& category);

/// The IR category a free-form string names unambiguously: "cabinet" is a cab, Tone3000's
/// "space" is a reverb. Tone3000's "ir" names neither, since it covers both.
[[nodiscard]] std::optional<std::string> MapToIrLibraryCategory(const std::string& rawCategory);

/// "reverb" when the file rings like a room, "cab" when it does not, nothing when it
/// cannot be read. Reads the whole file.
[[nodiscard]] std::optional<std::string> ClassifyIrFileCategory(const std::filesystem::path& filePath);

/// The category an IR resource is filed under: the requested one when it names a slot,
/// otherwise the file's measured one, otherwise a guess from the names.
[[nodiscard]] std::string ResolveIrLibraryCategory(const LibraryResource& resource,
                                                   const std::string& requestedCategory);

/// The tag that keeps a grouping an IR's old free-form category carried (a pack or folder
/// name), or nothing when the old value was only a generic label such as "Local" or "ir".
[[nodiscard]] std::optional<std::string> IrCategoryToKeepAsTag(const std::string& oldCategory);
} // namespace guitarfx::controller_detail
