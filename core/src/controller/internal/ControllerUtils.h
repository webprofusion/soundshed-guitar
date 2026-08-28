#pragma once

/**
 * ControllerUtils.h — Small helpers shared across the controller's
 * translation units.
 *
 * These are the pieces that no single feature owns: unit conversions, id and
 * timestamp generation, graph queries, and the rules for what may be written
 * into a debug snapshot. Anything specific to one feature belongs in that
 * feature's support header instead.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class FileSystem;
enum class BrowseFileType;
struct GraphNode;
struct SignalGraph;
struct GlobalSignalChainConfig;
} // namespace guitarfx

namespace guitarfx::controller_detail
{

// ── Storage / provider identifiers ──────────────────────────────────

inline constexpr const char* kLocalResourceProvider = "local";
inline constexpr const char* kLocalResourceStorageFolder = "local";
inline constexpr const char* kSessionLogFileName = "logs/session-log.txt";
inline constexpr const char* kDebugSnapshotFileName = "logs/debug-state.json";
inline constexpr const char* kSharedSyncStateDocumentId = "shared-sync-state";

/// How often a second instance re-reads state another instance may have written.
inline constexpr auto kSharedSyncPollInterval = std::chrono::milliseconds(2000);

// ── UI push rates ───────────────────────────────────────────────────
//
// Each of these trades display update rate against IPC overhead.

/// Maximum DSP performance stats messages sent to the UI per second.
inline constexpr int kDspPerformanceStatsRateHz = 5;
inline constexpr int kSignalDiagnosticsRateHz = 20;

/// Fast enough that a moving spatialiser puck looks continuous, slow enough to
/// be negligible.
inline constexpr int kSpatialPositionRateHz = 20;

/// A practice-tool progress readout does not need more than this.
inline constexpr int kPracticeToolRateHz = 12;

/// Floor used when converting to dB, so silence maps to a finite value.
inline constexpr double kMinLinear = 1e-6;

// ── Debug snapshots ─────────────────────────────────────────────────

/// True for keys whose values must never reach a debug snapshot.
[[nodiscard]] bool IsSensitiveDebugKey(std::string_view key);

/// Recursively replaces sensitive values in `value` with a redaction marker.
void ScrubSensitiveJson(nlohmann::json& value, std::string_view currentKey = {});

[[nodiscard]] std::filesystem::path ResolveDebugSnapshotPath(const FileSystem& fileSystem);

// ── Values ──────────────────────────────────────────────────────────

[[nodiscard]] double ToDbFS(double linear);
[[nodiscard]] double LinearFromDb(double db);
[[nodiscard]] double ClampValue(double value, double minimum, double maximum);

/// Bars covered by `frameCount` at the given tempo, rounded up, minimum 1.
[[nodiscard]] int ComputeBarsFromFrames(std::size_t frameCount, double sampleRate, double tempoBpm, int timeSigNum,
                                        int timeSigDen);

// ── Ids and timestamps ──────────────────────────────────────────────

[[nodiscard]] std::string GenerateGuidV4String();
[[nodiscard]] std::string GenerateUserPresetId();

/// UTC timestamp in ISO-8601 ("YYYY-MM-DDThh:mm:ssZ"), thread-safe.
[[nodiscard]] std::string BuildUtcIsoTimestamp();

/// Local-time timestamp ("YYYY-MM-DD hh:mm:ss") for human-read log lines.
[[nodiscard]] std::string FormatTimestamp();

/// Short stable digest of a value, for correlating log lines without printing
/// the value itself.
[[nodiscard]] std::string HashStringForLog(std::string_view value);

// ── Paths and files ─────────────────────────────────────────────────

/// Maps a resource type onto a native file-dialog category.
[[nodiscard]] BrowseFileType ResolveBrowseFileType(const std::string& resourceType);

[[nodiscard]] bool ShouldHashResourceFile(const std::filesystem::path& path);
[[nodiscard]] std::string InferPluginFormatFromPath(const std::filesystem::path& path);

/// True when a path contains ".." or a root component, i.e. could escape the
/// directory it is meant to stay inside.
[[nodiscard]] bool HasUnsafeRelativeSegments(const std::filesystem::path& path);

void SaveJsonFile(const FileSystem& fileSystem, const std::filesystem::path& path, const nlohmann::json& payload);

// ── Graph queries ───────────────────────────────────────────────────

[[nodiscard]] const GraphNode* FindNodeByIdOrType(const SignalGraph& graph, const std::string& id,
                                                  const std::string& type);

[[nodiscard]] int GetGlobalTransposeFromChainConfig(const GlobalSignalChainConfig& config);
[[nodiscard]] nlohmann::json SerializeGlobalFxSettings(const GlobalSignalChainConfig& config);

/// Appends a numeric suffix to `baseId` until it is unique within `graph`.
[[nodiscard]] std::string MakeUniqueNodeId(const SignalGraph& graph, const std::string& baseId);

/// Graphs must be acyclic before they reach Process().
[[nodiscard]] bool IsGraphAcyclic(const SignalGraph& graph);

} // namespace guitarfx::controller_detail
