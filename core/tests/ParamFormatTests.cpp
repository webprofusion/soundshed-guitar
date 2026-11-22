/**
 * ParamFormatTests.cpp - The settings flyouts' parameter rows (uiclient/ParamFormat.h): which
 * parameters are switches and how a value reads, held to the web UI's flyoutParamControls.ts.
 * Also Pitch Shift's Semitones knob range (uiclient/PitchShiftRange.h), held to the web UI's
 * pitchShiftRange.test.ts.
 */

#include "dsp/EffectGuids.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/PitchShiftRange.h"

#include <iostream>
#include <map>
#include <string>

namespace uic = guitarfx::uiclient;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

uic::EffectParamInfo Param(double min, double max, std::string unit, std::vector<std::string> labels = {},
                           double step = 0.0)
{
    uic::EffectParamInfo p;
    p.minValue = min;
    p.maxValue = max;
    p.unit = std::move(unit);
    p.labels = std::move(labels);
    p.step = step;
    return p;
}

void TestSwitches()
{
    Expect(uic::IsSwitchParam(Param(0, 1, "toggle")), "a toggle is a switch");
    Expect(uic::IsSwitchParam(Param(0, 1, "", {"Independent", "Linked"})), "so is a 0..1 pair of labels");
    Expect(!uic::IsSwitchParam(Param(0, 2, "enum", {"A", "B", "C"})), "three labels are not");
    Expect(!uic::IsSwitchParam(Param(-80, 0, "dB")), "nor is a level");
}

void TestValues()
{
    Expect(uic::FormatFlyoutParamValue(Param(0, 1, "", {"Independent", "Linked"}), 1.0) == "Linked",
           "a switch reads its label");
    Expect(uic::FormatFlyoutParamValue(Param(0, 1, "toggle"), 0.0) == "Off", "or Off");
    Expect(uic::FormatFlyoutParamValue(Param(0, 2, "enum", {"Gate", "Duck", "Expand"}, 1.0), 1.0) == "Duck",
           "an enum reads its label");
    Expect(uic::FormatFlyoutParamValue(Param(-80, 0, "dB"), -60.0) == "-60 dB", "a wide span has no decimals");
    Expect(uic::FormatFlyoutParamValue(Param(0.1, 50, "ms"), 1.0) == "1.0 ms", "a span within 60 has one");
    Expect(uic::FormatFlyoutParamValue(Param(0, 1, "amount"), 0.5) == "0.50", "amount is no unit, and two places");
}

using Params = std::map<std::string, double>;

bool SameRange(const uic::SemitoneRange& range, double min, double max, double step)
{
    return range.min == min && range.max == max && range.step == step;
}

void TestPitchShiftRange()
{
    Expect(uic::IsPitchShiftType(guitarfx::EffectGuids::kPitchShift), "Pitch Shift by its id");
    Expect(uic::IsPitchShiftType("pitch_shift"), "and by its legacy name");
    Expect(!uic::IsPitchShiftType(guitarfx::EffectGuids::kTranspose), "not Transpose");

    Expect(uic::IsPitchShiftRangeSetting("stepMode") && uic::IsPitchShiftRangeSetting("minSemitones") &&
               uic::IsPitchShiftRangeSetting("maxSemitones"),
           "the range settings reshape the Semitones knob");
    Expect(!uic::IsPitchShiftRangeSetting("semitones") && !uic::IsPitchShiftRangeSetting("mix"),
           "the knob itself and the rest do not");

    Expect(SameRange(uic::SemitoneKnobRange({}), -12, 12, 1), "the full scale in whole semitones by default");
    Expect(SameRange(uic::SemitoneKnobRange({{"minSemitones", 0}, {"maxSemitones", 7}, {"stepMode", 0}}), 0, 7, 0),
           "the node's range, continuous while not snapped");
    Expect(SameRange(uic::SemitoneKnobRange({{"minSemitones", 5}, {"maxSemitones", -30}}), -12, 5, 1),
           "crossed bounds read as one range, clamped to the scale");
}

void TestPitchShiftReconcile()
{
    using uic::ReconcilePitchShiftParams;

    Expect(ReconcilePitchShiftParams({{"semitones", 3}, {"minSemitones", 0}, {"maxSemitones", 7}, {"stepMode", 1}},
                                     "maxSemitones")
               .empty(),
           "nothing moves when the settings agree");
    Expect(ReconcilePitchShiftParams({{"semitones", 9}, {"minSemitones", 0}, {"maxSemitones", 7}}, "maxSemitones") ==
               Params{{"semitones", 7}},
           "a narrowed range pulls the semitones down into it");
    Expect(ReconcilePitchShiftParams({{"semitones", -4}, {"minSemitones", 0}, {"maxSemitones", 7}}, "minSemitones") ==
               Params{{"semitones", 0}},
           "and up into it");

    Expect(ReconcilePitchShiftParams({{"semitones", 2.4}, {"stepMode", 1}}, "stepMode") == Params{{"semitones", 2}},
           "snapping rounds a free value");
    Expect(ReconcilePitchShiftParams({{"semitones", -2.5}, {"stepMode", 1}}, "stepMode") == Params{{"semitones", -3}},
           "halves away from zero, as the engine");
    Expect(ReconcilePitchShiftParams({{"semitones", 2.4}, {"stepMode", 0}}, "stepMode").empty(),
           "a free value stays free unsnapped");

    Expect(ReconcilePitchShiftParams({{"semitones", 7}, {"minSemitones", 7}, {"maxSemitones", 5}}, "minSemitones") ==
               Params{{"maxSemitones", 8}},
           "a raised minimum pushes the maximum a semitone past it");
    Expect(ReconcilePitchShiftParams({{"semitones", 3}, {"minSemitones", 3}, {"maxSemitones", 3}}, "maxSemitones") ==
               Params{{"minSemitones", 2}},
           "a lowered maximum pushes the minimum a semitone under it");

    Expect(ReconcilePitchShiftParams({{"semitones", 12}, {"minSemitones", 12}, {"maxSemitones", 12}},
                                     "minSemitones") == Params{{"minSemitones", 11}},
           "the minimum gives way at the top of the scale");
    Expect(ReconcilePitchShiftParams({{"semitones", -12}, {"minSemitones", -12}, {"maxSemitones", -12}},
                                     "maxSemitones") == Params{{"maxSemitones", -11}},
           "the maximum gives way at the bottom");

    Expect(ReconcilePitchShiftParams({{"minSemitones", 3}, {"maxSemitones", 7}}, "minSemitones") ==
               Params{{"semitones", 3}},
           "an unset semitones (0 st) moves into a range without it");

    Expect(ReconcilePitchShiftParams({{"semitones", 7.6}, {"minSemitones", 0}, {"maxSemitones", 7}, {"stepMode", 0}},
                                     "maxSemitones") == Params{{"semitones", 7}},
           "a free value is held inside the range");
    Expect(ReconcilePitchShiftParams({{"semitones", 3.3}, {"minSemitones", 0}, {"maxSemitones", 7}, {"stepMode", 0}},
                                     "maxSemitones")
               .empty(),
           "and not rounded");
}
} // namespace

int main()
{
    TestSwitches();
    TestValues();
    TestPitchShiftRange();
    TestPitchShiftReconcile();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ParamFormatTests passed" << std::endl;
    return 0;
}
