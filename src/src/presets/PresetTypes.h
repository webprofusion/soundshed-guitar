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

    std::optional<ResourceRef> resource;      // For effects needing a single external file
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

  // Reserved node types for routing
  constexpr const char* kNodeTypeInput = "input";
  constexpr const char* kNodeTypeOutput = "output";
  constexpr const char* kNodeTypeSplitter = "splitter";
  constexpr const char* kNodeTypeMixer = "mixer";

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
    // Pre-chain settings (applied before preset processing)
    struct PreChain
    {
      // Noise Gate (dynamics_gate)
      bool gateEnabled = false;
      double gateThreshold = -40.0;  // dB
      double gateAttack = 0.5;       // ms
      double gateHold = 50.0;        // ms
      double gateRelease = 100.0;    // ms

      // Transpose (transpose)
      bool transposeEnabled = false;
      int transposeSemitones = 0;    // -36 to +12
    };

    // Post-chain settings (applied after preset mixing)
    struct PostChain
    {
      // Parametric EQ (eq_parametric)
      bool eqEnabled = false;
      double eqLowGain = 0.0;        // dB
      double eqLowFreq = 100.0;      // Hz
      double eqLowMidGain = 0.0;     // dB
      double eqLowMidFreq = 400.0;   // Hz
      double eqLowMidQ = 1.0;
      double eqHighMidGain = 0.0;    // dB
      double eqHighMidFreq = 2000.0; // Hz
      double eqHighMidQ = 1.0;
      double eqHighGain = 0.0;       // dB
      double eqHighFreq = 8000.0;    // Hz

      // Doubler (delay_doubler)
      bool doublerEnabled = false;
      double doublerDelay = 20.0;    // ms
      double doublerMix = 0.5;       // 0-1
      double doublerDetune = 5.0;    // cents
    };

    // Input stage settings
    double inputGain = 0.0;          // dB
    bool monoMode = false;
    int inputChannel = 0;            // 0=left, 1=right (when mono)
    bool autoLevelInput = false;

    // Output stage settings
    double outputGain = 0.0;         // dB (master volume)
    bool autoLevelOutput = false;
    bool limiterEnabled = false;

    PreChain preChain;
    PostChain postChain;

    /**
     * Build a SignalGraph for the pre-chain (input → gate → transpose → output).
     */
    [[nodiscard]] SignalGraph BuildPreChainGraph() const;

    /**
     * Build a SignalGraph for the post-chain (input → eq → doubler → output).
     */
    [[nodiscard]] SignalGraph BuildPostChainGraph() const;

    /**
     * Create default global chain configuration.
     */
    [[nodiscard]] static GlobalSignalChainConfig CreateDefault();
  };

} // namespace guitarfx
