/**
 * ParamRegistry.cpp — Generic string-addressed parameter registry implementation.
 */

#include "ParamRegistry.h"

#include <algorithm>

namespace guitarfx
{
void ParamRegistry::Register(const ParamRegistryEntry& entry)
{
    mEntries[entry.address] = entry;
}

const ParamRegistryEntry* ParamRegistry::Find(const std::string& address) const
{
    const auto it = mEntries.find(address);
    return it != mEntries.end() ? &it->second : nullptr;
}

std::vector<ParamRegistryInfo> ParamRegistry::GetAllInfo() const
{
    std::vector<ParamRegistryInfo> result;
    result.reserve(mEntries.size());

    for (const auto& [addr, entry] : mEntries)
    {
        ParamRegistryInfo info;
        info.address = entry.address;
        info.label = entry.label;
        info.unit = entry.unit;
        info.minValue = entry.minValue;
        info.maxValue = entry.maxValue;
        info.isStepped = entry.isStepped;
        info.isTrigger = entry.isTrigger;
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
    return result;
}

bool ParamRegistry::IsNodeAddress(const std::string& address)
{
    return address.size() > 5 && address.substr(0, 5) == "node.";
}

bool ParamRegistry::ParseNodeAddress(const std::string& address, std::string& effectType, std::string& paramId)
{
    if (!IsNodeAddress(address))
    {
        return false;
    }

    // Format: node.<effectType>.<paramId>
    const auto firstDot = address.find('.', 5);

    if (firstDot == std::string::npos)
    {
        return false;
    }

    effectType = address.substr(5, firstDot - 5);
    paramId = address.substr(firstDot + 1);
    return !effectType.empty() && !paramId.empty();
}
} // namespace guitarfx
