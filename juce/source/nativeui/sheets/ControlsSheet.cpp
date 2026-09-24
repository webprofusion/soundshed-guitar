#include "nativeui/sheets/ControlsSheet.h"

#include "uiclient/ParamFormat.h"

#include <array>

namespace soundshed::nano
{
using guitarfx::uiclient::EffectParamInfo;

namespace
{
constexpr const char* kLimiterSetting = "audio.dsp.outputLimiterEnabled";

EffectParamInfo param (const char* key, const char* name, const char* unit, double lo, double hi, double def, double step = 0.0)
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

const guitarfx::GraphNode* findNode (const guitarfx::SignalGraph& graph, const char* id)
{
    return graph.FindNode (id);
}

double nodeParam (const guitarfx::SignalGraph& graph, const char* id, const char* key, double fallback)
{
    if (const auto* node = findNode (graph, id))
        if (const auto it = node->params.find (key); it != node->params.end())
            return it->second;

    return fallback;
}

bool nodeEnabled (const guitarfx::SignalGraph& graph, const char* id)
{
    const auto* node = findNode (graph, id);
    return node != nullptr && node->enabled;
}
} // namespace

ControlsContent::ControlsContent (NanoContext& contextIn) : context (contextIn)
{
    setComponentID ("controls");
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::nonHover);
    addAndMakeVisible (viewport);

    auto& commands = context.commands;
    const auto chain = [this] () -> const guitarfx::GlobalSignalChainConfig& { return context.state().globalChain; };

    auto& input = addSection ("Input");
    addKnob (input, param ("inputGain", "Level", "dB", -12.0, 12.0, 0.0), [chain] { return chain().inputGain; },
             [&commands] (double v) { commands.SetGlobalChainParam ("input.gain", v); });

    if (context.state().environment.standalone)
    {
        addSwitch (input, "Mono input", [chain] { return chain().monoMode; },
                   [this, chain] (bool mono) { context.commands.SetInputMode (mono, chain().inputChannel); });
        addSwitch (input, "Right channel", [chain] { return chain().inputChannel == 1; },
                   [this, chain] (bool right) { context.commands.SetInputMode (chain().monoMode, right ? 1 : 0); });
    }

    auto& gate = addSection ("Noise gate");
    addSwitch (gate, "On", [chain] { return nodeEnabled (chain().preChainGraph, "global_gate"); },
               [&commands] (bool on) { commands.SetGlobalChainToggle ("gate.enabled", on); });
    addKnob (gate, param ("threshold", "Threshold", "dB", -90.0, 0.0, -60.0, 1.0),
             [chain] { return nodeParam (chain().preChainGraph, "global_gate", "threshold", -60.0); },
             [&commands] (double v) { commands.SetGlobalChainParam ("gate.threshold", v); });

    auto& transpose = addSection ("Transpose");
    addKnob (transpose, param ("semitones", "Semitones", " st", -12.0, 12.0, 0.0, 1.0),
             [chain] { return nodeEnabled (chain().preChainGraph, "global_transpose")
                                  ? nodeParam (chain().preChainGraph, "global_transpose", "semitones", 0.0)
                                  : 0.0; },
             [&commands] (double v) {
                 // As the web UI's knob: the transpose runs only while it transposes.
                 const auto semitones = std::round (v);
                 commands.SetGlobalChainParam ("transpose.semitones", semitones);
                 commands.SetGlobalChainToggle ("transpose.enabled", semitones != 0.0);
             });
    knobs.back().control->setFormatter ([] (double v) {
        const auto semitones = juce::roundToInt (v);
        return (semitones > 0 ? "+" : "") + juce::String (semitones) + " st";
    });

    auto& eq = addSection ("Global EQ");
    addSwitch (eq, "On", [chain] { return nodeEnabled (chain().postChainGraph, "global_eq"); },
               [&commands] (bool on) { commands.SetGlobalChainToggle ("eq.enabled", on); });

    for (const auto& [key, name] : std::initializer_list<std::pair<const char*, const char*>> {
             { "lowGain", "Low" }, { "lowMidGain", "Low mid" }, { "highMidGain", "High mid" }, { "highGain", "High" } })
    {
        const std::string path = std::string ("eq.") + key;
        addKnob (eq, param (key, name, "dB", -12.0, 12.0, 0.0),
                 [chain, key = key] { return nodeParam (chain().postChainGraph, "global_eq", key, 0.0); },
                 [&commands, path] (double v) { commands.SetGlobalChainParam (path, v); });
    }

    auto& doubler = addSection ("Doubler");
    addSwitch (doubler, "On", [chain] { return nodeEnabled (chain().postChainGraph, "global_doubler"); },
               [&commands] (bool on) { commands.SetGlobalChainToggle ("doubler.enabled", on); });
    addKnob (doubler, param ("time", "Delay", "ms", 0.5, 50.0, 6.0),
             [chain] { return nodeParam (chain().postChainGraph, "global_doubler", "time", 6.0); },
             [&commands] (double v) { commands.SetGlobalChainParam ("doubler.delay", v); });
    addKnob (doubler, param ("mix", "Mix", "amount", 0.0, 1.0, 0.5),
             [chain] { return nodeParam (chain().postChainGraph, "global_doubler", "mix", 0.5); },
             [&commands] (double v) { commands.SetGlobalChainParam ("doubler.mix", v); });

    auto& output = addSection ("Output");
    addKnob (output, param ("outputGain", "Level", "dB", -12.0, 12.0, 0.0), [chain] { return chain().outputGain; },
             [&commands] (double v) { commands.SetGlobalChainParam ("output.gain", v); });
    addSwitch (output, "Mute", [this] { return context.state().outputMuted; },
               [&commands] (bool muted) { commands.SetOutputMuted (muted); });
    addSwitch (output, "Limiter", [this] { return context.state().appSettings.value (kLimiterSetting, false); },
               [&commands] (bool on) { commands.SetSetting (kLimiterSetting, on); });

    chainSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::GlobalChain, [this] { update(); });
    sessionSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Session, [this] { update(); });
    update();
}

ControlsContent::~ControlsContent() = default;

ControlsContent::Section& ControlsContent::addSection (const juce::String& title)
{
    auto section = std::make_unique<Section>();
    section->heading = std::make_unique<juce::Label>();
    section->heading->setText (title.toUpperCase(), juce::dontSendNotification);
    section->heading->setFont (context.font (NanoTheme::textOverline, FontWeight::semibold).withExtraKerningFactor (0.06f));
    section->heading->setColour (juce::Label::textColourId, context.theme.textMuted());
    content.addAndMakeVisible (*section->heading);
    sections.push_back (std::move (section));
    return *sections.back();
}

void ControlsContent::addKnob (Section& section, const EffectParamInfo& info, std::function<double()> read,
                               std::function<void (double)> write)
{
    auto control = std::make_unique<ParamControl> (context, info, ParamControl::Style::Knob);
    control->onChange = std::move (write);
    content.addAndMakeVisible (*control);
    section.knobs.push_back (control.get());
    knobs.push_back ({ std::move (control), std::move (read) });
}

void ControlsContent::addSwitch (Section& section, const juce::String& text, std::function<bool()> read,
                                 std::function<void (bool)> write)
{
    auto button = std::make_unique<juce::ToggleButton> (text);
    button->setComponentID ("controls-" + section.heading->getText().toLowerCase().replaceCharacter (' ', '-') + "-"
                            + text.toLowerCase().replaceCharacter (' ', '-'));
    auto* raw = button.get();
    button->onClick = [raw, write = std::move (write)] { write (raw->getToggleState()); };
    content.addAndMakeVisible (*button);
    section.switches.push_back (button.get());
    switches.push_back ({ std::move (button), std::move (read) });
}

void ControlsContent::update()
{
    for (auto& knob : knobs)
        knob.control->setValue (knob.read());

    for (auto& toggle : switches)
        toggle.button->setToggleState (toggle.read(), juce::dontSendNotification);
}

int ControlsContent::layoutContent (int width, bool apply)
{
    const int knobWidth = context.touch ? 100 : 88;
    const int switchHeight = context.touch ? 44 : 34;

    // Sections go into two columns when there is room, each into the shorter column so far.
    constexpr int columnGap = 16;
    const int columns = width >= 600 ? 2 : 1;
    const int columnWidth = (width - columnGap * (columns - 1)) / columns;
    std::array<int, 2> columnBottom {};

    for (auto& section : sections)
    {
        const int column = columns == 2 && columnBottom[1] < columnBottom[0] ? 1 : 0;
        const int left = column * (columnWidth + columnGap);
        int y = columnBottom[(std::size_t) column];

        if (apply)
            section->heading->setBounds (left + 4, y, columnWidth - 8, 22);

        y += 24;
        int x = 0;

        for (auto* toggle : section->switches)
        {
            if (x > 0 && x + 150 > columnWidth)
            {
                x = 0;
                y += switchHeight;
            }

            if (apply)
                toggle->setBounds (left + x + 4, y, 146, switchHeight);

            x += 150;
        }

        if (! section->switches.empty())
            y += switchHeight + 4;

        x = 0;
        int rowHeight = 0;

        for (auto* knob : section->knobs)
        {
            const int h = knob->preferredHeight (knobWidth);

            if (x > 0 && x + knobWidth > columnWidth)
            {
                x = 0;
                y += rowHeight;
                rowHeight = 0;
            }

            if (apply)
                knob->setBounds (left + x, y, knobWidth, h);

            x += knobWidth;
            rowHeight = juce::jmax (rowHeight, h);
        }

        columnBottom[(std::size_t) column] = y + rowHeight + 10;
    }

    return juce::jmax (columnBottom[0], columnBottom[1]);
}

void ControlsContent::resized()
{
    viewport.setBounds (getLocalBounds());
    const int width = getWidth() - 8;
    content.setSize (width, layoutContent (width, false));
    layoutContent (width, true);
}
} // namespace soundshed::nano
