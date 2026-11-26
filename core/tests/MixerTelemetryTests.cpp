#include "dsp/MixerTelemetry.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace guitarfx;

int main()
{
    MixerTelemetry telemetry;
    const float left[] = {1.5f, -0.5f};
    const float right[] = {0.5f, -2.0f};

    telemetry.Record(MixerTelemetry::Stage::RawInput, left, right, 2);
    telemetry.Record(MixerTelemetry::Stage::Input, left, nullptr, 2);
    telemetry.Record(MixerTelemetry::Stage::Output, nullptr, nullptr, 2);
    telemetry.NoteOversizedBlock();

    const auto snapshot = telemetry.GetSnapshot();

    if (snapshot.rawInput.peak != 2.0 || snapshot.rawInput.clipCount != 2 ||
        std::abs(snapshot.rawInput.rms - std::sqrt(6.75 / 4.0)) > 1e-6 || snapshot.input.peak != 1.5 ||
        snapshot.input.clipCount != 1 || std::abs(snapshot.input.rms - std::sqrt(2.5 / 2.0)) > 1e-6 ||
        snapshot.output.peak != 0.0 || snapshot.output.rms != 0.0 || telemetry.GetOversizedBlockCount() != 1)
    {
        std::cerr << "Mixer level accumulation changed\n";
        return 1;
    }

    // The span runs from one snapshot to the next: a loud block followed by quiet ones still
    // reads as the loud peak, and the RMS and clips cover every block in between.
    {
        MixerTelemetry span;
        const float loud[] = {0.9f, -1.2f};
        const float quiet[] = {0.1f, -0.1f};

        span.Record(MixerTelemetry::Stage::Output, loud, nullptr, 2);

        for (int i = 0; i < 3; ++i)
        {
            span.Record(MixerTelemetry::Stage::Output, quiet, nullptr, 2);
        }

        const auto first = span.GetSnapshot();
        const double meanSquare = (0.81 + 1.44 + 6 * 0.01) / 8.0;

        if (std::abs(first.output.peak - 1.2) > 1e-6 || first.output.clipCount != 1 ||
            std::abs(first.output.rms - std::sqrt(meanSquare)) > 1e-6)
        {
            std::cerr << "A block between two snapshots was lost (peak " << first.output.peak << ")\n";
            return 1;
        }

        // No block since: the same reading again, not silence.
        const auto repeat = span.GetSnapshot();

        if (repeat.output.peak != first.output.peak || repeat.output.rms != first.output.rms)
        {
            std::cerr << "A snapshot with no new block did not repeat the last one\n";
            return 1;
        }

        // The next block starts a new span.
        span.Record(MixerTelemetry::Stage::Output, quiet, nullptr, 2);
        const auto next = span.GetSnapshot();

        if (std::abs(next.output.peak - 0.1) > 1e-6 || next.output.clipCount != 0 ||
            std::abs(next.output.rms - 0.1) > 1e-6)
        {
            std::cerr << "A snapshot did not start a new span (peak " << next.output.peak << ")\n";
            return 1;
        }

        // A span nobody reads starts again once it is long, so a stale peak does not wait for
        // the next reader.
        span.Record(MixerTelemetry::Stage::Input, loud, nullptr, 2);
        std::vector<float> silence(4096, 0.0f);

        for (int i = 0; i < 17; ++i)
        {
            span.Record(MixerTelemetry::Stage::Input, silence.data(), nullptr, static_cast<int>(silence.size()));
        }

        if (span.GetSnapshot().input.peak != 0.0)
        {
            std::cerr << "An unread span kept its peak past the cap\n";
            return 1;
        }
    }

    SignalGraphExecutor::NodeSignalLevel source;
    source.nodeId = "gate";
    source.nodeType = "dynamics.gate";
    source.peak = 0.8;
    source.rms = 0.4;
    source.clipCount = 3;
    source.channelCount = 2;
    const auto node = MixerTelemetry::ToSnapshotNode(source, "preset", "slot-a");

    if (node.scope != "preset" || node.presetId != "slot-a" || node.nodeId != source.nodeId ||
        node.nodeType != source.nodeType || node.channelCount != 2 || node.levels.peak != 0.8 ||
        node.levels.rms != 0.4 || node.levels.clipCount != 3)
    {
        std::cerr << "Node diagnostics mapping changed\n";
        return 1;
    }

    return 0;
}
