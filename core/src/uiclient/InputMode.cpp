#include "uiclient/InputMode.h"

namespace guitarfx::uiclient::input_mode
{
Mode FromSettings(bool monoMode, int inputChannel, bool dualMono)
{
    if (monoMode)
    {
        return inputChannel == 1 ? Mode::Mono2 : inputChannel == 2 ? Mode::MonoSum : Mode::Mono1;
    }

    return dualMono ? Mode::DualMono : Mode::Stereo;
}

Settings ToSettings(Mode mode)
{
    switch (mode)
    {
    case Mode::Mono2:
        return {true, 1, false};
    case Mode::MonoSum:
        return {true, 2, false};
    case Mode::Stereo:
        return {false, 0, false};
    case Mode::DualMono:
        return {false, 0, true};
    case Mode::Mono1:
        break;
    }

    return {true, 0, false};
}

std::vector<Mode> Listed(const InputCapabilities& caps)
{
    if (caps.hostControlled)
    {
        return {Mode::Stereo, Mode::DualMono};
    }

    return {Mode::Mono1, Mode::Mono2, Mode::MonoSum, Mode::Stereo, Mode::DualMono};
}

std::optional<std::string> UnavailableReason(Mode mode, const InputCapabilities& caps)
{
    const bool twoInputs = caps.inputChannels >= 2;
    const bool twoOutputs = caps.outputChannels >= 2;

    if (caps.hostControlled)
    {
        if (mode == Mode::DualMono && !(twoInputs && twoOutputs))
        {
            return twoInputs ? "Needs a stereo output" : "Needs a stereo track";
        }

        return std::nullopt;
    }

    if (mode == Mode::Mono1)
    {
        return std::nullopt;
    }

    if (mode == Mode::DualMono && !(twoInputs && twoOutputs))
    {
        return twoInputs ? "Needs two outputs" : "Needs two inputs and two outputs";
    }

    return twoInputs ? std::nullopt : std::optional<std::string>("Needs a second input");
}

Mode Displayed(Mode current, const InputCapabilities& caps)
{
    return caps.hostControlled && current != Mode::DualMono ? Mode::Stereo : current;
}

Presentation Present(Mode mode, const InputCapabilities& caps)
{
    switch (mode)
    {
    case Mode::Mono1:
        return {"mono1", "Mono In 1", "Input 1 on both sides"};
    case Mode::Mono2:
        return {"mono2", "Mono In 2", "Input 2 on both sides"};
    case Mode::MonoSum:
        return {"monoSum", "Mono 1+2", "Inputs 1 and 2 summed"};
    case Mode::DualMono:
        return {"dualMono", "Dual mono", "A separate rig per input"};
    case Mode::Stereo:
        break;
    }

    if (caps.hostControlled)
    {
        return caps.inputChannels >= 2 ? Presentation{"stereo", "Track", "Stereo, as the track is"}
                                       : Presentation{"mono", "Track", "Mono, as the track is"};
    }

    return {"stereo", "Stereo", "Input 1 left, input 2 right"};
}

namespace
{
/// What `mode` runs as when nothing gets in its way.
std::string IntendedEffective(Mode mode, const InputCapabilities& caps)
{
    if (mode == Mode::DualMono)
    {
        return "dualMono";
    }

    if (mode == Mode::Stereo)
    {
        return caps.hostControlled && caps.inputChannels < 2 ? "mono" : "stereo";
    }

    return "mono";
}

int SharedPluginCount(const InputCapabilities& caps)
{
    return caps.effectiveMode == "dualMono" ? caps.dualMonoShared : 0;
}
} // namespace

bool FallsBack(Mode mode, const InputCapabilities& caps)
{
    return caps.known && IntendedEffective(mode, caps) != caps.effectiveMode;
}

std::string EffectiveName(const InputCapabilities& caps)
{
    return caps.effectiveMode == "stereo" ? "Stereo" : caps.effectiveMode == "dualMono" ? "Dual mono" : "Mono";
}

std::string StatusNote(Mode current, const InputCapabilities& caps)
{
    if (!caps.known)
    {
        return {};
    }

    if (const int shared = SharedPluginCount(caps); shared > 0)
    {
        return std::to_string(shared) + " hosted plugin" + (shared == 1 ? "" : "s") + " hear" +
               (shared == 1 ? "s" : "") + " both inputs: a plugin cannot be doubled up for dual mono.";
    }

    if (caps.hostControlled)
    {
        return "Your DAW track decides mono or stereo. Dual mono runs a stereo track as two separate rigs.";
    }

    if (caps.inputChannels < 2)
    {
        return "The audio device has one input, so the chain runs in mono.";
    }

    if (current == Mode::DualMono && caps.outputChannels < 2)
    {
        return "Dual mono needs two outputs, so both inputs are summed to mono.";
    }

    return {};
}

std::string Tooltip(Mode current, const InputCapabilities& caps)
{
    const auto shown = Displayed(current, caps);
    const auto presentation = Present(shown, caps);
    std::string text = "Input: " + presentation.name + ". " + presentation.description + ".";

    if (FallsBack(shown, caps))
    {
        auto running = EffectiveName(caps);

        for (auto& c : running)
        {
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }

        text += " Running in " + running + ".";
    }

    if (const auto note = StatusNote(current, caps); !note.empty())
    {
        text += " " + note;
    }

    return text;
}
} // namespace guitarfx::uiclient::input_mode
