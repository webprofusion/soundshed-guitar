#include "Base64.h"

#include <array>
#include <cctype>

namespace guitarfx::util
{

std::vector<std::uint8_t> DecodeBase64(const std::string& encoded)
{
    static const std::array<int, 256> decodeTable = []()
    {
        std::array<int, 256> table{};
        table.fill(-1);
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t idx = 0; idx < alphabet.size(); ++idx)
            table[static_cast<unsigned char>(alphabet[idx])] = static_cast<int>(idx);
        table[static_cast<unsigned char>('-')] = 62;
        table[static_cast<unsigned char>('_')] = 63;
        return table;
    }();

    std::vector<std::uint8_t> output;
    int accumulator = 0;
    int bits = -8;

    for (unsigned char c : encoded)
    {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const int value = decodeTable[c];
        if (value < 0) return {};
        accumulator = (accumulator << 6) + value;
        bits += 6;
        if (bits >= 0)
        {
            output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

std::string EncodeBase64(const std::vector<std::uint8_t>& data)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((data.size() + 2) / 3) * 4);

    for (std::size_t i = 0; i < data.size(); i += 3)
    {
        const std::uint32_t octetA = data[i];
        const std::uint32_t octetB = (i + 1) < data.size() ? data[i + 1] : 0;
        const std::uint32_t octetC = (i + 2) < data.size() ? data[i + 2] : 0;
        const std::uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;

        output.push_back(alphabet[(triple >> 18) & 0x3F]);
        output.push_back(alphabet[(triple >> 12) & 0x3F]);
        output.push_back((i + 1) < data.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
        output.push_back((i + 2) < data.size() ? alphabet[triple & 0x3F] : '=');
    }
    return output;
}

} // namespace guitarfx::util
