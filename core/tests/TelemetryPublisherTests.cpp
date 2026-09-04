/**
 * @file TelemetryPublisherTests.cpp
 * @brief Covers the signal-diagnostics wire format, and specifically what belongs
 *        in the roster versus in a per-frame payload.
 *
 * The roster exists so that everything static about a node is sent once instead of
 * twenty times a second, and it is re-sent whenever it differs from the last one.
 * That makes it a trap: put a value in it that tracks the *signal* rather than the
 * node set, and the roster is rebuilt and re-sent several times a second forever.
 * Channel count did exactly that until it moved into the frame, so these tests pin
 * the arrangement down from both sides -- the roster must sit still while the signal
 * moves, and must still be re-sent when the node set genuinely changes.
 */

#include "IPluginHost.h"
#include "controller/TelemetryPublisher.h"
#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace guitarfx;
using namespace guitarfx::controller_detail;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

/// Values per node entry in a frame's "d": the four level values plus channelCount.
constexpr std::size_t kNodeTupleLength = 5;

/// Fields per roster entry: scope, presetId, nodeId, nodeType, hasAnalyzer.
constexpr std::size_t kRosterEntryLength = 5;

bool Report(const std::string& label, bool passed, const std::string& detail = {})
{
    std::cout << "  " << std::left << std::setw(58) << (label + ":") << (passed ? "PASS" : "FAIL");

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";
    return passed;
}

/// Minimal standalone host. The telemetry publisher only asks it for the sample
/// rate and block size, which ride along in the performance-stats message.
class TestHost final : public IPluginHost
{
  public:
    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&, std::function<void(const BrowseFileResult&)>) override
    {
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)>) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        if (fn)
        {
            fn();
        }
    }

    [[nodiscard]] std::filesystem::path GetUserDataPath() const override
    {
        return {};
    }

    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override
    {
        return {};
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlockSize;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }
};

Preset MakePreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"gain", "gain", "utility", "Gain", true});
    graph.nodes.back().params["gainDb"] = 0.0;
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gain", 0, 0, 1.0});
    graph.edges.push_back({"gain", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

struct Fixture
{
    TestHost host;
    MultiPresetMixer mixer;
    std::vector<nlohmann::json> messages;
    TelemetryPublisher publisher;

    std::vector<float> inL = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> inR = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> outL = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> outR = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);

    Fixture()
        : publisher(host, mixer, [this](const std::string& message) {
              messages.push_back(nlohmann::json::parse(message, nullptr, false));
          })
    {
        mixer.Prepare(kSampleRate, kBlockSize);
        mixer.SetSignalDiagnosticsEnabled(true);
        mixer.AddActivePreset(MakePreset("presetA"), "presetA", "presetA");
    }

    /// Runs one block of audio. `stereo` decides whether the right channel carries
    /// its own content, which is what moves a node's reported channel count.
    void ProcessBlock(bool stereo)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const double t = static_cast<double>(i) / kSampleRate;
            const auto left = static_cast<float>(0.3 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * t));
            inL[static_cast<std::size_t>(i)] = left;
            inR[static_cast<std::size_t>(i)] = stereo ? -left : 0.0f;
        }

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        mixer.Process(inputs, outputs, kBlockSize);
    }

    /// Forces one diagnostics send. The publisher rate-limits to
    /// kSignalDiagnosticsRateHz against a real clock, so waiting out that interval
    /// is the only way to make consecutive sends actually go out.
    void SendDiagnostics()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kSignalDiagnosticsRateHz + 10));
        publisher.RequestSignalDiagnostics();
    }

    [[nodiscard]] std::vector<nlohmann::json> OfType(const std::string& type) const
    {
        std::vector<nlohmann::json> out;

        for (const auto& message : messages)
        {
            if (!message.is_discarded() && message.value("type", std::string{}) == type)
            {
                out.push_back(message);
            }
        }

        return out;
    }
};

/// A changing signal must not drag the roster along with it.
bool TestRosterIsStableWhileChannelCountMoves()
{
    std::cout << "\nRoster stability across signal changes\n";
    bool ok = true;

    Fixture fixture;

    // Alternate mono and stereo input. Before channel count moved into the frame,
    // each flip rebuilt the roster and re-sent it.
    for (int i = 0; i < 6; ++i)
    {
        fixture.ProcessBlock(i % 2 == 0);
        fixture.SendDiagnostics();
    }

    const auto rosters = fixture.OfType("sldRoster");
    const auto frames = fixture.OfType("sld");

    ok &= Report("Several frames were sent", frames.size() >= 4, std::to_string(frames.size()) + " frames");
    ok &= Report("The roster was sent exactly once", rosters.size() == 1, std::to_string(rosters.size()) + " rosters");

    // The channel count must still be reported, and must still track the signal --
    // suppressing the roster churn by freezing the value would be no fix at all.
    bool sawMono = false;
    bool sawStereo = false;

    for (const auto& frame : frames)
    {
        const auto& values = frame["d"];

        for (std::size_t node = 0; node * kNodeTupleLength < values.size(); ++node)
        {
            const int channelCount = values[node * kNodeTupleLength + 4].get<int>();

            if (channelCount == 1)
            {
                sawMono = true;
            }
            else if (channelCount == 2)
            {
                sawStereo = true;
            }
        }
    }

    ok &= Report("Channel count still tracks the signal", sawMono && sawStereo,
                 std::string("mono=") + (sawMono ? "yes" : "no") + " stereo=" + (sawStereo ? "yes" : "no"));

    return ok;
}

/// The frame and the roster have to agree on how many values each node contributes,
/// or the UI silently drops every frame as malformed.
bool TestFrameNodeStrideMatchesRoster()
{
    std::cout << "\nFrame and roster agree on width\n";
    bool ok = true;

    Fixture fixture;
    fixture.ProcessBlock(true);
    fixture.SendDiagnostics();

    const auto rosters = fixture.OfType("sldRoster");
    const auto frames = fixture.OfType("sld");

    if (rosters.empty() || frames.empty())
    {
        return Report("A roster and a frame were sent", false);
    }

    const auto nodeCount = rosters.front()["nodes"].size();
    const auto frameValues = frames.front()["d"].size();

    ok &= Report("Frame carries one node tuple per roster node", frameValues == nodeCount * kNodeTupleLength,
                 std::to_string(frameValues) + " values for " + std::to_string(nodeCount) + " nodes");

    bool everyEntryIsRightWidth = !rosters.front()["nodes"].empty();

    for (const auto& entry : rosters.front()["nodes"])
    {
        if (entry.size() != kRosterEntryLength)
        {
            everyEntryIsRightWidth = false;
        }
    }

    ok &= Report("Roster entries carry no per-block fields", everyEntryIsRightWidth,
                 std::to_string(kRosterEntryLength) + " fields expected");

    return ok;
}

/// The other half of the contract: suppressing roster churn must not suppress a
/// roster the UI genuinely needs.
bool TestRosterIsResentWhenNodeSetChanges()
{
    std::cout << "\nRoster resend on a real node-set change\n";
    bool ok = true;

    Fixture fixture;
    fixture.ProcessBlock(true);
    fixture.SendDiagnostics();

    const auto before = fixture.OfType("sldRoster").size();

    fixture.mixer.AddActivePreset(MakePreset("presetB"), "presetB", "presetB");
    fixture.ProcessBlock(true);
    fixture.SendDiagnostics();

    const auto rosters = fixture.OfType("sldRoster");
    ok &= Report("Adding a preset re-sent the roster", rosters.size() > before,
                 std::to_string(before) + " then " + std::to_string(rosters.size()));

    if (rosters.size() >= 2)
    {
        const auto firstSeq = rosters.front().value("seq", 0u);
        const auto lastSeq = rosters.back().value("seq", 0u);
        ok &= Report("The roster sequence advanced", lastSeq > firstSeq,
                     std::to_string(firstSeq) + " -> " + std::to_string(lastSeq));

        // A frame is matched to a roster by seq, so the newest frame must quote the
        // newest roster or the UI will discard it.
        const auto frames = fixture.OfType("sld");

        if (!frames.empty())
        {
            ok &= Report("The newest frame quotes the newest roster", frames.back().value("seq", 0u) == lastSeq);
        }
    }

    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;

    for (const auto& test : {TestRosterIsStableWhileChannelCountMoves, TestFrameNodeStrideMatchesRoster,
                             TestRosterIsResentWhenNodeSetChanges})
    {
        if (!test())
        {
            allPassed = false;
        }
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
