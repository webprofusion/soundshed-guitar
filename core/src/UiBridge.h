#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::ui
{

[[nodiscard]] bool IsValidResourceRoot(const std::filesystem::path& root);
[[nodiscard]] std::filesystem::path ResolveResourceRoot(
    const std::vector<std::filesystem::path>& extraCandidates = {});

[[nodiscard]] std::string EscapeForJavascript(std::string_view input);
[[nodiscard]] std::string BuildIPlugReceiveScript(std::string_view jsonMessage);

} // namespace guitarfx::ui
