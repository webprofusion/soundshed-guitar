/**
 * UiClientAnalysisHandlers.cpp - The analysis feeds (an EQ's spectrum backdrop, loudness for the
 * DSP status readouts, Signal Analyzer frames, spatial positions, gate Learn) and the replies to
 * the editors' measuring requests, which Analysis matches to their callbacks.
 */

#include "dsp/FiniteCheck.h"
#include "uiclient/Analysis.h"
#include "uiclient/UiClient.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::uiclient
{
namespace
{
std::optional<double> OptionalNumber(const nlohmann::json& values, std::size_t index)
{
    if (!values.is_array() || index >= values.size() || !values[index].is_number())
    {
        return std::nullopt;
    }

    const double value = values[index].get<double>();
    return guitarfx::IsFinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::optional<double> OptionalField(const nlohmann::json& m, const char* key)
{
    const auto it = m.find(key);
    return it != m.end() && it->is_number() ? std::optional<double>(it->get<double>()) : std::nullopt;
}

std::vector<double> Numbers(const nlohmann::json& m, const char* key)
{
    std::vector<double> values;
    const auto it = m.find(key);

    if (it == m.end() || !it->is_array())
    {
        return values;
    }

    values.reserve(it->size());

    for (const auto& item : *it)
    {
        values.push_back(item.is_number() ? item.get<double>() : -120.0);
    }

    return values;
}

/// A frame's output bins ("o") and band meters ("b": [lowHz, highHz, grDb, levelDb] each, then
/// thresholdDb from newer engines), into `spectrum`; both left empty unless the whole of each is
/// well formed (as eqSpectrum.ts parseBandMeters reads them).
void ReadBandMeters(const nlohmann::json& m, NodeSpectrum& spectrum)
{
    spectrum.outputBinsDb.clear();
    spectrum.bands.clear();
    const auto output = m.find("o");
    const auto bands = m.find("b");

    if (output == m.end() || !output->is_array() || output->size() != spectrum.binsDb.size() || bands == m.end() ||
        !bands->is_array())
    {
        return;
    }

    std::vector<NodeSpectrumBand> parsed;

    for (const auto& entry : *bands)
    {
        if (!entry.is_array() || entry.size() < 4)
        {
            return;
        }

        const auto lowHz = OptionalNumber(entry, 0);
        const auto highHz = OptionalNumber(entry, 1);
        const auto reduction = OptionalNumber(entry, 2);
        const auto level = OptionalNumber(entry, 3);

        if (!lowHz || !highHz || !reduction || !level)
        {
            return;
        }

        NodeSpectrumBand band;
        band.lowHz = std::max(0.0, *lowHz);
        band.highHz = std::max(0.0, *highHz);
        band.gainReductionDb = std::max(0.0, *reduction);
        band.levelDb = *level;
        band.thresholdDb = OptionalNumber(entry, 4);
        parsed.push_back(band);
    }

    spectrum.outputBinsDb.reserve(output->size());

    for (std::size_t i = 0; i < output->size(); ++i)
    {
        spectrum.outputBinsDb.push_back(OptionalNumber(*output, i).value_or(spectrum.floorDb));
    }

    spectrum.bands = std::move(parsed);
}
} // namespace

void UiClient::RegisterAnalysisHandlers()
{
    // ── The spectrum behind an EQ curve, ~30 Hz while watched ─────────────────
    On("sldS", [this](const nlohmann::json& m) {
        auto& spectrum =
            mState.analysis.spectra[NodeFeedKey(m.value("scope", ""), m.value("presetId", ""), m.value("id", ""))];
        const auto range = m.value("r", nlohmann::json::array());
        spectrum.minHz = OptionalNumber(range, 0).value_or(20.0);
        spectrum.maxHz = OptionalNumber(range, 1).value_or(20000.0);
        spectrum.floorDb = OptionalNumber(range, 2).value_or(-96.0);
        spectrum.ceilingDb = OptionalNumber(range, 3).value_or(0.0);
        spectrum.binsDb = Numbers(m, "s");
        ReadBandMeters(m, spectrum);
        spectrum.receivedAt = mNowSeconds;
        Notify(Topic::Analysis);
    });

    // ── Loudness of a node's output, or (slot 1) of the app's output ──────────
    On("sldL", [this](const nlohmann::json& m) {
        const auto values = m.value("l", nlohmann::json::array());
        LoudnessReading reading;
        reading.momentaryLufs = OptionalNumber(values, 0);
        reading.shortTermLufs = OptionalNumber(values, 1);
        reading.integratedLufs = OptionalNumber(values, 2);
        reading.integratedSeconds = OptionalNumber(values, 3).value_or(0.0);
        reading.receivedAt = mNowSeconds;

        if (m.value("slot", 0) == 1)
        {
            mState.analysis.outputLoudness = reading;
        }
        else
        {
            mState.analysis.loudness[NodeFeedKey(m.value("scope", ""), m.value("presetId", ""), m.value("id", ""))] =
                reading;
        }

        Notify(Topic::Analysis);
    });

    // ── Signal Analyzer nodes ────────────────────────────────────────────────
    On("sldA", [this](const nlohmann::json& m) {
        const auto id = m.value("id", std::string());
        const auto levels = m.value("l", nlohmann::json::array());

        if (id.empty() || !levels.is_array())
        {
            return;
        }

        AnalyzerFrame frame;
        frame.peakPercent = OptionalNumber(levels, 0).value_or(0.0);
        frame.rmsPercent = OptionalNumber(levels, 1).value_or(0.0);
        frame.rmsDbu = OptionalNumber(levels, 2).value_or(0.0);
        frame.rmsDbv = OptionalNumber(levels, 3).value_or(0.0);
        frame.rmsVolts = OptionalNumber(levels, 4).value_or(0.0);
        frame.momentaryLufs = OptionalNumber(levels, 5);
        frame.shortTermLufs = OptionalNumber(levels, 6);
        frame.integratedLufs = OptionalNumber(levels, 7);
        frame.activeChannelCount = static_cast<int>(OptionalNumber(levels, 8).value_or(0.0));
        frame.stereo = OptionalNumber(levels, 9).value_or(0.0) == 1.0;
        frame.loudnessValid = OptionalNumber(levels, 10).value_or(0.0) == 1.0;
        frame.spectrogramDb = Numbers(m, "s");
        frame.barkDb = Numbers(m, "b");
        frame.timeMs = m.value("t", 0.0);
        mState.analysis.analyzers[id] = std::move(frame);
        Notify(Topic::Analysis);
    });

    // ── Where each 3D Spatial node's source is, ~20 Hz ───────────────────────
    On("spatialPosition", [this](const nlohmann::json& m) {
        static std::uint64_t serial = 0;
        ++serial;
        auto& spatial = mState.analysis.spatial;
        spatial.clear();

        for (const auto& node : m.value("nodes", nlohmann::json::array()))
        {
            if (!node.is_object())
            {
                continue;
            }

            // As the web UI reads them (spatial.ts): a field that is not a number takes its
            // default, and only `true` is moving.
            SpatialPosition position;
            position.azimuth = OptionalField(node, "azimuth").value_or(0.0);
            position.elevation = OptionalField(node, "elevation").value_or(0.0);
            position.distance = OptionalField(node, "distance").value_or(1.5);
            position.itdUs = OptionalField(node, "itdUs").value_or(0.0);
            position.ildDb = OptionalField(node, "ildDb").value_or(0.0);
            position.rateHz = OptionalField(node, "rateHz").value_or(0.0);
            const auto moving = node.find("moving");
            position.moving = moving != node.end() && moving->is_boolean() && moving->get<bool>();
            position.serial = serial;
            spatial[NodeFeedKey(node.value("scope", ""), node.value("presetId", ""), node.value("nodeId", ""))] =
                position;
        }

        Notify(Topic::Analysis);
    });

    // ── Noise gate Learn ─────────────────────────────────────────────────────
    On("gateLearnStatus", [this](const nlohmann::json& m) {
        auto& status =
            mState.analysis
                .gateLearn[NodeFeedKey(m.value("scope", ""), m.value("presetId", ""), m.value("nodeId", ""))];
        status.state = m.value("state", std::string());
        status.progress = OptionalField(m, "progress").value_or(status.state == "done" ? 1.0 : status.progress);
        status.levelDb = OptionalField(m, "levelDb");
        status.spreadDb = OptionalField(m, "spreadDb");
        status.thresholdDb = OptionalField(m, "thresholdDb");
        status.reason = m.value("reason", std::string());

        // A status without a noise floor keeps showing the last one learned, and a listen that
        // heard playing leaves it as it was: what it heard was not noise. A Multi-band Gate's
        // bands: live levels while listening, results once done; a band that brings no noise
        // floor keeps the one last learned (gateLearn.ts onGateLearnStatus).
        const bool heardPlaying = status.state == "failed" && status.reason == "playing";

        if (const auto noiseFloorDb = OptionalField(m, "noiseFloorDb"); noiseFloorDb && !heardPlaying)
        {
            status.noiseFloorDb = noiseFloorDb;
        }

        const auto previous = std::move(status.bands);
        status.bands.clear();

        if (const auto it = m.find("bands"); it != m.end() && it->is_array() && !heardPlaying)
        {
            for (const auto& entry : *it)
            {
                GateLearnBandStatus band;
                band.levelDb = OptionalField(entry, "levelDb");
                band.noiseFloorDb = OptionalField(entry, "noiseFloorDb");
                band.spreadDb = OptionalField(entry, "spreadDb");
                band.thresholdDb = OptionalField(entry, "thresholdDb");

                if (const auto index = status.bands.size(); !band.noiseFloorDb && index < previous.size())
                {
                    band.noiseFloorDb = previous[index].noiseFloorDb;
                }

                status.bands.push_back(band);
            }
        }
        else
        {
            for (const auto& kept : previous)
            {
                status.bands.push_back(GateLearnBandStatus{{}, kept.noiseFloorDb, {}, {}});
            }
        }

        Notify(Topic::Analysis);
    });

    // ── Replies to the editors' requests ─────────────────────────────────────
    On("effectResponse", [this](const nlohmann::json& m) { mAnalysis->OnEffectResponse(m); });
    On("irAlignment", [this](const nlohmann::json& m) { mAnalysis->OnIrAlignment(m); });
    On("irAlignmentResponse", [this](const nlohmann::json& m) { mAnalysis->OnIrAlignmentResponse(m); });
    On("simpleCabIrMatch", [this](const nlohmann::json& m) { mAnalysis->OnSimpleCabMatch(m); });
}
} // namespace guitarfx::uiclient
