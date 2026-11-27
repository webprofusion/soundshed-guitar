// Controls: the global chain around every preset, as the web UI's compact Controls strip drops it
// under the preset bar (ui-components/control-bar.html): the input meter and level, with which
// inputs feed it; the noise gate; a global transpose; the four band EQ; the doubler; the output
// level and meter, and mute. It stays open while it is adjusted, bound in place. The output
// limiter is in Settings. The full layout's control bar (WideFrame.cpp) lays out the same cells.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"

#include "uiclient/InputMode.h"
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

Shell::ControlCells Shell::BuildControlCells(float cellWidth)
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
    // .control-stack-label: 9 px bold, spaced .08em, --text-muted outside Dark.
    const auto titleInk = mTheme.DarkOr(el::rgba(201, 209, 225, 200), mTheme.TextMuted());
    constexpr float kTitleTracking = 0.08f;
    // The small pills of the bar's flyouts and the output's mute (18 x 16).
    auto pill = [this](const char* icon, float iconSize) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Pill);
        b->Size(18, 16).IconSize(iconSize).TouchTarget(36);
        return b;
    };

    // A cell of the strip: its label (with a switch beside it for a stage that turns off) over a
    // knob and its value, and, for the input, its mode under that.
    auto knobCell = [&](const char* label, const EffectParamInfo& info, Read read, Write write, Format format,
                        std::function<bool()> readOn = {}, std::function<void(bool)> writeOn = {}, Ptr below = nullptr,
                        Ptr besideValue = nullptr) -> Ptr {
        auto knob = std::make_shared<ParamKnob>(mTheme, "");
        knob->AccessName(info.name);
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
        title->Colour(titleInk).Tracking(kTitleTracking).Fixed(true);
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

        // A flyout's chevron beside the value, as the gate's (.gate-settings-trigger).
        if (besideValue)
        {
            top = Over(Col({Stretch(), FixedHeight(16, Row({Stretch(), std::move(besideValue), HGap(3)})), VGap(4)}),
                       top);
        }

        std::vector<Ptr> cell{FixedHeight(67 + knobTop, top)};

        if (below)
        {
            cell.push_back(VGap(1));
            cell.push_back(FixedHeight(belowHeight, Row({Stretch(), below, Stretch()})));
        }

        cell.push_back(std::make_shared<el::element>());
        return FixedWidth(cellWidth, Col(cell));
    };

    // ── IN: the level, and which inputs feed it ──
    // As the web UI's input mode control (inputMode.ts, uiclient's InputMode.h): the trigger names
    // the mode, in the warning colour when what runs is not what was chosen, and says why under
    // the pointer.
    namespace im = guitarfx::uiclient::input_mode;
    auto currentMode = [chain] {
        const auto& c = chain();
        return im::FromSettings(c.monoMode, c.inputChannel, c.dualMono);
    };

    struct ModeLook
    {
        std::string label;
        std::string tooltip;
        bool fallback = false;

        bool operator==(const ModeLook&) const = default;
    };

    auto modeLook = [this, currentMode] {
        const auto& caps = mState.inputCapabilities;
        const auto current = currentMode();
        const auto shown = im::Displayed(current, caps);
        return ModeLook{im::Present(shown, caps).name, im::Tooltip(current, caps), im::FallsBack(shown, caps)};
    };
    auto mode = std::make_shared<IconButton>(mTheme, mIcons, "");
    mode->Size(70, 16).Label(modeLook().label, 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    mode->onClick = [this, chain, currentMode, m = mode.get()] {
        const auto& c = chain();
        const auto& caps = mState.inputCapabilities;
        const auto shown = im::Displayed(currentMode(), caps);
        MenuItem title{"Input mode", false, true, {}};

        // Said only when it is not what the ticked mode would run: Stereo on a one-input device.
        if (im::FallsBack(shown, caps))
        {
            title.detail = "Running " + Lower(im::EffectiveName(caps));
        }

        std::vector<MenuItem> items{std::move(title)};

        // In the standalone app the player picks mono (input 1, input 2 or both summed), stereo or
        // dual mono; in a DAW the track decides mono or stereo, and dual mono is the one choice
        // left. A mode the input or output cannot carry is greyed, with what it needs.
        for (const auto listed : im::Listed(caps))
        {
            const auto look = im::Present(listed, caps);
            const auto reason = im::UnavailableReason(listed, caps);
            auto settings = im::ToSettings(listed);

            // In a DAW, the track keeps the mono choice it had.
            if (caps.hostControlled && listed == im::Mode::Stereo)
            {
                settings = {c.monoMode, c.inputChannel, false};
            }

            MenuItem item{look.name, listed == shown, false, [this, settings] {
                              mSession.Commands().SetInputMode(settings.monoMode, settings.inputChannel,
                                                               settings.dualMono);
                          }};
            item.icon = "input-" + look.icon;
            item.subtitle = reason.value_or(look.description);
            item.disabled = reason.has_value();
            items.push_back(std::move(item));
        }

        if (const auto note = im::StatusNote(currentMode(), caps); !note.empty())
        {
            MenuItem remark{note, false, false, {}};
            remark.note = true;
            items.push_back(std::move(remark));
        }

        OpenDropdown(m->Bounds(), std::move(items), 250.0f);
    };
    mLive.Bind(mode, modeLook, [this](IconButton& m, const ModeLook& look) {
        m.Label(look.label, 10.0f).Ink(look.fallback ? mTheme.Warning() : mTheme.TextSecondary());
        m.Tooltip(look.tooltip);
    });

    auto in = knobCell(
        "IN", Param("inputGain", "In", "dB", -12.0, 12.0, 0.0), [chain] { return chain().inputGain; },
        [&commands](double v) { commands.SetGlobalChainParam("input.gain", v); }, Decibels, {}, {}, mode);

    // ── GATE ──
    // ── GATE: Threshold and on/off here; everything else, and Learn, behind Settings ──
    auto gateSettings = pill("ui-chevron-down", 9.0f);
    gateSettings->AccessName("Noise gate settings");
    gateSettings->onClick = [this, g = gateSettings.get()] { OpenGlobalGate(g->Bounds()); };
    auto gate = knobCell(
        "GATE", Param("threshold", "Gate", "dB", -90.0, 0.0, -60.0, 1.0),
        [chain] { return NodeParam(chain().preChainGraph, "global_gate", "threshold", -60.0); },
        [&commands](double v) { commands.SetGlobalChainParam("gate.threshold", v); },
        [](double v) { return std::to_string(static_cast<int>(std::lround(v))) + " dB"; },
        [chain] { return NodeEnabled(chain().preChainGraph, "global_gate"); },
        [&commands](bool on) { commands.SetGlobalChainToggle("gate.enabled", on); }, nullptr, gateSettings);

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
    // .eq-icon-btn: 48 x 48 with the web UI's bars, glowing in the accent while the EQ is on.
    auto eqButton = std::make_shared<IconButton>(mTheme, mIcons, "ui-eq-bars", IconButton::Look::Well);
    eqButton->Size(48, 48).IconSize(18).AccessName("Global EQ");
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
    eqTitle->Colour(titleInk).Tracking(kTitleTracking).Fixed(true);
    Ptr eqHead = Row({Stretch(), eqTitle, HGap(5), eqOn, Stretch()});

    if (touch)
    {
        eqOn->Title("EQ", titleSize, titleInk);
        eqHead = Row({eqOn});
    }

    auto eq =
        FixedWidth(72, Col({FixedHeight(headHeight, eqHead), VGap(touch ? 4 : 7),
                            FixedHeight(48, Row({Stretch(), eqButton, Stretch()})), std::make_shared<el::element>()}));

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

    // ── OUT; its mute and the output's status chevron sit under its meter (.output-vu-meter) ──
    auto outStatus = pill("ui-chevron-down", 9.0f);
    outStatus->AccessName("Output DSP status");
    outStatus->onClick = [this, o = outStatus.get()] { OpenOutputStatus(o->Bounds()); };
    auto out = knobCell(
        "OUT", Param("outputGain", "Out", "dB", -12.0, 12.0, 0.0), [chain] { return chain().outputGain; },
        [&commands](double v) { commands.SetGlobalChainParam("output.gain", v); }, Decibels);

    // The meters, from the telemetry feed (20 Hz): only a bar that moved is redrawn.
    auto inMeter = std::make_shared<MeterBar>(mTheme);
    inMeter->Vertical(true);
    mLive.Feed(inMeter, [this](MeterBar& m) {
        return m.Set(mState.telemetry.input.peakDb, mState.telemetry.input.clipped, mState.telemetry.input.rmsDb);
    });
    auto outMeter = std::make_shared<MeterBar>(mTheme);
    outMeter->Vertical(true);
    mLive.Feed(outMeter, [this](MeterBar& m) {
        return m.Set(mState.telemetry.output.peakDb, mState.telemetry.output.clipped, mState.telemetry.output.rmsDb);
    });
    // .output-quick-mute-btn: coral while muted, with the wave struck through.
    auto mute = pill(mState.outputMuted ? "ui-out-muted" : "ui-out-speaker", 11.0f);
    mute->LitColour(el::rgba(240, 138, 101, 255)).AccessName("Mute output");
    mute->onClick = [this] { mSession.Commands().SetOutputMuted(!mState.outputMuted); };
    mLive.Bind(
        mute, [this] { return mState.outputMuted; },
        [](IconButton& b, bool muted) { b.Lit(muted).Icon(muted ? "ui-out-muted" : "ui-out-speaker"); });

    ControlCells cells;
    cells.in = in;
    cells.gate = gate;
    cells.transpose = transpose;
    cells.eq = eq;
    cells.doubler = doubler;
    cells.out = out;
    cells.inMeter = inMeter;
    cells.outMeter = outMeter;
    cells.mute = Tight({mute, HGap(3), outStatus});
    cells.levelling = BuildInputLevellingButton();
    return cells;
}

void Shell::OpenControls()
{
    // Touch sizes: the chevron and mute under the meters take 36 px rows, as the cells' do.
    const float under = metrics::Touch() ? 36.0f : 18.0f;
    // and the gaps beside the meters close up, so the strip is no wider than without them.
    const float meterGap = metrics::Touch() ? 8.0f : 14.0f;
    // The five knob cells narrow (down to their 70 px drop-downs) where the strip would run past
    // the window: at 640 wide it needed 28 px more than the popover has. The EQ cell is 72, a meter
    // column about 30 (36 with touch sizes), and the popover's sides 6 and its padding 8 each.
    const auto view = ViewSize();
    // Under the stacked frame's tab bar; the rail's preset bar is at the top.
    const float top = mStack ? metrics::kStackTabBarHeight : 0.0f;
    const float room = view.x - StageLeft() - 12.0f - 16.0f;
    const float fixed = 72.0f + 2.0f * meterGap + 2.0f * (metrics::Touch() ? 36.0f : 30.0f) + 2.0f;
    auto c = BuildControlCells(std::clamp(std::floor((room - fixed) / 5.0f), 72.0f, 84.0f));
    auto strip =
        Row({Stretch(),
             Col({FixedHeight(84, c.inMeter), VGap(2), FixedHeight(under, Row({Stretch(), c.levelling, Stretch()})),
                  std::make_shared<el::element>()}),
             HGap(meterGap), c.in, c.gate, c.transpose, Stretch(), c.eq, c.doubler, c.out, HGap(meterGap),
             Col({FixedHeight(84, c.outMeter), VGap(2), FixedHeight(under, Row({Stretch(), c.mute, Stretch()})),
                  std::make_shared<el::element>()}),
             Stretch()});

    ShowPopover(Pad(8, 10, 8, 6, strip),
                el::rect{StageLeft() + 6.0f, top, view.x - 6.0f, top + metrics::TopBarHeight() - 6.0f},
                view.x - StageLeft() - 12.0f, 124.0f + (metrics::Touch() ? metrics::kTouchCellExtra : 0.0f));
    // The bar's CONTROLS stays lit while the strip is out, as the web UI's tab does.
    mControlsOpen = true;
    mOnOverlayClosed = [this] { mControlsOpen = false; };
}
} // namespace nanoq::ui
