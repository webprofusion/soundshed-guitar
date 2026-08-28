#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "presets/PresetTypes.h"

#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace guitarfx
{
class ResourceLibrary;

/**
 * Composite effect processor — wraps a mini signal graph and exposes
 * selected parameters to the parent graph. Internally delegates to a
 * nested SignalGraphExecutor.
 *
 * Processing flow:
 *   Parent graph → CompositeEffectProcessor::Process()
 *     → inner SignalGraphExecutor processes sub-graph
 *     → output returned to parent graph
 *
 * Parameter routing:
 *   SetParam("drive", 5.0) → inner executor → SetNodeParam("amp", "inputGain", 5.0)
 */
class CompositeEffectProcessor : public EffectProcessor
{
  public:
    explicit CompositeEffectProcessor(const CompositeEffectDefinition& definition);
    ~CompositeEffectProcessor() override = default;

    // EffectProcessor interface
    void Prepare(double sampleRate, int maxBlockSize) override;
    void Reset() override;
    void Process(float** inputs, float** outputs, int numSamples) override;

    void SetParam(const std::string& key, double value) override;
    void SetConfig(const std::string& key, const std::string& value) override;
    [[nodiscard]] double GetParam(const std::string& key) const override;
    [[nodiscard]] std::string GetConfig(const std::string& key) const override;
    bool LoadResources(const std::vector<ResourceRef>& refs, const std::vector<std::filesystem::path>& paths) override;

    [[nodiscard]] std::string GetType() const override;
    [[nodiscard]] std::string GetCategory() const override;

    [[nodiscard]] bool RequiresResource() const override
    {
        return false;
    }

    [[nodiscard]] bool HasResource() const override
    {
        return true;
    }

    // A composite is only as thread-safe to load and prepare as the graph it wraps: if it
    // contains a plugin host, the parent must keep it on the main thread rather than
    // dispatching it to a worker.
    [[nodiscard]] bool RequiresMainThreadLoad() const noexcept override
    {
        return mInnerExecutor.AnyNodeRequiresMainThreadLoad();
    }

    /// Set the resource library for inner resource resolution.
    void SetResourceLibrary(ResourceLibrary* library);

    /// Get the composite definition.
    [[nodiscard]] const CompositeEffectDefinition& GetDefinition() const
    {
        return mDefinition;
    }

    /// Get the inner executor for diagnostics.
    [[nodiscard]] const SignalGraphExecutor& GetInnerExecutor() const
    {
        return mInnerExecutor;
    }

    /// Forward a per-instance node-type config default into the wrapped graph.
    /// SetConfig() does not route into inner nodes, so settings that apply to a node
    /// *type* (NAM quality) need this explicit path to reach nodes inside a composite.
    void SetInnerNodeTypeConfigDefault(const std::string& type, const std::string& key, const std::string& value)
    {
        mInnerExecutor.SetNodeTypeConfigDefault(type, key, value);
    }

    /// Seed the wrapped graph with a whole set of type defaults, before it is built.
    void SeedInnerNodeTypeConfigDefaults(const std::map<std::string, std::map<std::string, std::string>>& defaults)
    {
        mInnerExecutor.SeedNodeTypeConfigDefaults(defaults);
    }

  private:
    /// Build the exposed parameter lookup map.
    void BuildParamMap();
    void ApplyPendingResourceOverrides();

    struct PendingResourceOverride
    {
        std::string nodeId;
        ResourceRef ref;
    };

    CompositeEffectDefinition mDefinition;
    SignalGraphExecutor mInnerExecutor;

    /// Map from exposed paramId → ExposedParameter for fast lookup.
    std::unordered_map<std::string, const ExposedParameter*> mParamMap;
    std::vector<PendingResourceOverride> mPendingResourceOverrides;
    bool mInnerPrepared = false;
};

/**
 * Library of composite effect definitions.
 * Manages loading, saving, and registration of composite effects.
 */
class CompositeEffectLibrary
{
  public:
    CompositeEffectLibrary() = default;
    ~CompositeEffectLibrary() = default;

    /// Add a definition and register it with the EffectRegistry.
    void AddDefinition(const CompositeEffectDefinition& def);

    /// Remove a definition and unregister it.
    void RemoveDefinition(const std::string& id);

    /// Get a definition by its ID.
    [[nodiscard]] const CompositeEffectDefinition* GetDefinition(const std::string& id) const;

    /// Get all definitions.
    [[nodiscard]] const std::vector<CompositeEffectDefinition>& GetAllDefinitions() const
    {
        return mDefinitions;
    }

    /// Check if a definition exists.
    [[nodiscard]] bool HasDefinition(const std::string& id) const;

    /// Register all loaded definitions with the EffectRegistry.
    void RegisterAll();

    /// Set the resource library pointer (shared with the main executor).
    void SetResourceLibrary(ResourceLibrary* library)
    {
        mResourceLibrary = library;
    }

    /// Load definitions from a directory (factory + user).
    void LoadFromDirectory(const std::filesystem::path& dir);

    /// Save a definition to the user directory.
    bool SaveDefinition(const CompositeEffectDefinition& def, const std::filesystem::path& userDir);

    /// Delete a definition file from the user directory.
    bool DeleteDefinition(const std::string& id, const std::filesystem::path& userDir);

  private:
    /// Register a single definition with the EffectRegistry.
    void RegisterDefinition(const CompositeEffectDefinition& def);

    /// Unregister a definition from the EffectRegistry.
    void UnregisterDefinition(const std::string& id);

    std::vector<CompositeEffectDefinition> mDefinitions;
    ResourceLibrary* mResourceLibrary = nullptr;
};

} // namespace guitarfx
