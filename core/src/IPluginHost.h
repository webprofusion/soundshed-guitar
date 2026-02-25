#pragma once

/**
 * IPluginHost — Abstract interface between the shared PluginController
 * and the framework-specific plugin host (iPlug2, JUCE, etc.).
 *
 * Framework adapters implement this interface to provide:
 *   - WebView message transport
 *   - File dialogs
 *   - Thread bouncing (audio → main thread)
 *   - Platform storage paths
 *   - DAW integration hooks
 *
 * The PluginController and MessageDispatcher call through this interface
 * so they never reference iPlug2 or JUCE types directly.
 */

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace guitarfx
{

/// Categories for file browse dialogs.
enum class BrowseFileType
{
    NAMModel,
    IRFile,
    PresetFile,
    ImageFile,
    AudioFile,
    ArchiveFile,
    Any
};

/// Result of a file browse operation.
struct BrowseFileResult
{
    bool success = false;
    std::filesystem::path path;
};

/**
 * Interface that framework-specific plugin hosts must implement.
 * One instance per plugin instance; owned by the framework adapter.
 */
class IPluginHost
{
public:
    virtual ~IPluginHost() = default;

    // ── WebView message transport ──────────────────────────────────
    /// Send a JSON message string to the WebView UI.
    virtual void SendMessageToUI(const std::string& jsonMessage) = 0;

    // ── File dialogs ───────────────────────────────────────────────
    /// Open a native file browser asynchronously. Callback fires on main thread.
    virtual void BrowseFileAsync(BrowseFileType type,
                                 const std::string& title,
                                 std::function<void(const BrowseFileResult&)> callback) = 0;

    /// Open a native save-file dialog asynchronously.
    virtual void SaveFileAsync(BrowseFileType type,
                               const std::string& title,
                               const std::string& defaultName,
                               std::function<void(const BrowseFileResult&)> callback) = 0;

    // ── Threading ──────────────────────────────────────────────────
    /// Run a function on the main/UI thread. Safe to call from the audio thread.
    virtual void RunOnMainThread(std::function<void()> fn) = 0;

    // ── Platform paths ─────────────────────────────────────────────
    /// Root directory for user data (~/.guitarfx/ or platform equivalent).
    [[nodiscard]] virtual std::filesystem::path GetUserDataPath() const = 0;

    /// Root directory for bundled assets (factory presets, default IRs, etc.).
    [[nodiscard]] virtual std::filesystem::path GetBundledAssetsPath() const = 0;

    // ── Audio context ──────────────────────────────────────────────
    [[nodiscard]] virtual double GetSampleRate() const = 0;
    [[nodiscard]] virtual int GetBlockSize() const = 0;

    // ── Host-specific features ─────────────────────────────────────
    /// Open the standalone app audio/MIDI preferences (no-op in plugin formats).
    virtual void OpenAudioPreferences() {}

    /// Notify host that plugin state has changed (marks DAW project dirty).
    virtual void NotifyStateChanged() {}

    /// Notify the host that algorithmic latency has changed (e.g. after loading an IR).
    /// Implementations should call the host's latency-reporting API (e.g. setLatencySamples).
    virtual void NotifyLatencyChanged(int /*latencySamples*/) {}

    /// Get the DAW/host tempo in BPM, if available.
    [[nodiscard]] virtual double GetHostTempo() const { return 120.0; }

    /// Check if DAW is currently playing.
    [[nodiscard]] virtual bool IsHostPlaying() const { return false; }

    /// True for standalone app builds; false for DAW plugin formats.
    [[nodiscard]] virtual bool IsStandalone() const { return false; }
};

} // namespace guitarfx
