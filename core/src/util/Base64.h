#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace guitarfx::util
{

[[nodiscard]] std::vector<std::uint8_t> DecodeBase64(const std::string& encoded);
[[nodiscard]] std::string EncodeBase64(const std::vector<std::uint8_t>& data);

} // namespace guitarfx::util
