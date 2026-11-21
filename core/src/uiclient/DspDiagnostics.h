#pragma once

/**
 * DspDiagnostics - the figures of Settings → DSP Performance for the native UIs, as the web UI
 * shows them (views.ts updateDSPPerformancePlot and renderSignalDiagnosticsPanel, dspNodeTable.ts):
 * the DSP load and its recent history, the chain's input and output levels, and a table of every
 * node the engine is running, grouped by chain (the global pre chain, each running preset with the
 * active one first, the global post chain) and in signal order within each.
 *
 * Peaks are held for ten seconds, as the web UI holds them, so a figure can be read; the table is
 * worth redrawing at most four times a second (TakeRenderTurn).
 */

#include "uiclient/ClientState.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// One row of the per-node table, formatted for its cells.
struct DspDiagnosticsRow
{
    std::string label;
    std::string channels; // "2", "1", or a dash when it did not run
    std::string time;     // microseconds in the last block
    std::string share;    // percent of the chain's processing
    std::string latency;  // "1.33 ms", or a dash when it adds none
    std::string peak;     // dBFS, held
    bool stereo = false;
    bool clipped = false;
    int clipCount = 0;

    bool operator==(const DspDiagnosticsRow&) const = default;
};

/// A chain's rows under its name; the chain input's group has no title.
struct DspDiagnosticsGroup
{
    std::string title;
    std::vector<DspDiagnosticsRow> rows;

    bool operator==(const DspDiagnosticsGroup&) const = default;
};

/// The chain's input or output: peak (held), RMS, headroom and clipping.
struct DspLevelSummary
{
    std::string peak;
    std::string rms;
    std::string headroom;
    bool clipped = false;
    int clipCount = 0;

    bool operator==(const DspLevelSummary&) const = default;
};

class DspDiagnostics
{
  public:
    static constexpr double kPeakHoldSeconds = 10.0;
    static constexpr std::size_t kHistoryPoints = 100; // ~20 s of the 5 Hz performance feed
    static constexpr double kRenderIntervalSeconds = 0.25;

    /// Folds in the state's latest telemetry. Call every tick; a frame already seen is skipped.
    void Update(const ClientState& state, double nowSeconds);
    /// Whether the figures are due a redraw; marks them drawn when they are.
    bool TakeRenderTurn(double nowSeconds);

    /// DSP load per performance frame, oldest first.
    [[nodiscard]] const std::deque<double>& LoadHistory() const
    {
        return mLoad;
    }

    [[nodiscard]] std::string Load() const;
    /// The highest load in the history.
    [[nodiscard]] std::string PeakLoad() const;
    [[nodiscard]] static std::string Latency(const ClientState& state);

    [[nodiscard]] DspLevelSummary Input(const ClientState& state) const;
    [[nodiscard]] DspLevelSummary Output(const ClientState& state) const;
    [[nodiscard]] std::vector<DspDiagnosticsGroup> Table(const ClientState& state) const;

    void Reset();

  private:
    struct Hold
    {
        double peakDb = -120.0;
        double since = 0.0;
    };

    void HoldPeak(const std::string& key, double peakDb, double now);
    [[nodiscard]] double Held(const std::string& key, double fallback) const;

    std::map<std::string, Hold> mHolds;
    std::deque<double> mLoad;
    unsigned mLastFrame = 0;
    unsigned mLastPerformanceFrame = 0;
    std::string mHoldPresetId; // the holds start over when the active preset changes
    double mLastRender = -1.0e9;
};
} // namespace guitarfx::uiclient
