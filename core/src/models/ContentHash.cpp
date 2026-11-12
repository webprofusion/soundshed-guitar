#include "models/ContentHash.h"

#include "dsp/FiniteCheck.h"
#include "util/JsonSkimmer.h"
#include "util/LongPath.h"
#include "util/Sha256.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <istream>
#include <iterator>
#include <memory>
#include <vector>

namespace guitarfx
{
namespace
{
using util::JsonSkimmer;

constexpr std::string_view kNamTag = "nam-content-v1";
constexpr std::string_view kIrTag = "ir-content-v1";

/// A JSON number token as a double, the way Python's json module reads it: correctly
/// rounded, an exponent too large overflowing to infinity and one too small to zero.
std::optional<double> ParseJsonNumber(const std::string& token)
{
    if (token.empty())
    {
        return std::nullopt;
    }

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double value = 0.0;
    const char* const end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(token.data(), end, value, std::chars_format::general);

    if (ptr != end)
    {
        return std::nullopt;
    }

    if (ec == std::errc::result_out_of_range)
    {
        // Too large is infinity, which no weight may be; too small rounds to zero.
        const auto exponent = token.find_first_of("eE");
        const bool tiny = exponent != std::string::npos && exponent + 1 < token.size() && token[exponent + 1] == '-';
        return tiny ? std::optional<double>{0.0} : std::optional<double>{HUGE_VAL};
    }

    if (ec != std::errc{})
    {
        return std::nullopt;
    }

    return value;
#else
    // strtod follows LC_NUMERIC, which a host may have set to a comma locale; JSON writes '.'.
    std::string text = token;
    const char decimalPoint = *std::localeconv()->decimal_point;

    if (decimalPoint != '.')
    {
        std::replace(text.begin(), text.end(), '.', decimalPoint);
    }

    char* parsedEnd = nullptr;
    const double value = std::strtod(text.c_str(), &parsedEnd);

    if (parsedEnd != text.c_str() + text.size())
    {
        return std::nullopt;
    }

    return value;
#endif
}

/// What the hash needs of one model object, read in whatever order its members come: the
/// architecture may follow the config and the weights, so every model keeps both until the
/// walk knows which one counts.
struct ModelNode
{
    std::string architecture;

    /// Python's `config or {}`: a missing or falsy config is an empty one, anything else that
    /// is not an object makes the model unhashable.
    bool configInvalid = false;

    struct Submodel
    {
        bool isObject = false;
        bool hasNumericMaxValue = false;
        double maxValue = 0.0;
        std::unique_ptr<ModelNode> model; ///< Null when "model" is missing or not an object.
    };

    enum class ListState
    {
        Missing,
        NotAList,
        List
    };

    ListState submodelsState = ListState::Missing;
    std::vector<Submodel> submodels;

    ListState modelsState = ListState::Missing;
    std::vector<std::unique_ptr<ModelNode>> models; ///< Null entries for non-objects.

    ListState weightsState = ListState::Missing;
    bool weightsValid = true;              ///< Every element a number that is a finite float32.
    std::vector<std::uint32_t> weightBits; ///< float32 bit patterns, -0 already +0.
};

class NamModelReader
{
  public:
    explicit NamModelReader(std::istream& in) : mJson(in)
    {
    }

    bool ReadRoot(ModelNode& node)
    {
        return mJson.PeekValue() == '{' && ReadModel(node);
    }

  private:
    bool ReadModel(ModelNode& node)
    {
        return mJson.ForEachMember([&](const std::string& key) {
            if (key == "architecture")
            {
                // A non-string architecture names no container, so the model is a leaf.
                return mJson.ReadString(node.architecture);
            }

            if (key == "config")
            {
                return ReadConfig(node);
            }

            if (key == "weights")
            {
                return ReadWeights(node);
            }

            return mJson.SkipValue();
        });
    }

    bool ReadConfig(ModelNode& node)
    {
        node.configInvalid = false;
        node.submodelsState = ModelNode::ListState::Missing;
        node.submodels.clear();
        node.modelsState = ModelNode::ListState::Missing;
        node.models.clear();

        const int first = mJson.PeekValue();

        if (first == '{')
        {
            return mJson.ForEachMember([&](const std::string& key) {
                if (key == "submodels")
                {
                    return ReadSubmodels(node);
                }

                if (key == "models")
                {
                    return ReadModels(node);
                }

                return mJson.SkipValue();
            });
        }

        // Falsy in Python: null, false, 0, "" and []. Anything else is not an object.
        if (first == 'n' || first == 'f')
        {
            return mJson.SkipValue();
        }

        if (first == '[')
        {
            std::size_t count = 0;
            const bool ok = mJson.ForEachElement([&]() {
                ++count;
                return mJson.SkipValue();
            });
            node.configInvalid = count > 0;
            return ok;
        }

        if (first == '"')
        {
            std::string text;
            const bool ok = mJson.ReadString(text);
            node.configInvalid = !text.empty();
            return ok;
        }

        if (first == '-' || (first >= '0' && first <= '9'))
        {
            const bool ok = mJson.ReadNumber(mToken);
            const auto value = ParseJsonNumber(mToken);
            node.configInvalid = !value.has_value() || *value != 0.0;
            return ok;
        }

        node.configInvalid = true;
        return mJson.SkipValue();
    }

    bool ReadSubmodels(ModelNode& node)
    {
        node.submodels.clear();

        if (mJson.PeekValue() != '[')
        {
            node.submodelsState = ModelNode::ListState::NotAList;
            return mJson.SkipValue();
        }

        node.submodelsState = ModelNode::ListState::List;
        return mJson.ForEachElement([&]() {
            auto& submodel = node.submodels.emplace_back();

            if (mJson.PeekValue() != '{')
            {
                return mJson.SkipValue();
            }

            submodel.isObject = true;
            return mJson.ForEachMember([&](const std::string& key) {
                if (key == "max_value")
                {
                    submodel.hasNumericMaxValue = false;
                    const int first = mJson.PeekValue();

                    if (first != '-' && (first < '0' || first > '9'))
                    {
                        return mJson.SkipValue();
                    }

                    const bool ok = mJson.ReadNumber(mToken);

                    if (const auto value = ParseJsonNumber(mToken))
                    {
                        submodel.hasNumericMaxValue = true;
                        submodel.maxValue = *value;
                    }

                    return ok;
                }

                if (key == "model")
                {
                    submodel.model.reset();

                    if (mJson.PeekValue() != '{')
                    {
                        return mJson.SkipValue();
                    }

                    submodel.model = std::make_unique<ModelNode>();
                    return ReadModel(*submodel.model);
                }

                return mJson.SkipValue();
            });
        });
    }

    bool ReadModels(ModelNode& node)
    {
        node.models.clear();

        if (mJson.PeekValue() != '[')
        {
            node.modelsState = ModelNode::ListState::NotAList;
            return mJson.SkipValue();
        }

        node.modelsState = ModelNode::ListState::List;
        return mJson.ForEachElement([&]() {
            auto& child = node.models.emplace_back();

            if (mJson.PeekValue() != '{')
            {
                return mJson.SkipValue();
            }

            child = std::make_unique<ModelNode>();
            return ReadModel(*child);
        });
    }

    bool ReadWeights(ModelNode& node)
    {
        node.weightBits.clear();
        node.weightsValid = true;

        if (mJson.PeekValue() != '[')
        {
            node.weightsState = ModelNode::ListState::NotAList;
            return mJson.SkipValue();
        }

        node.weightsState = ModelNode::ListState::List;
        return mJson.ForEachElement([&]() {
            const int first = mJson.PeekValue();

            // true, false, null, strings and the NaN/Infinity a Python writer allows are
            // not finite numbers.
            if (first != '-' && (first < '0' || first > '9'))
            {
                node.weightsValid = false;
                return mJson.SkipValue();
            }

            if (!mJson.ReadNumber(mToken))
            {
                return false;
            }

            if (!node.weightsValid)
            {
                return true;
            }

            const auto value = ParseJsonNumber(mToken);

            if (!value || !IsFinite(*value))
            {
                node.weightsValid = false;
                return true;
            }

            const float single = static_cast<float>(*value);

            if (!IsFinite(single))
            {
                node.weightsValid = false;
                return true;
            }

            std::uint32_t bits = std::bit_cast<std::uint32_t>(single);

            if ((bits & 0x7fffffffu) == 0)
            {
                bits = 0; // -0 hashes as +0
            }

            node.weightBits.push_back(bits);
            return true;
        });
    }

    JsonSkimmer mJson;
    std::string mToken;
};

/// The leaves of a model in hashing order, or false when the model is unhashable.
bool CollectLeaves(const ModelNode& node, std::vector<const ModelNode*>& leaves)
{
    if (node.configInvalid)
    {
        return false;
    }

    if (node.architecture == "SlimmableContainer")
    {
        if (node.submodelsState != ModelNode::ListState::List || node.submodels.empty())
        {
            return false;
        }

        std::vector<const ModelNode::Submodel*> ordered;

        for (const auto& submodel : node.submodels)
        {
            if (!submodel.isObject || !submodel.hasNumericMaxValue)
            {
                return false;
            }

            ordered.push_back(&submodel);
        }

        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto* a, const auto* b) { return a->maxValue < b->maxValue; });

        for (std::size_t i = 1; i < ordered.size(); ++i)
        {
            if (ordered[i]->maxValue == ordered[i - 1]->maxValue)
            {
                return false;
            }
        }

        for (const auto* submodel : ordered)
        {
            if (!submodel->model || !CollectLeaves(*submodel->model, leaves))
            {
                return false;
            }
        }

        return true;
    }

    if (node.architecture == "Sequential")
    {
        if (node.modelsState != ModelNode::ListState::List || node.models.empty())
        {
            return false;
        }

        for (const auto& child : node.models)
        {
            if (!child || !CollectLeaves(*child, leaves))
            {
                return false;
            }
        }

        return true;
    }

    if (node.weightsState != ModelNode::ListState::List || !node.weightsValid || node.weightBits.empty())
    {
        return false;
    }

    leaves.push_back(&node);
    return true;
}

void AppendLittleEndian(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

std::string Tagged(std::string_view tag, std::span<const std::uint8_t> message)
{
    std::string result{tag};
    result += ':';
    result += util::Sha256Hex(message);
    return result;
}

std::uint32_t ReadU32Le(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[offset + 2]) << 16 | static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

bool FourCC(std::span<const std::uint8_t> bytes, std::size_t offset, const char* id)
{
    return std::memcmp(bytes.data() + offset, id, 4) == 0;
}
} // namespace

bool IsContentHash(std::string_view value)
{
    std::string_view hex;

    if (value.starts_with("nam-content-v1:"))
    {
        hex = value.substr(15);
    }
    else if (value.starts_with("ir-content-v1:"))
    {
        hex = value.substr(14);
    }
    else
    {
        return false;
    }

    return hex.size() == 64 &&
           std::all_of(hex.begin(), hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<std::string> NamWeightsHash(std::istream& in)
{
    ModelNode root;

    try
    {
        NamModelReader reader(in);

        if (!reader.ReadRoot(root))
        {
            return std::nullopt;
        }
    }
    catch (...)
    {
        return std::nullopt;
    }

    std::vector<const ModelNode*> leaves;

    if (!CollectLeaves(root, leaves))
    {
        return std::nullopt;
    }

    std::size_t size = kNamTag.size() + 1;

    for (const auto* leaf : leaves)
    {
        size += 8 + 4 * leaf->weightBits.size();
    }

    std::vector<std::uint8_t> message;
    message.reserve(size);
    message.insert(message.end(), kNamTag.begin(), kNamTag.end());
    message.push_back(0);

    for (const auto* leaf : leaves)
    {
        AppendLittleEndian(message, leaf->weightBits.size(), 8);

        for (const std::uint32_t bits : leaf->weightBits)
        {
            AppendLittleEndian(message, bits, 4);
        }
    }

    return Tagged(kNamTag, message);
}

std::optional<std::string> IrContentHash(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 12 || !FourCC(bytes, 0, "RIFF") || !FourCC(bytes, 8, "WAVE"))
    {
        return std::nullopt;
    }

    std::size_t offset = 12;

    while (offset + 8 <= bytes.size())
    {
        const std::uint32_t chunkSize = ReadU32Le(bytes, offset + 4);
        const std::size_t start = offset + 8;

        if (FourCC(bytes, offset, "data"))
        {
            // A chunk that claims more than the file holds (a recorder that never wrote the
            // size back) is read to the end of the file.
            const std::size_t length = std::min<std::size_t>(chunkSize, bytes.size() - start);
            std::vector<std::uint8_t> message;
            message.reserve(kIrTag.size() + 1 + length);
            message.insert(message.end(), kIrTag.begin(), kIrTag.end());
            message.push_back(0);
            message.insert(message.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start),
                           bytes.begin() + static_cast<std::ptrdiff_t>(start + length));
            return Tagged(kIrTag, message);
        }

        // An odd-sized chunk is followed by a zero pad byte, but some writers leave it out, so
        // it is skipped only when it is zero (as IRWavLoader does): no chunk id starts with 0.
        offset = start + static_cast<std::size_t>(chunkSize);

        if ((chunkSize & 1u) != 0 && offset < bytes.size() && bytes[offset] == 0)
        {
            ++offset;
        }
    }

    return std::nullopt;
}

std::optional<std::string> ContentHashOfFile(const std::string& resourceType, const std::filesystem::path& filePath)
{
    try
    {
        if (resourceType == "nam")
        {
            std::ifstream file(util::LongPathSafe(filePath), std::ios::binary);
            return file ? NamWeightsHash(file) : std::nullopt;
        }

        if (resourceType == "ir")
        {
            std::ifstream file(util::LongPathSafe(filePath), std::ios::binary);

            if (!file)
            {
                return std::nullopt;
            }

            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                                  std::istreambuf_iterator<char>()};
            return IrContentHash(bytes);
        }
    }
    catch (...)
    {
    }

    return std::nullopt;
}
} // namespace guitarfx
