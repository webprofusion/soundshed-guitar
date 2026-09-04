#include "controller/TelemetryPublisher.h"

#include "IPluginHost.h"
#include "controller/internal/ControllerUtils.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/InputAnalyzerEffect.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
TelemetryPublisher::TelemetryPublisher(IPluginHost& host, MultiPresetMixer& presetMixer, SendMessageFn sendMessage)
    : mHost(host), mPresetMixer(presetMixer), mSendMessage(std::move(sendMessage))
{
}

void TelemetryPublisher::Send(const std::string& jsonMessage)
{
    if (mSendMessage)
    {
        mSendMessage(jsonMessage);
    }
}

void TelemetryPublisher::OnIdle()
{
    // The telemetry feeds are the app's largest ongoing cost - signal
    // diagnostics alone is ~6.7 KB at 20 Hz - and they only exist to drive
    // on-screen meters, so they are suppressed entirely while the UI is
    // hidden. The counters keep running so the cadence does not jump when it
    // comes back.
    const bool uiVisible = mUiVisible.load(std::memory_order_acquire);

    mPerformanceStatsCounter++;

    if (mPerformanceStatsCounter >= 60 / kDspPerformanceStatsRateHz)
    {
        mPerformanceStatsCounter = 0;

        if (uiVisible)
        {
            RequestPerformanceStats();
        }
    }

    TrySendPendingPerformanceStats();

    if (mSignalDiagnosticsEnabled.load(std::memory_order_acquire))
    {
        mSignalDiagnosticsCounter++;

        if (mSignalDiagnosticsCounter >= 60 / kSignalDiagnosticsRateHz)
        {
            mSignalDiagnosticsCounter = 0;

            if (uiVisible)
            {
                RequestSignalDiagnostics();
            }
        }
    }

    TrySendPendingSignalDiagnostics();

    // The spatial panner's on-screen puck has to match what is being heard: on
    // a 30 second orbit a visual phase drift is glaringly obvious. This is sent
    // on its own schedule rather than piggybacking on the diagnostics feed,
    // which the user has to opt into, and it costs nothing when no spatialiser
    // is in the chain.
    mSpatialPositionCounter++;

    if (mSpatialPositionCounter >= 60 / kSpatialPositionRateHz)
    {
        mSpatialPositionCounter = 0;
        SendSpatialPositions();
    }
}

void TelemetryPublisher::RequestSignalDiagnostics()
{
    mPendingSignalDiagnostics = true;
    TrySendPendingSignalDiagnostics();
}

void TelemetryPublisher::TrySendPendingSignalDiagnostics()
{
    if (!mPendingSignalDiagnostics)
    {
        return;
    }

    constexpr auto kMinSignalDiagnosticsInterval = std::chrono::milliseconds(1000 / kSignalDiagnosticsRateHz);
    const auto now = std::chrono::steady_clock::now();

    if (mLastSignalDiagnosticsSentAt.time_since_epoch().count() != 0 &&
        (now - mLastSignalDiagnosticsSentAt) < kMinSignalDiagnosticsInterval)
    {
        return;
    }

    mPendingSignalDiagnostics = false;
    mLastSignalDiagnosticsSentAt = now;
    SendSignalDiagnostics();
}

void TelemetryPublisher::SendSignalDiagnostics()
{
    auto snapshot = mPresetMixer.GetSignalDiagnosticsSnapshot();

    // Levels travel as bare numeric tuples, so the 20 Hz frame carries no repeated keys or
    // node ids. Values are rounded to 0.1 dB: the UI renders one decimal place, and full
    // double precision was costing ~14 characters per value for digits nothing displays.
    const auto roundDb = [](double db) { return std::round(db * 10.0) / 10.0; };
    const auto buildLevelTuple = [&roundDb](const MultiPresetMixer::SignalLevelStats& stats) {
        // `clipped` is carried explicitly rather than derived UI-side from the rounded peak:
        // a peak of 0.999 rounds to -0.0 dBFS, which would otherwise read as clipping.
        const bool clipped = stats.clipCount > 0 || stats.peak >= 1.0;
        return nlohmann::json::array({
            roundDb(ToDbFS(stats.peak)),
            roundDb(ToDbFS(stats.rms)),
            stats.clipCount,
            clipped ? 1 : 0,
        });
    };

    std::vector<RosterEntry> roster;
    roster.reserve(snapshot.nodes.size());

    for (const auto& n : snapshot.nodes)
    {
        roster.push_back(RosterEntry{n.scope, n.presetId, n.nodeId, n.nodeType, n.analyzer.has_value()});
    }

    if (mRosterDirty || roster != mRoster)
    {
        mRoster = roster;
        mRosterDirty = false;
        ++mRosterSeq;

        nlohmann::json rosterNodes = nlohmann::json::array();

        for (const auto& entry : mRoster)
        {
            rosterNodes.push_back(nlohmann::json::array(
                {entry.scope, entry.presetId, entry.nodeId, entry.nodeType, entry.hasAnalyzer ? 1 : 0}));
        }

        // The analyzer display ranges are compile-time constants, so they ride along with
        // the roster instead of being repeated in every analyzer payload.
        nlohmann::json rosterMsg;
        rosterMsg["type"] = "sldRoster";
        rosterMsg["seq"] = mRosterSeq;
        rosterMsg["nodes"] = std::move(rosterNodes);
        rosterMsg["spectrogramRange"] = nlohmann::json::array(
            {InputAnalyzerEffect::kSpectrogramMinDbfs, InputAnalyzerEffect::kSpectrogramMaxDbfs,
             InputAnalyzerEffect::kSpectrogramMinFrequencyHz, InputAnalyzerEffect::kSpectrogramMaxFrequencyHz});
        rosterMsg["barkRange"] =
            nlohmann::json::array({InputAnalyzerEffect::kBarkMinDbfs, InputAnalyzerEffect::kBarkMaxDbfs,
                                   InputAnalyzerEffect::kBarkMinFrequencyHz, InputAnalyzerEffect::kBarkMaxFrequencyHz});
        Send(rosterMsg.dump());
    }

    // Per-node entries carry one value more than the bare level tuple: the channel count,
    // which changes with the signal (0 when the node did not run this block, 1 mono,
    // 2 stereo) and so cannot live in the roster. The rawInput/input/output tuples below
    // stay at the plain level width.
    nlohmann::json frameLevels = nlohmann::json::array();

    for (const auto& n : snapshot.nodes)
    {
        for (const auto& value : buildLevelTuple(n.levels))
        {
            frameLevels.push_back(value);
        }

        frameLevels.push_back(n.channelCount);
    }

    nlohmann::json frame;
    frame["type"] = "sld";
    frame["seq"] = mRosterSeq;
    frame["r"] = buildLevelTuple(snapshot.rawInput);
    frame["i"] = buildLevelTuple(snapshot.input);
    frame["o"] = buildLevelTuple(snapshot.output);
    frame["d"] = std::move(frameLevels);
    Send(frame.dump());

    // Analyzer telemetry is an order of magnitude larger than a level tuple, so it moves out
    // of the frame into its own message rather than bloating every node entry. Band values are
    // rounded to whole dB — the spectrogram heatmap and bark bar graph cannot resolve finer.
    // NOTE: this still goes out for every analyzer node on every frame. The analyzer refreshes
    // on each processed block, so there is no cheap staleness check to skip on; the remaining
    // win here is only sending it for the node whose analyzer panel is actually open.
    for (const auto& n : snapshot.nodes)
    {
        if (!n.analyzer)
        {
            continue;
        }

        const auto& analyzer = *n.analyzer;
        const auto quantiseBands = [](const std::vector<float>& values) {
            nlohmann::json out = nlohmann::json::array();

            for (const float value : values)
            {
                out.push_back(std::isfinite(value) ? static_cast<int>(std::lround(value)) : -120);
            }

            return out;
        };

        nlohmann::json analyzerMsg;
        analyzerMsg["type"] = "sldA";
        analyzerMsg["seq"] = mRosterSeq;
        analyzerMsg["id"] = n.nodeId;
        analyzerMsg["t"] = analyzer.generatedAtMs;
        // Percent-of-full-scale values are not rounded to 0.1 like the dB fields: the UI also
        // converts them back to dBFS for display, where 0.1 percentage points is a 6 dB error
        // on a quiet signal. 1e-6 is far below the -120 dBFS floor and still trims the double.
        const auto roundPercent = [](double v) { return std::round(v * 1.0e6) / 1.0e6; };

        // [peakPercent, rmsPercent, rmsDbu, rmsDbv, rmsVolts, momentaryLufs, shortTermLufs,
        //  integratedLufs, activeChannelCount, stereo, loudnessValid]
        analyzerMsg["l"] = nlohmann::json::array({
            roundPercent(analyzer.peakPercent),
            roundPercent(analyzer.rmsPercent),
            roundDb(analyzer.rmsDbu),
            roundDb(analyzer.rmsDbv),
            std::round(analyzer.rmsVolts * 1000.0) / 1000.0,
            roundDb(analyzer.momentaryLufs),
            roundDb(analyzer.shortTermLufs),
            roundDb(analyzer.integratedLufs),
            analyzer.activeChannelCount,
            analyzer.stereo ? 1 : 0,
            analyzer.loudnessValid ? 1 : 0,
        });
        analyzerMsg["s"] = quantiseBands(analyzer.spectrogramBinsDb);
        analyzerMsg["b"] = quantiseBands(analyzer.barkBandsDb);
        Send(analyzerMsg.dump());
    }
}

void TelemetryPublisher::RequestPerformanceStats()
{
    mPendingPerformanceStats = true;
    TrySendPendingPerformanceStats();
}

void TelemetryPublisher::TrySendPendingPerformanceStats()
{
    if (!mPendingPerformanceStats)
    {
        return;
    }

    constexpr auto kMinPerformanceStatsInterval = std::chrono::milliseconds(1000 / kDspPerformanceStatsRateHz);
    const auto now = std::chrono::steady_clock::now();

    if (mLastPerformanceStatsSentAt.time_since_epoch().count() != 0 &&
        (now - mLastPerformanceStatsSentAt) < kMinPerformanceStatsInterval)
    {
        return;
    }

    mPendingPerformanceStats = false;
    mLastPerformanceStatsSentAt = now;
    SendPerformanceStats();
}

void TelemetryPublisher::SendPerformanceStats()
{
    auto stats = mPresetMixer.GetPerformanceStats();
    const int totalLatencySamples = mPresetMixer.GetTotalLatencySamples();
    nlohmann::json statsJson;
    statsJson["totalProcessingTimeUs"] = stats.totalProcessingTimeUs;
    statsJson["realTimeUs"] = stats.realTimeUs;
    statsJson["dspLoadPercent"] = stats.dspLoadPercent;
    statsJson["totalLatencySamples"] = totalLatencySamples;
    nlohmann::json nodeTimes = nlohmann::json::object();

    for (const auto& [nodeId, timeUs] : stats.nodeProcessingTimesUs)
    {
        nodeTimes[nodeId] = timeUs;
    }

    statsJson["nodeProcessingTimesUs"] = nodeTimes;
    nlohmann::json scopedNodeTimes = nlohmann::json::object();

    for (const auto& [nodeId, timeUs] : stats.scopedNodeProcessingTimesUs)
    {
        scopedNodeTimes[nodeId] = timeUs;
    }

    statsJson["scopedNodeProcessingTimesUs"] = scopedNodeTimes;
    nlohmann::json nodeLatencies = nlohmann::json::object();

    for (const auto& [nodeId, latencySamples] : stats.nodeLatencySamples)
    {
        nodeLatencies[nodeId] = latencySamples;
    }

    statsJson["nodeLatencySamples"] = nodeLatencies;
    nlohmann::json scopedNodeLatencies = nlohmann::json::object();

    for (const auto& [nodeId, latencySamples] : stats.scopedNodeLatencySamples)
    {
        scopedNodeLatencies[nodeId] = latencySamples;
    }

    statsJson["scopedNodeLatencySamples"] = scopedNodeLatencies;

    nlohmann::json msg;
    msg["type"] = "dspPerformance";
    msg["stats"] = statsJson;
    msg["sampleRate"] = mHost.GetSampleRate();
    msg["blockSize"] = mHost.GetBlockSize();
    Send(msg.dump());
}

void TelemetryPublisher::SendSpatialPositions()
{
    static const std::vector<std::string> kSpatialReadoutParams = {
        "currentAzimuth", "currentElevation", "currentDistance", "currentItdUs",
        "currentIldDb",   "effectiveRate",    "motionMode"};

    const auto readouts = mPresetMixer.ReadNodeParamsForType(EffectGuids::kSpatial3D, kSpatialReadoutParams);

    if (readouts.empty())
    {
        // Send one final empty update so a UI that was tracking a node it can no
        // longer see stops animating, then go quiet until a spatialiser reappears.
        if (!mSpatialPositionsWereSent)
        {
            return;
        }

        mSpatialPositionsWereSent = false;
    }
    else
    {
        mSpatialPositionsWereSent = true;
    }

    nlohmann::json nodes = nlohmann::json::array();

    for (const auto& readout : readouts)
    {
        nlohmann::json node{
            {"scope", readout.scope},         {"nodeId", readout.nodeId},      {"azimuth", readout.values[0]},
            {"elevation", readout.values[1]}, {"distance", readout.values[2]}, {"itdUs", readout.values[3]},
            {"ildDb", readout.values[4]},     {"rateHz", readout.values[5]},   {"moving", readout.values[6] > 0.5}};

        if (!readout.presetId.empty())
        {
            node["presetId"] = readout.presetId;
        }

        nodes.push_back(std::move(node));
    }

    nlohmann::json msg{{"type", "spatialPosition"}, {"nodes", std::move(nodes)}};
    Send(msg.dump());
}
} // namespace guitarfx
