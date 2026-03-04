#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
  class EffectProcessor;

  /**
   * Parameter definition for UI rendering.
   */
  struct ParameterDef
  {
    std::string id;
    std::string displayName;
    double defaultValue = 0.0;
    double minValue = 0.0;
    double maxValue = 1.0;
    std::string unit; // "dB", "Hz", "ms", "%", etc.
    std::string group;
    bool advanced = false;
    double step = 0.0; // Step size for integer/discrete params (0 = continuous)
    std::vector<std::string> labels; // Enum labels for discrete params (e.g. {"Off","On"})
  };

  /**
   * Describes an available effect type.
   *
   * Type ID convention: UUID v4 string (e.g. "2eb53b40-6139-4696-8820-387ac56ffa91").
   * All canonical IDs are declared in EffectGuids.h.  Human-readable legacy strings
   * (e.g. "amp_nam") are kept in EffectTypeInfo::aliases so that existing presets
   * continue to load.  Never reuse or reassign a UUID once it has been shipped.
   * Recognised categories: amp, cab, eq, dynamics, dist, mod, delay, reverb, pitch, utility, synth
   */
  struct EffectTypeInfo
  {
    std::string type;        // Canonical UUID type ID (see EffectGuids.h)
    std::string displayName; // Human-readable name
    std::string category;    // "amp", "cab", "eq", "dynamics", "dist", "mod", "delay", "reverb", "pitch", "utility", "synth"
    std::string description; // User-facing description
    bool requiresResource = false;
    bool requiresTempo = false; // If true, the effect receives current BPM via SetParam("bpm", bpm) each audio block
    std::string resourceType; // "nam", "ir", etc. (if requiresResource is true)
    std::vector<std::string> resourceFilterHint; // Equipment type filter ("amp", "full-rig", "pedal", etc.)
    std::vector<ParameterDef> parameters;
    // Legacy IDs that resolve to this effect (for preset backward-compatibility).
    // Populated by RegisterAlias() or set directly before calling Register().
    std::vector<std::string> aliases;
  };

  /**
   * Factory function for creating effect processors.
   */
  using EffectFactory = std::function<std::unique_ptr<EffectProcessor>()>;

  /**
   * Registry for effect types.
   * Effects register themselves at startup.
   */
  class EffectRegistry
  {
  public:
    static EffectRegistry &Instance();

    // Registration
    void Register(const std::string &type, const EffectTypeInfo &info, EffectFactory factory);
    void Unregister(const std::string &type);

    // Resolve a type ID or legacy alias to the canonical type ID. Returns the input unchanged if not an alias.
    [[nodiscard]] std::string Resolve(const std::string &type) const;

    // Factory
    [[nodiscard]] std::unique_ptr<EffectProcessor> Create(const std::string &type) const;

    // Queries
    [[nodiscard]] std::vector<EffectTypeInfo> GetAllTypes() const;
    [[nodiscard]] std::vector<EffectTypeInfo> GetTypesByCategory(const std::string &category) const;
    [[nodiscard]] std::vector<std::string> GetCategories() const;
    [[nodiscard]] bool HasType(const std::string &type) const;
    [[nodiscard]] std::optional<EffectTypeInfo> GetTypeInfo(const std::string &type) const;

  private:
    EffectRegistry() = default;

    std::map<std::string, EffectTypeInfo> mTypeInfo;
    std::map<std::string, EffectFactory> mFactories;
    // Maps legacy/alias IDs → canonical type ID
    std::map<std::string, std::string> mAliases;
  };

/**
 * Helper macro for effect registration.
 */
#define REGISTER_EFFECT(TypeClass, typeId, displayName, category, description, requiresResource)           \
  namespace                                                                                                \
  {                                                                                                        \
    struct TypeClass##Registrar                                                                            \
    {                                                                                                      \
      TypeClass##Registrar()                                                                               \
      {                                                                                                    \
        EffectTypeInfo info;                                                                               \
        info.type = typeId;                                                                                \
        info.displayName = displayName;                                                                    \
        info.category = category;                                                                          \
        info.description = description;                                                                    \
        info.requiresResource = requiresResource;                                                          \
        EffectRegistry::Instance().Register(typeId, info, []() { return std::make_unique<TypeClass>(); }); \
      }                                                                                                    \
    } g##TypeClass##Registrar;                                                                             \
  }

} // namespace guitarfx
