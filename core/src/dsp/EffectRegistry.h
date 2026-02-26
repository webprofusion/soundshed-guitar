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
  };

  /**
   * Describes an available effect type.
   */
  struct EffectTypeInfo
  {
    std::string type;        // Unique type ID
    std::string displayName; // Human-readable name
    std::string category;    // "amp", "cab", "eq", etc.
    std::string description; // User-facing description
    bool requiresResource = false;
    std::string resourceType; // "nam", "ir", etc. (if requiresResource is true)
    std::vector<std::string> resourceFilterHint; // Equipment type filter ("amp", "full-rig", "pedal", etc.)
    std::vector<ParameterDef> parameters;
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
