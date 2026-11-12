// Controls: the global chain around every preset, as the web UI's compact Controls strip drops it
// under the preset bar (ui-components/control-bar.html): the input meter and level, with which
// inputs feed it; the noise gate; a global transpose; the four band EQ; the doubler; the output
// level and meter, and mute. It stays open while it is adjusted, bound in place. The output
// limiter is in Settings. The full layout's control bar (WideFrame.cpp) lays out the same cells.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"

#include "uiclient/ParamFormat.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectParamInfo;
using guitarfx::uiclient::Topic;

namespace
{
EffectParamInfo Param(const char* key, const char* name, const char* unit, double lo, double hi, double def,
                      double step = 0.0)
{
    EffectParamInfo info;
    info.key = key;
    info.name = name;
    info.unit = unit;
    info.minValue = lo;
    info.maxValue = hi;
    info.defaultValue = def;
    info.step = step;
    return info;
}

double NodeParam(const guitarfx::SignalGraph& graph, const char* id, const char* key, double fallback)
{
    if (const auto* node = graph.FindNode(id))
    {
        if (const auto it = node->params.find(key); it != node->params.end())
        {
            return it->second;
        }
    }

    return fallback;
}

bool NodeEnabled(const guitarfx::SignalGraph& graph, const char* id)
{
    const auto* node = graph.FindNode(id);
    return node != nullptr && node->enabled;
}

std::string Decibels(double v)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%+.1f dB", v);
    return text;
}
} // namespace

Shell::ControlCells Shell::BuildControlCells()
{
    auto& commands = mSession.Commands();
    const auto chain = [this]() -> const guitarfx::GlobalSignalChainConfig& { return mState.globalChain; };
    using Read = std::function<double()>;
    using Write = std::function<void(double)>;
    using Format = std::function<std::string(double)>;
    // Touch sizes: the labels at 11 px, and a 36 px row each for a label with its switch (the
    // two one control, MiniSwitch::Title) and for the drop-down under the knob.
    const bool touch = metrics::Touch();
    const float titleSize = metrics::Readable(9.0f);
    const float headHeight = touch ? 36.0f : 12.0f;
    const float belowHeight = touch ? 36.0f : 16.0f;
    const auto titleInk = mTheme.Pick(el::rgba(201, 209, 225, 200), mTheme.TextSecondary());

    // A cell of the strip: its label (with a switch beside it for a stage that turns off) over a
    // knob and its value, and, for the input, its mode under that.
    auto knobCell = [&](const char* label, const EffectParamInfo& info, Read read, Write write, Format format,
                        std::function<bool()> readOn = {}, std::function<void(bool)> writeOn = {},
                        Ptr below = nullptr) -> Ptr {
        auto knob = std::make_shared<ParamKnob>(mTheme, "");
        knob->AccessName(info.name);
        knob->DefaultPosition(guitarfx::uiclient::ParamValueToPosition(info, info.defaultValue));
        knob->onChange = [info, write, format, k = knob.get()](double position) {
            const double v =
                guitarfx::uiclient::SnapParamValue(info, guitarfx::uiclient::ParamPositionToValue(info, position));
            k->SetValueText(format(v));
            write(v);
        };
        knob->onReset = [info, write] { write(info.defaultValue); };
        // A value typed in where its value text is, as the web UI's knob does.
        knob->editText = [info, read] { return guitarfx::uiclient::EditableParamText(info, read()); };
        knob->onTyped = [info, write](const std::string& entered) {
            if (const auto typed = guitarfx::uiclient::ParseTypedNumber(entered))
            {
                write(guitarfx::uiclient::SnapParamValue(info, std::clamp(*typed, info.minValue, info.maxValue)));
            }
        };

        // The input and output levels take a MIDI mapping, as the web UI's control-bar knobs do.
        if (std::string_view(label) == "IN" || std::string_view(label) == "OUT")
        {
            knob->onMenu = [this, which = std::string(std::string_view(label) == "IN" ? "input" : "output")](
                               el::point at) { OpenGlobalKnobMidiMenu(which, at); };
        }

        mLive.Bind(knob, read, [info, format](ParamKnob& k, double v) {
            k.Update(guitarfx::uiclient::ParamValueToPosition(info, v), format(v));
        });

        auto title = std::make_shared<Text>(mTheme, label, titleSize, Weight::SemiBold);
        title->Colour(titleInk).Fixed(true);
        std::vector<Ptr> head{Stretch(), title};

        if (readOn)
        {
            auto toggle = std::make_shared<MiniSwitch>(mTheme, readOn());
            toggle->AccessName(std::string(label) + " on");
            toggle->onClick = [readOn, writeOn] { writeOn(!readOn()); };
            mLive.Bind(toggle, readOn, &MiniSwitch::Set);

            if (touch)
            {
                toggle->Title(label, titleSize, titleInk);
                head = {toggle};
            }
            else
            {
                head.push_back(HGap(5));
                head.push_back(toggle);
            }
        }

        if (!touch || !readOn)
        {
            head.push_back(Stretch());
        }

        // The knob draws no name of its own: the label row sits over the space it leaves (and
        // with touch sizes over a gap above it too).
        const float knobTop = headHeight - 12.0f;
        Ptr knobRow = Row({Stretch(), knob, Stretch()});
        auto top = Over(Col({FixedHeight(headHeight, Row(head)), std::make_shared<el::element>()}),
                        touch ? Col({VGap(knobTop), knobRow}) : knobRow);
        std::vector<Ptr> cell{FixedHeight(67 + knobTop, top)};

        if (below)
        {
            cell.push_back(VGap(1));
            cell.push_back(FixedHeight(belowHeight, Row({Stretch(), below, Stretch()})));
        }

        cell.push_back(std::make_shared<el::element>());
        return FixedWidth(84, Col(cell));
    };

    // ── IN: the level, and which inputs feed it ──
    auto inputModeLabel = [chain, this] {
        const auto& c = chain();

        if (c.dualMono)
        {
            return std::string("Dual mono");
        }

        if (!c.monoMode)
        {
            return std::string(mState.environment.standalone ? "Stereo" : "Track");
        }

        return std::string(c.inputChannel == 1 ? "Mono In 2" : c.inputChannel == 2 ? "Mono 1+2" : "Mono In 1");
    };
    auto mode = std::make_shared<IconButton>(mTheme, mIcons, "");
    mode->Size(70, 16).Label(inputModeLabel(), 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    mode->onClick = [this, chain, m = mode.get()] {
        auto set = [this](bool mono, int channel, bool dual) {
            return [this, mono, channel, dual] { mSession.Commands().SetInputMode(mono, channel, dual); };
        };
        const auto& c = chain();
        // As the web UI's input mode flyout (inputMode.ts): each mode with its routing icon, its
        // name and what it does.
        const auto mode = [](std::string name, std::string description, std::string icon, bool chosen,
                             std::function<void()> choose) {
            MenuItem item{std::move(name), chosen, false, std::move(choose)};
            item.icon = std::move(icon);
            item.subtitle = std::move(description);
            return item;
        };
        std::vector<MenuItem> items{{"Input mode", false, true, {}}};

        // In the standalone app the player picks mono (input 1, input 2 or both summed), stereo or
        // dual mono; in a DAW the track decides mono or stereo, and dual mono is the one choice left.
        if (mState.environment.standalone)
        {
            const bool mono = c.monoMode && !c.dualMono;
            items.push_back(mode("Mono In 1", "Input 1 on both sides", "input-mono1", mono && c.inputChannel == 0,
                                 set(true, 0, false)));
            items.push_back(mode("Mono In 2", "Input 2 on both sides", "input-mono2", mono && c.inputChannel == 1,
                                 set(true, 1, false)));
            items.push_back(mode("Mono 1+2", "Inputs 1 and 2 summed", "input-monoSum", mono && c.inputChannel == 2,
                                 set(true, 2, false)));
            items.push_back(mode("Stereo", "Input 1 left, input 2 right", "input-stereo", !c.monoMode && !c.dualMono,
                                 set(false, c.inputChannel, false)));
        }
        else
        {
            items.push_back(
                mode("Track", "As the track is", "input-stereo", !c.dualMono, set(c.monoMode, c.inputChannel, false)));
        }

        // Dual mono needs both inputs kept apart, so it turns mono off.
        items.push_back(mode("Dual mono", "A separate rig per input", "input-dualMono", c.dualMono,
                             set(false, c.inputChannel, true)));
        OpenDropdown(m->Bounds(), std::move(items), 250.0f);
    };
    mLive.Bind(mode, inputModeLabel, [](IconButton& m, const std::string& label) { m.Label(label, 10.0f); });

    auto in = knobCell(
        "IN", Param("inputGain", "In", "dB", -12.0, 12.0, 0.0), [chain] { return chain().inputGain; },
        [&commands](double v) { commands.SetGlobalChainParam("input.gain", v); }, Decibels, {}, {}, mode);

    // ── GATE ──
    // ── GATE: Threshold and on/off here; everything else, and Learn, behind Settings ──
    auto gateSettings = std::make_shared<IconButton>(mTheme, mIcons, "");
    gateSettings->Size(70, 16).Label("Settings", 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    gateSettings->onClick = [this, g = gateSettings.get()] { OpenGlobalGate(g->Bounds()); };
    auto gate = knobCell(
        "GATE", Param("threshold", "Gate", "dB", -90.0, 0.0, -60.0, 1.0),
        [chain] { return NodeParam(chain().preChainGraph, "global_gate", "threshold", -60.0); },
        [&commands](double v) { commands.SetGlobalChainParam("gate.threshold", v); },
        [](double v) { return std::to_string(static_cast<int>(std::lround(v))) + " dB"; },
        [chain] { return NodeEnabled(chain().preChainGraph, "global_gate"); },
        [&commands](bool on) { commands.SetGlobalChainToggle("gate.enabled", on); }, gateSettings);

    // ── TRANSPOSE: as the web UI's knob, the transpose runs only while it transposes ──
    auto transpose = knobCell(
        "TRANSPOSE", Param("semitones", "Transpose", " st", -12.0, 12.0, 0.0, 1.0),
        [chain] {
            return NodeEnabled(chain().preChainGraph, "global_transpose")
                       ? NodeParam(chain().preChainGraph, "global_transpose", "semitones", 0.0)
                       : 0.0;
        },
        [&commands](double v) {
            const auto semitones = std::round(v);
            commands.SetGlobalChainParam("transpose.semitones", semitones);
            commands.SetGlobalChainToggle("transpose.enabled", semitones != 0.0);
        },
        [](double v) {
            const int semitones = static_cast<int>(std::lround(v));
            return std::string(semitones > 0 ? "+" : "") + std::to_string(semitones) + " st";
        });

    // ── EQ: its switch, and a button that opens its four bands ──
    auto eqButton = std::make_shared<IconButton>(mTheme, mIcons, "sliders", IconButton::Look::Boxed);
    eqButton->Size(46, 46).IconSize(18).Ink(mTheme.TextSecondary());
    eqButton->onClick = [this] { OpenGlobalEq(); };
    // Lit in the accent while the Global EQ is on, as the web UI's .eq-icon-btn.
    mLive.Bind(
        eqButton, [chain] { return NodeEnabled(chain().postChainGraph, "global_eq"); },
        [](IconButton& b, bool on) { b.Lit(on); });
    auto eqOn = std::make_shared<MiniSwitch>(mTheme, NodeEnabled(chain().postChainGraph, "global_eq"));
    eqOn->AccessName("EQ on");
    eqOn->onClick = [this, chain] {
        mSession.Commands().SetGlobalChainToggle("eq.enabled", !NodeEnabled(chain().postChainGraph, "global_eq"));
    };
    mLive.Bind(eqOn, [chain] { return NodeEnabled(chain().postChainGraph, "global_eq"); }, &MiniSwitch::Set);
    auto eqTitle = std::make_shared<Text>(mTheme, "EQ", titleSize, Weight::SemiBold);
    eqTitle->Colour(titleInk).Fixed(true);
    Ptr eqHead = Row({Stretch(), eqTitle, HGap(5), eqOn, Stretch()});

    if (touch)
    {
        eqOn->Title("EQ", titleSize, titleInk);
        eqHead = Row({eqOn});
    }

    auto eq =
        FixedWidth(72, Col({FixedHeight(headHeight, eqHead), VGap(touch ? 4 : 8),
                            FixedHeight(46, Row({Stretch(), eqButton, Stretch()})), std::make_shared<el::element>()}));

    // ── DOUBLER ──
    auto doubler = knobCell(
        "DOUBLER", Param("time", "Doubler", "ms", 0.5, 50.0, 6.0),
        [chain] { return NodeParam(chain().postChainGraph, "global_doubler", "time", 6.0); },
        [&commands](double v) { commands.SetGlobalChainParam("doubler.delay", v); },
        [](double v) {
            char text[24];
            std::snprintf(text, sizeof(text), "%.2f ms", v);
            return std::string(text);
        },
        [chain] { return NodeEnabled(chain().postChainGraph, "global_doubler"); },
        [&commands](bool on) { commands.SetGlobalChainToggle("doubler.enabled", on); });

    // ── OUT, with mute under its meter, and the output's status readout behind Status ──
    auto outStatus = std::make_shared<IconButton>(mTheme, mIcons, "");
    outStatus->Size(70, 16).Label("Status", 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    outStatus->onClick = [this, o = outStatus.get()] { OpenOutputStatus(o->Bounds()); };
    auto out = knobCell(
        "OUT", Param("outputGain", "Out", "dB", -12.0, 12.0, 0.0), [chain] { return chain().outputGain; },
        [&commands](double v) { commands.SetGlobalChainParam("output.gain", v); }, Decibels, {}, {}, outStatus);

    // The meters, from the telemetry feed (20 Hz): only a bar that moved is redrawn.
    auto inMeter = std::make_shared<MeterBar>(mTheme);
    inMeter->Vertical(true);
    mLive.Feed(inMeter,
               [this](MeterBar& m) { return m.Set(mState.telemetry.input.peakDb, mState.telemetry.input.clipped); });
    auto outMeter = std::make_shared<MeterBar>(mTheme);
    outMeter->Vertical(true);
    mLive.Feed(outMeter,
               [this](MeterBar& m) { return m.Set(mState.telemetry.output.peakDb, mState.telemetry.output.clipped); });
    auto mute = std::make_shared<IconButton>(mTheme, mIcons, mState.outputMuted ? "ui-mute" : "ui-speaker");
    mute->Size(20, 18).IconSize(12).TouchTarget(36);
    mute->onClick = [this] { mSession.Commands().SetOutputMuted(!mState.outputMuted); };
    mLive.Bind(mute, [this] { return mState.outputMuted; }, &IconButton::Lit);

    ControlCells cells;
    cells.in = in;
    cells.gate = gate;
    cells.transpose = transpose;
    cells.eq = eq;
    cells.doubler = doubler;
    cells.out = out;
    cells.inMeter = inMeter;
    cells.outMeter = outMeter;
    cells.mute = mute;
    cells.levelling = BuildInputLevellingButton();
    return cells;
}

void Shell::OpenControls()
{
    auto c = BuildControlCells();
    // Touch sizes: the chevron and mute under the meters take 36 px rows, as the cells' do.
    const float under = metrics::Touch() ? 36.0f : 18.0f;
    // and the gaps beside the meters close up, so the strip is no wider than without them.
    const float meterGap = metrics::Touch() ? 8.0f : 14.0f;
    auto strip =
        Row({Stretch(),
             Col({FixedHeight(84, c.inMeter), VGap(2), FixedHeight(under, Row({Stretch(), c.levelling, Stretch()})),
                  std::make_shared<el::element>()}),
             HGap(meterGap), c.in, c.gate, c.transpose, Stretch(), c.eq, c.doubler, c.out, HGap(meterGap),
             Col({FixedHeight(84, c.outMeter), VGap(2), FixedHeight(under, Row({Stretch(), c.mute, Stretch()})),
                  std::make_shared<el::element>()}),
             Stretch()});

    const auto view = ViewSize();
    ShowPopover(Pad(8, 10, 8, 6, strip),
                el::rect{metrics::kRailWidth + 6.0f, 0.0f, view.x - 6.0f, metrics::TopBarHeight() - 6.0f},
                view.x - metrics::kRailWidth - 12.0f, 124.0f + (metrics::Touch() ? metrics::kTouchCellExtra : 0.0f));
    // The bar's CONTROLS stays lit while the strip is out, as the web UI's tab does.
    mControlsOpen = true;
    mOnOverlayClosed = [this] { mControlsOpen = false; };
}
} // namespace nanoq::ui
