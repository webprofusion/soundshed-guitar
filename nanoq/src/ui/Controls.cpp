// "Input and output": the global chain around every preset. Input level, mono and dual mono,
// the noise gate, a global transpose, a four band EQ, the doubler, output level, mute and the
// output limiter. A dialog that stays open while it is adjusted, patched in place.

#include "ui/Shell.h"

#include "uiclient/ParamFormat.h"

#include <cmath>
#include <functional>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectParamInfo;
using guitarfx::uiclient::Topic;

namespace
{
constexpr const char* kLimiterSetting = "audio.dsp.outputLimiterEnabled";

EffectParamInfo Param(const char* key, const char* name, const char* unit, double lo, double hi, double def, double step = 0.0)
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
        if (const auto it = node->params.find(key); it != node->params.end())
            return it->second;
    return fallback;
}

bool NodeEnabled(const guitarfx::SignalGraph& graph, const char* id)
{
    const auto* node = graph.FindNode(id);
    return node != nullptr && node->enabled;
}
} // namespace

void Shell::OpenControls()
{
    auto& commands = mSession.Commands();
    auto syncs = std::make_shared<std::vector<std::function<void()>>>();
    const auto chain = [this]() -> const guitarfx::GlobalSignalChainConfig& { return mState.globalChain; };

    std::vector<Ptr> page;

    auto heading = [this, &page](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 11.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        page.push_back(FixedHeight(30, PadXY(4, 6, t)));
    };

    // A row of switches, then a row of sliders.
    struct Section
    {
        std::vector<Ptr> switches;
        std::vector<Ptr> sliders;
    };
    Section section;

    auto flush = [&] {
        if (!section.switches.empty())
        {
            std::vector<Ptr> row;
            for (auto& s : section.switches)
            {
                row.push_back(s);
                row.push_back(HGap(8));
            }
            row.push_back(Stretch());
            page.push_back(FixedHeight(44, Row(row)));
        }
        if (!section.sliders.empty())
        {
            std::vector<Ptr> row;
            for (auto& s : section.sliders)
            {
                if (!row.empty())
                    row.push_back(HGap(16));
                row.push_back(s);
            }
            if (row.size() == 1)
            {
                // A lone slider keeps a column's width rather than spanning the dialog.
                row.push_back(HGap(16));
                row.push_back(Stretch());
            }
            page.push_back(FixedHeight(60, Row(row)));
        }
        section = {};
    };

    auto addSwitch = [&](const std::string& text, std::function<bool()> read, std::function<void(bool)> write) {
        auto button = std::make_shared<Button>(mTheme, text, [read, write] { write(!read()); }, Button::Style::Plain);
        button->Dot(true).MinWidth(150);
        section.switches.push_back(button);
        syncs->push_back([this, button, read, text] {
            button->On(read());
            mSession.View().refresh(*button);
        });
    };

    auto addSlider = [&](const EffectParamInfo& info, std::function<double()> read, std::function<void(double)> write,
                         std::function<std::string(double)> format = {}) {
        auto slider = std::make_shared<ParamSlider>(mTheme, info.name);
        slider->onChange = [info, write](double position) {
            write(guitarfx::uiclient::SnapParamValue(info, guitarfx::uiclient::ParamPositionToValue(info, position)));
        };
        slider->onReset = [info, write] { write(info.defaultValue); };
        section.sliders.push_back(slider);
        syncs->push_back([this, slider, info, read, format] {
            const double v = read();
            slider->Update(guitarfx::uiclient::ParamValueToPosition(info, v),
                           format ? format(v) : guitarfx::uiclient::FormatParamValue(info, v));
            mSession.View().refresh(*slider);
        });
    };

    // ── Input ──
    heading("INPUT");
    addSlider(Param("inputGain", "Level", "dB", -12.0, 12.0, 0.0), [chain] { return chain().inputGain; },
              [&commands](double v) { commands.SetGlobalChainParam("input.gain", v); });

    // In the standalone app the player picks mono (input 1, input 2 or both summed), stereo or
    // dual mono; in a DAW the track decides mono or stereo, and dual mono is the one choice left.
    if (mState.environment.standalone)
    {
        addSwitch("Mono input", [chain] { return chain().monoMode; },
                  [this, chain](bool mono) { mSession.Commands().SetInputMode(mono, chain().inputChannel, chain().dualMono); });
        addSwitch("Input 2", [chain] { return chain().inputChannel == 1; },
                  [this, chain](bool right) { mSession.Commands().SetInputMode(chain().monoMode, right ? 1 : 0, chain().dualMono); });
        addSwitch("Both inputs summed", [chain] { return chain().inputChannel == 2; },
                  [this, chain](bool sum) { mSession.Commands().SetInputMode(chain().monoMode, sum ? 2 : 0, chain().dualMono); });
    }
    addSwitch("Dual mono", [chain] { return chain().dualMono; }, [this, chain](bool dual) {
        // Dual mono needs both inputs kept apart, so it turns mono off.
        mSession.Commands().SetInputMode(dual ? false : chain().monoMode, chain().inputChannel, dual);
    });
    flush();

    // ── Noise gate ──
    heading("NOISE GATE");
    addSwitch("On", [chain] { return NodeEnabled(chain().preChainGraph, "global_gate"); },
              [&commands](bool on) { commands.SetGlobalChainToggle("gate.enabled", on); });
    addSlider(Param("threshold", "Threshold", "dB", -90.0, 0.0, -60.0, 1.0),
              [chain] { return NodeParam(chain().preChainGraph, "global_gate", "threshold", -60.0); },
              [&commands](double v) { commands.SetGlobalChainParam("gate.threshold", v); });
    flush();

    // ── Transpose ──
    heading("TRANSPOSE");
    addSlider(Param("semitones", "Semitones", " st", -12.0, 12.0, 0.0, 1.0),
              [chain] {
                  return NodeEnabled(chain().preChainGraph, "global_transpose")
                             ? NodeParam(chain().preChainGraph, "global_transpose", "semitones", 0.0)
                             : 0.0;
              },
              [&commands](double v) {
                  // As the web UI's knob: the transpose runs only while it transposes.
                  const auto semitones = std::round(v);
                  commands.SetGlobalChainParam("transpose.semitones", semitones);
                  commands.SetGlobalChainToggle("transpose.enabled", semitones != 0.0);
              },
              [](double v) {
                  const int semitones = static_cast<int>(std::lround(v));
                  return std::string(semitones > 0 ? "+" : "") + std::to_string(semitones) + " st";
              });
    flush();

    // ── Global EQ ──
    heading("GLOBAL EQ");
    addSwitch("On", [chain] { return NodeEnabled(chain().postChainGraph, "global_eq"); },
              [&commands](bool on) { commands.SetGlobalChainToggle("eq.enabled", on); });
    for (const auto& [key, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"lowGain", "Low"}, {"lowMidGain", "Low mid"}, {"highMidGain", "High mid"}, {"highGain", "High"}})
    {
        const std::string path = std::string("eq.") + key;
        addSlider(Param(key, name, "dB", -12.0, 12.0, 0.0),
                  [chain, key = key] { return NodeParam(chain().postChainGraph, "global_eq", key, 0.0); },
                  [&commands, path](double v) { commands.SetGlobalChainParam(path, v); });
        if (section.sliders.size() == 2)
            flush(); // the EQ bands, two to a row
    }
    flush();

    // ── Doubler ──
    heading("DOUBLER");
    addSwitch("On", [chain] { return NodeEnabled(chain().postChainGraph, "global_doubler"); },
              [&commands](bool on) { commands.SetGlobalChainToggle("doubler.enabled", on); });
    addSlider(Param("time", "Delay", "ms", 0.5, 50.0, 6.0),
              [chain] { return NodeParam(chain().postChainGraph, "global_doubler", "time", 6.0); },
              [&commands](double v) { commands.SetGlobalChainParam("doubler.delay", v); });
    addSlider(Param("mix", "Mix", "amount", 0.0, 1.0, 0.5),
              [chain] { return NodeParam(chain().postChainGraph, "global_doubler", "mix", 0.5); },
              [&commands](double v) { commands.SetGlobalChainParam("doubler.mix", v); });
    flush();

    // ── Output ──
    heading("OUTPUT");
    addSlider(Param("outputGain", "Level", "dB", -12.0, 12.0, 0.0), [chain] { return chain().outputGain; },
              [&commands](double v) { commands.SetGlobalChainParam("output.gain", v); });
    addSwitch("Mute", [this] { return mState.outputMuted; }, [&commands](bool muted) { commands.SetOutputMuted(muted); });
    addSwitch("Limiter", [this] { return mState.appSettings.value(kLimiterSetting, false); },
              [&commands](bool on) { commands.SetSetting(kLimiterSetting, on); });
    flush();

    auto title = std::make_shared<Text>(mTheme, "Input and output", 15.0f, Weight::SemiBold);
    auto done = std::make_shared<Button>(mTheme, "Done", [this] { CloseOverlay(); }, Button::Style::Plain);
    done->MinWidth(88);

    const auto view = mSession.View().size();
    ShowOverlay(Pad(16, 14, 16, 12,
                    Col({Row({FixedHeight(30, title), Stretch(nullptr, 0.001), done}), VScroll(Col(page))})),
                std::min(620.0f, std::max(320.0f, view.x - 48.0f)), std::max(280.0f, view.y - 60.0f));

    auto sync = [syncs] {
        for (auto& fn : *syncs)
            fn();
    };
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::GlobalChain, sync));
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Session, sync));
    sync();
}
} // namespace nanoq::ui
