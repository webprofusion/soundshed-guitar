#pragma once

// TelemetryPublisher — the three metering feeds the UI reads continuously:
// per-node signal diagnostics, DSP performance stats, and spatialiser puck
// positions.
//
// These are the app's largest ongoing IPC cost — signal diagnostics alone is
// ~6.7 KB at 20 Hz — and they exist only to drive on-screen meters. Everything
// here is therefore built around not sending: nothing goes out while the UI is
// hidden, each feed is rate-limited independently of the idle tick that drives
// it, and the diagnostics frames carry numbers only.
//
// The roster is what makes that last part work. Everything about a node that
// does not change frame to frame lives in a roster message sent once per
// change, with a sequence number; the 20 Hz frames reference that sequence and
// the UI drops any frame whose roster it does not have. A graph edit marks the
// roster dirty, and a reloaded UI recovers without waiting for one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace guitarfx
{
class IPluginHost;
class MultiPresetMixer;

class TelemetryPublisher
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;

    TelemetryPublisher(IPluginHost& host, MultiPresetMixer& presetMixer, SendMessageFn sendMessage);

    /// Drives all three feeds. Called once per idle tick (~60 Hz).
    void OnIdle();

    /// Suppresses every feed while false. Set from the UI's visibility message.
    void SetUiVisible(bool visible)
    {
        mUiVisible.store(visible, std::memory_order_release);
    }

    [[nodiscard]] bool IsUiVisible() const
    {
        return mUiVisible.load(std::memory_order_acquire);
    }

    /// Forces the next diagnostics send to re-emit the roster even if the node
    /// set is unchanged — what a graph edit or a reloaded UI needs.
    void MarkRosterDirty()
    {
        mRosterDirty = true;
    }

    /// Out-of-band requests, for a UI asking for a sample immediately rather
    /// than waiting for the next tick.
    void RequestSignalDiagnostics();
    void RequestPerformanceStats();

  private:
    /// One node in the diagnostics roster.
    ///
    /// Everything here must be a property of the *node set*, not of the signal passing
    /// through it: the roster is only re-sent when this compares unequal, and anything
    /// that varies block to block turns that into a re-send several times a second.
    /// Channel count is exactly such a value, so it rides in the per-frame payload.
    struct RosterEntry
    {
        std::string scope;
        std::string presetId;
        std::string nodeId;
        std::string nodeType;
        bool hasAnalyzer = false;

        bool operator==(const RosterEntry&) const = default;
    };

    void Send(const std::string& jsonMessage);

    void TrySendPendingSignalDiagnostics();
    void SendSignalDiagnostics();
    void TrySendPendingPerformanceStats();
    void SendPerformanceStats();
    void SendSpatialPositions();

    IPluginHost& mHost;
    MultiPresetMixer& mPresetMixer;
    SendMessageFn mSendMessage;

    std::atomic<bool> mUiVisible{true};
    std::atomic<bool> mSignalDiagnosticsEnabled{true};

    // Idle-tick dividers, one per feed.
    int mPerformanceStatsCounter = 0;
    int mSignalDiagnosticsCounter = 0;
    int mSpatialPositionCounter = 0;

    bool mPendingSignalDiagnostics = false;
    std::chrono::steady_clock::time_point mLastSignalDiagnosticsSentAt{};
    bool mPendingPerformanceStats = false;
    std::chrono::steady_clock::time_point mLastPerformanceStatsSentAt{};

    std::vector<RosterEntry> mRoster;
    std::uint32_t mRosterSeq = 0;
    bool mRosterDirty = true;

    /// Lets the spatial feed send one final "nothing here" frame when the last
    /// spatialiser leaves the chain, instead of the puck freezing where it was.
    bool mSpatialPositionsWereSent = false;
};
} // namespace guitarfx
