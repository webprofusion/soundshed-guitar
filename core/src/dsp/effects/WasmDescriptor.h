#pragma once

// WasmDescriptor.h - How a Custom Effect describes itself, read: the key=value entries a module
// exports (or a descriptor blob of them, as a node's config keeps them) turned into the
// WasmModuleDescriptor the host and the UI use, held to ABI 2's id-keyed params and resources.
// Internal to WasmEffect.cpp and WasmDescriptor.cpp.

#include "dsp/effects/WasmEffect.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::wasm_descriptor
{
// Appended to every refusal that comes from a module written for the old interface.
inline constexpr const char* kAbiMigrationHint =
    " See \"Moving a module from ABI 1 to ABI 2\" in the Custom Effect authoring guide.";

// Strings a module declares about itself reach the UI and presets; bound their size.
inline constexpr std::size_t kMaxDescriptorTextLength = 1000;

[[nodiscard]] inline bool IsAsciiLetter(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

[[nodiscard]] inline bool IsAsciiDigit(char ch)
{
    return ch >= '0' && ch <= '9';
}

/// The descriptor the entries make, or nothing with the reason in `error`.
[[nodiscard]] std::optional<WasmModuleDescriptor> BuildDescriptorFromEntries(const std::vector<WasmMetadataEntry>& entries,
                                                                             std::string* error);

/// The descriptor a blob of key=value lines makes ('#' comments and blank lines skipped).
[[nodiscard]] std::optional<WasmModuleDescriptor> ParseDescriptorBlob(const std::string& text, std::string* error);
} // namespace guitarfx::wasm_descriptor
