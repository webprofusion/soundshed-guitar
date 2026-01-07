#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace namguitar
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
    std::string type;     // Effect type (e.g., "nam_amp", "ir_cab", "eq_parametric")
    std::string category; // UI grouping: "amp", "cab", "eq", "dynamics", etc.
    std::string label;    // Optional display name override
    bool enabled = true;  // Bypass toggle

    std::map<std::string, double> params;      // Numeric parameters
    std::map<std::string, std::string> config; // String config

    std::optional<ResourceRef> resource; // For effects needing external files
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
    double inputTrim = 0.0;  // Input gain in dB
    double outputTrim = 0.0; // Master output in dB
    int transpose = 0;       // Pitch shift in semitones
  };

  /**
   * Preset v2 data model with flexible signal graph.
   */
  struct PresetV2
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

} // namespace namguitar
