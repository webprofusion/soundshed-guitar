#pragma once

/**
 * ParamRegistry.h — Generic string-addressed parameter registry.
 *
 * The registry holds static entries for `global.*` and `setlist.*` addresses.
 * `node.*` addresses are resolved lazily by a single prefix handler, not
 * registered per-effect.
 */

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace guitarfx
{

/// A registered automatable parameter.
struct ParamRegistryEntry
{
    std::string address; ///< e.g. "global.inputTrim"
    std::string label;   ///< "Input Trim"
    std::string unit;    ///< "dB", "%", "st", ""
    double minValue = 0.0;
    double maxValue = 1.0;
    bool isStepped = false; ///< Discrete (setlist preset index)
    bool isTrigger = false; ///< Edge-triggered (bank up/down)

    /// Read current native-range value.
    std::function<double()> get;
    /// Write native-range value. alreadyLocked = caller holds mDSPMutex.
    std::function<void(double value, bool alreadyLocked)> apply;
};

/// Serialized form for UI consumption.
struct ParamRegistryInfo
{
    std::string address;
    std::string label;
    std::string unit;
    double minValue = 0.0;
    double maxValue = 1.0;
    bool isStepped = false;
    bool isTrigger = false;
};

class ParamRegistry
{
  public:
    /// Register a static parameter (global.* or setlist.*).
    void Register(const ParamRegistryEntry& entry);

    /// Find a static entry by address. Returns nullptr for node.* addresses.
    [[nodiscard]] const ParamRegistryEntry* Find(const std::string& address) const;

    /// Get all registered static entries (for UI picker).
    [[nodiscard]] std::vector<ParamRegistryInfo> GetAllInfo() const;

    /// Check if an address is a node.* address.
    [[nodiscard]] static bool IsNodeAddress(const std::string& address);

    /// Parse a node.* address into (effectType, paramId). Returns false if not a node address.
    static bool ParseNodeAddress(const std::string& address, std::string& effectType, std::string& paramId);

  private:
    std::unordered_map<std::string, ParamRegistryEntry> mEntries;
};

} // namespace guitarfx
