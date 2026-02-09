#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
  /**
   * Reference to a resource (NAM model, IR, etc.)
   *
   * Resources can be referenced in three ways:
   * 1. Library resource: resourceType + resourceId for pre-defined resources
   * 2. Custom file: filePath for user-provided files
   * 3. Embedded: embeddedId for portable preset sharing
   */
  struct ResourceRef
  {
    std::string resourceType; // "nam", "ir", etc.
    std::string resourceId;   // Library ID (e.g., "plexi-bright")
    std::filesystem::path filePath; // Direct file path
    std::string embeddedId;   // References EmbeddedResource.id
    std::string parameterId;  // Mapped parameter identifier (e.g., "gain", "warp")
    std::optional<double> parameterValue; // Captured parameter value for blending
    std::map<std::string, double> parameters; // Multi-parameter mappings for blends

    [[nodiscard]] bool IsLibraryRef() const { return !resourceType.empty() && !resourceId.empty(); }
    [[nodiscard]] bool IsFilePath() const { return !filePath.empty(); }
    [[nodiscard]] bool IsEmbedded() const { return !embeddedId.empty(); }
    [[nodiscard]] bool IsValid() const { return IsLibraryRef() || IsFilePath() || IsEmbedded(); }
  };

  /**
   * Embedded resource for portable preset sharing.
   * Only needed when sharing presets with custom files.
   */
  struct EmbeddedResource
  {
    std::string id;            // Reference ID within this preset
    std::string type;          // "nam", "ir", etc.
    std::string name;          // Display name
    std::string hash;          // SHA-256 for verification
    std::string data;          // Base64-encoded file data (optional)
    std::filesystem::path originalPath; // Original file path
  };

  /**
   * A node in the signal graph.
   */
  struct GraphNode
  {
    std::string id;       // Unique node ID within this graph
    std::string type;     // Effect type (e.g., "amp_nam", "ir_cab", "eq_parametric")
    std::string category; // UI grouping: "amp", "cab", "eq", "dynamics", etc.
    std::string label;    // Optional display name override
    bool enabled = true;  // Bypass toggle

    std::map<std::string, double> params;      // Numeric parameters
    std::map<std::string, std::string> config; // String config

    std::vector<ResourceRef> resources;       // For effects needing multiple external files
  };

  /**
   * An edge connecting two nodes in the signal graph.
   */
  struct GraphEdge
  {
    std::string from;  // Source node ID
    std::string to;    // Destination node ID
    int fromPort = 0;  // Output port index (for splitters)
    int toPort = 0;    // Input port index (for mixers)
    double gain = 1.0; // Edge gain multiplier
  };

  /**
   * Signal graph containing nodes and edges.
   */
  struct SignalGraph
  {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;

    [[nodiscard]] const GraphNode* FindNode(const std::string& id) const
    {
      for (const auto& node : nodes)
      {
        if (node.id == id) return &node;
      }
      return nullptr;
    }

    [[nodiscard]] GraphNode* FindNode(const std::string& id)
    {
      for (auto& node : nodes)
      {
        if (node.id == id) return &node;
      }
      return nullptr;
    }
  };

  /**
   * Global settings for the preset.
   */
  struct GlobalSettings
  {
    double inputTrim = 0.0;   // Input gain in dB
    double outputTrim = 0.0;  // Output trim in dB
    double outputVolume = 1.0; // Output volume (0.0-1.0 linear)
    bool autoLevelInput = false;  // Apply model-referenced input gain if available
    bool autoLevelOutput = false; // Apply model-referenced output trim if available
    int transpose = 0;        // Pitch shift in semitones (-36..+12)
  };

  /**
   * Preset data model with flexible signal graph.
   */
  struct Preset
  {
    // Metadata
    std::string id;
    std::string name;
    std::string author;
    std::string category;
    std::string description;
    int version = 2;
    std::string createdAt;
    std::string modifiedAt;
    std::vector<std::string> tags;

    // Settings
    GlobalSettings global;

    // Signal graph
    SignalGraph graph;

    // Embedded resources (optional, for sharing)
    std::vector<EmbeddedResource> embeddedResources;
  };

  /**
   * Maps a user-facing parameter to an inner node parameter within a composite effect.
   */
  struct ExposedParameter
  {
    std::string paramId;       // User-facing parameter ID (e.g., "drive")
    std::string displayName;   // Label shown in UI
    std::string nodeId;        // Target node ID within the inner graph
    std::string nodeParamKey;  // Parameter key on the target node

    double minValue = 0.0;     // Override min range
    double maxValue = 1.0;     // Override max range
    double defaultValue = 0.0; // Override default value
    std::string unit;          // Display unit (e.g., "dB", "Hz", "ms")
    std::string curve = "linear"; // Mapping curve: "linear", "log", "exp"
  };

  /**
   * Defines a composite effect — a reusable mini signal path that appears
   * as a single node in the parent graph. Contains an inner graph of effects
   * with only selected parameters exposed to the user.
   */
  struct CompositeEffectDefinition
  {
    std::string id;            // Unique identifier (e.g., "composite-vintage-marshall")
    std::string name;          // Display name
    std::string category;      // Effect category for UI grouping (e.g., "channel", "amp")
    std::string description;   // User-facing description
    std::string author;        // Creator name
    std::vector<std::string> tags; // Searchable tags
    int version = 1;           // Definition schema version

    SignalGraph innerGraph;    // The mini signal graph of inner effects
    std::vector<ExposedParameter> exposedParams; // Parameters surfaced to the user

    // Optional layout JSON stored as a string (parsed by UI)
    std::string layoutJson;

    std::string createdAt;     // ISO 8601 timestamp
    std::string modifiedAt;    // Last modification timestamp

    /**
     * Get the effect type ID used to register this composite with the EffectRegistry.
     * Format: "composite:{id}"
     */
    [[nodiscard]] std::string GetEffectTypeId() const
    {
      return "composite:" + id;
    }

    /**
     * Check if this definition has a valid inner graph.
     */
    [[nodiscard]] bool IsValid() const
    {
      return !id.empty() && !name.empty() && !innerGraph.nodes.empty();
    }
  };

  // Reserved node types for routing
  constexpr const char* kNodeTypeInput = "input";
  constexpr const char* kNodeTypeOutput = "output";
  constexpr const char* kNodeTypeSplitter = "splitter";
  constexpr const char* kNodeTypeMixer = "mixer";
  constexpr const char* kNodeTypeCompositePrefix = "composite:";

  /**
   * Global signal chain configuration.
   * Developer-only: defines global effects applied before and after preset processing.
   * 
   * Signal flow:
   *   Input → [Pre-chain: Tuner tap → Noise Gate → Transpose] → 
   *   [Preset Mixer] → [Post-chain: EQ → Doubler] → Output
   */
  struct GlobalSignalChainConfig
  {
    // Signal graph definitions for global pre/post chains (preferred over legacy param blocks)
    SignalGraph preChainGraph;
    SignalGraph postChainGraph;

    // Input stage settings
    double inputGain = 0.0;          // dB
    bool monoMode = false;
    int inputChannel = 0;            // 0=left, 1=right (when mono)
    bool autoLevelInput = false;

    // Output stage settings
    double outputGain = 0.0;         // dB (master volume)
    bool autoLevelOutput = false;
    bool limiterEnabled = false;

    /**
     * Build a SignalGraph for the pre-chain (input → gate → transpose → output).
     */
    [[nodiscard]] SignalGraph BuildPreChainGraph() const;

    /**
     * Build a SignalGraph for the post-chain (input → eq → doubler → output).
     */
    [[nodiscard]] SignalGraph BuildPostChainGraph() const;

    /**
     * Build default graphs for the global pre/post chain.
     */
    [[nodiscard]] static SignalGraph BuildDefaultPreChainGraph();
    [[nodiscard]] static SignalGraph BuildDefaultPostChainGraph();

    /**
     * Create default global chain configuration.
     */
    [[nodiscard]] static GlobalSignalChainConfig CreateDefault();
  };

} // namespace guitarfx
