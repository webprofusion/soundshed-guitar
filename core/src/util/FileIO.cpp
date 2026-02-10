#include "FileIO.h"

#include <fstream>

namespace guitarfx::util
{

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0)
        return {};

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

} // namespace guitarfx::util
