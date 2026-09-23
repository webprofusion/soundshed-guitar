/**
 * UiClientLiveHandlers.cpp - The fast feeds: meters, DSP load, tuner, metronome beats, the
 * audio device page, and demo playback.
 */

#include "uiclient/UiClient.h"
#include "uiclient/UiClientParsing.h"

namespace guitarfx::uiclient
{
namespace
{
/// Values per node in a "sld" frame: peak, rms, clip count, clipped, channel count.
constexpr std::size_t kNodeTupleLength = 5;

LevelReading ReadTuple(const nlohmann::json& values, std::size_t offset, bool withChannels)
{
    LevelReading reading;

    if (!values.is_array() || values.size() < offset + 4)
    {
        return reading;
    }

    reading.peakDb = values[offset].is_number() ? values[offset].get<double>() : -120.0;
    reading.rmsDb = values[offset + 1].is_number() ? values[offset + 1].get<double>() : -120.0;
    reading.clipCount = values[offset + 2].is_number() ? values[offset + 2].get<int>() : 0;
    reading.clipped = values[offset + 3].is_number() && values[offset + 3].get<int>() == 1;

    if (withChannels && values.size() > offset + 4 && values[offset + 4].is_number())
    {
        reading.channelCount = values[offset + 4].get<int>();
    }

    return reading;
}
} // namespace

void UiClient::RegisterLiveHandlers()
{
    // ── Signal diagnostics: a roster when the node set changes, then bare tuples ──
    On("sldRoster", [this](const nlohmann::json& m) {
        mRoster.clear();
        mRosterSeq = m.value("seq", -1);

        if (m.contains("nodes") && m["nodes"].is_array())
        {
            for (const auto& entry : m["nodes"])
            {
                if (!entry.is_array() || entry.size() < 3)
                {
                    continue;
                }

                const auto scope = entry[0].is_string() ? entry[0].get<std::string>() : std::string{};
                const auto presetId = entry[1].is_string() ? entry[1].get<std::string>() : std::string{};
                const auto nodeId = entry[2].is_string() ? entry[2].get<std::string>() : std::string{};
                // Keyed as the engine keys dspPerformance: "pre", "post", or the preset id.
                const auto owner = scope == "pre" || scope == "post" ? scope : presetId;
                mRoster.push_back(RosterEntry{owner + "::" + nodeId, nodeId});
            }
        }
    });

    On("sld", [this](const nlohmann::json& m) {
        auto& telemetry = mState.telemetry;
        telemetry.rawInput = ReadTuple(m.value("r", nlohmann::json::array()), 0, false);
        telemetry.input = ReadTuple(m.value("i", nlohmann::json::array()), 0, false);
        telemetry.output = ReadTuple(m.value("o", nlohmann::json::array()), 0, false);

        const auto& frame = m.contains("d") ? m["d"] : nlohmann::json::array();
        const bool rosterMatches = m.value("seq", -2) == mRosterSeq && frame.is_array() &&
                                   frame.size() == mRoster.size() * kNodeTupleLength;

        if (rosterMatches)
        {
            telemetry.nodes.clear();

            for (std::size_t i = 0; i < mRoster.size(); ++i)
            {
                telemetry.nodes[mRoster[i].key] = ReadTuple(frame, i * kNodeTupleLength, true);
            }
        }
        else if (mNowSeconds - mLastRosterRequest >= 1.0)
        {
            // Rosters are only sent when the node set changes, so with none held nothing
            // arrives on its own. Ask again, at most once a second.
            mLastRosterRequest = mNowSeconds;
            Send("setSignalDiagnosticsEnabled", {{"enabled", true}});
        }

        ++telemetry.frameCounter;
        Notify(Topic::Telemetry);
    });

    On("dspPerformance", [this](const nlohmann::json& m) {
        auto& telemetry = mState.telemetry;
        const auto stats = m.value("stats", nlohmann::json::object());
        telemetry.dspLoadPercent = stats.value("dspLoadPercent", 0.0);
        telemetry.totalLatencySamples = stats.value("totalLatencySamples", 0);
        telemetry.nodeProcessingUs.clear();

        if (stats.contains("nodeProcessingTimesUs") && stats["nodeProcessingTimesUs"].is_object())
        {
            for (const auto& [key, value] : stats["nodeProcessingTimesUs"].items())
            {
                if (value.is_number())
                {
                    telemetry.nodeProcessingUs[key] = value.get<double>();
                }
            }
        }

        Notify(Topic::Telemetry);
    });

    // ── Tuner ────────────────────────────────────────────────────────────────
    On("tunerStarted", [this](const nlohmann::json& m) {
        auto& tuner = mState.tuner;
        tuner.active = true;
        tuner.referenceFrequency = m.value("referenceFrequency", tuner.referenceFrequency);
        tuner.liveMode = m.value("liveMode", tuner.liveMode);
        Notify(Topic::Tuner);
    });

    On("tunerStopped", [this](const nlohmann::json&) {
        mState.tuner.active = false;
        mState.tuner.detected = false;
        Notify(Topic::Tuner);
    });

    On("tunerLiveModeChanged", [this](const nlohmann::json& m) {
        mState.tuner.liveMode = m.value("liveMode", mState.tuner.liveMode);
        Notify(Topic::Tuner);
    });

    On("tunerReferenceChanged", [this](const nlohmann::json& m) {
        mState.tuner.referenceFrequency = m.value("referenceFrequency", mState.tuner.referenceFrequency);
        Notify(Topic::Tuner);
    });

    On("tunerUpdate", [this](const nlohmann::json& m) {
        auto& tuner = mState.tuner;
        tuner.detected = m.value("detected", false);
        tuner.noteName = m.value("noteName", "");
        tuner.octave = m.value("octave", 0);
        tuner.frequency = m.value("frequency", 0.0);
        tuner.centOffset = m.value("centOffset", 0.0);
        tuner.confidence = m.value("confidence", 0.0);
        Notify(Topic::Tuner);
    });

    // ── Metronome ────────────────────────────────────────────────────────────
    On("metronomeBeat", [this](const nlohmann::json& m) {
        auto& metronome = mState.metronome;
        metronome.beatIndex = m.value("beatIndex", 0);
        metronome.beatsPerBar = m.value("beatsPerBar", metronome.beatsPerBar);
        metronome.beatLevel = m.value("level", "normal");
        ++metronome.beatCounter;
        Notify(Topic::MetronomeBeat);
    });

    // ── Audio device (standalone) ──────────────────────────────────────────
    On("audioDeviceState", [this](const nlohmann::json& m) {
        auto& device = mState.device;
        device.known = true;
        device.available = m.value("available", false);
        device.error = m.value("error", "");
        device.state = m.contains("state") && m["state"].is_object() ? m["state"] : nlohmann::json::object();
        Notify(Topic::Device);
    });

    On("audioDeviceLevels", [this](const nlohmann::json& m) {
        mState.device.inputLevelDb = m.value("input", -100.0);
        mState.device.xruns = m.value("xruns", -1);
        Notify(Topic::DeviceLevels);
    });

    // ── Demo audio ───────────────────────────────────────────────────────────
    On("previewStarted", [this](const nlohmann::json& m) {
        mState.demo.playing = true;
        mState.demo.title = m.value("title", "");
        Notify(Topic::Demo);
    });

    On("previewComplete", [this](const nlohmann::json&) {
        mState.demo.playing = false;
        Notify(Topic::Demo);
    });

    On("previewStopped", [this](const nlohmann::json&) {
        mState.demo.playing = false;
        Notify(Topic::Demo);
    });

    On("demoAudioRenderSaved", [this](const nlohmann::json& m) {
        mState.demo.lastRenderPath = m.value("path", "");
        Notify(Topic::Demo);
        PostNotification({Notification::Kind::Info, "Render saved", mState.demo.lastRenderPath});
    });

    On("demoAudioRenderFailed", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Error, "Render failed", m.value("message", "")});
    });
}
} // namespace guitarfx::uiclient
