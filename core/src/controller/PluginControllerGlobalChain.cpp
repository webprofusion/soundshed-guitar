/**
 * PluginControllerGlobalChain.cpp - The global signal chain and the input stage.
 *
 * The global chain (gate, transpose, EQ, doubler, input/output trim, limiter)
 * sits outside any preset and is owned by the mixer. Two ways in: a whole
 * config at once, built off the DSP lock and installed under it, or one
 * paramPath at a time. "setParameter" is a third spelling of the second — a
 * flat name mapped to a path — kept because it is the documented UI/scripting
 * entry point, and routed through the same handler so there is no second copy
 * of the values to drift.
 */

#include "PluginController.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace guitarfx
{
void PluginController::HandleGetGlobalChainRequest()
{
    SendGlobalChainStateToUI();
}

void PluginController::HandleSetGlobalChainRequest(const nlohmann::json& payload)
{
    // Full global chain config replacement
    if (payload.contains("config"))
    {
        auto config = payload["config"].get<GlobalSignalChainConfig>();

        // Build off the lock, install under it — see PrepareGlobalChainSwap().
        mPresetMixer.PrepareGlobalChainSwap(config);

        {
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
        }
        PersistGlobalFxSettingsToAppSettings();
    }

    SendGlobalChainStateToUI();
}

void PluginController::HandleSetParameterRequest(const nlohmann::json& payload)
{
    // Named alias for the global-chain paths, kept because "setParameter" is the
    // documented UI/scripting entry point. It carries a flat name and a number, so
    // translate to a path and hand off — HandleSetGlobalChainParamRequest owns the
    // routing and the persistence, and there is no second copy of the values to
    // drift out of sync.
    //
    // Both spellings are accepted: the protocol documents camelCase, older UI code
    // used snake_case, and a name that silently matched neither is what made the
    // legacy path dead in the first place.
    struct GlobalParamAlias
    {
        const char* path;
        bool isBoolean;
        bool isInteger;
    };

    static const std::unordered_map<std::string, GlobalParamAlias> kAliases = {
        {"inputTrim", {"input.gain", false, false}},         {"input_trim", {"input.gain", false, false}},
        {"outputTrim", {"output.gain", false, false}},       {"output_trim", {"output.gain", false, false}},
        {"gateEnabled", {"gate.enabled", true, false}},      {"gate_enabled", {"gate.enabled", true, false}},
        {"gateThreshold", {"gate.threshold", false, false}}, {"gate_threshold", {"gate.threshold", false, false}},
        {"transpose", {"transpose.semitones", false, true}}, {"doublerEnabled", {"doubler.enabled", true, false}},
        {"doublerDelay", {"doubler.delay", false, false}},   {"eqEnabled", {"eq.enabled", true, false}},
        {"eqLowGain", {"eq.lowGain", false, false}},         {"eqLowFreq", {"eq.lowFreq", false, false}},
        {"eqLowMidGain", {"eq.lowMidGain", false, false}},   {"eqLowMidFreq", {"eq.lowMidFreq", false, false}},
        {"eqLowMidQ", {"eq.lowMidQ", false, false}},         {"eqHighMidGain", {"eq.highMidGain", false, false}},
        {"eqHighMidFreq", {"eq.highMidFreq", false, false}}, {"eqHighMidQ", {"eq.highMidQ", false, false}},
        {"eqHighGain", {"eq.highGain", false, false}},       {"eqHighFreq", {"eq.highFreq", false, false}},
    };

    const auto name = payload.value("name", std::string{});
    const auto it = kAliases.find(name);

    if (it == kAliases.end())
    {
        AppendSessionLog("Ignoring setParameter for unknown parameter: " + name);
        return;
    }

    if (!payload.contains("value") || !payload["value"].is_number())
    {
        AppendSessionLog("Ignoring setParameter without a numeric value: " + name);
        return;
    }

    const double raw = payload["value"].get<double>();

    nlohmann::json forwarded;
    forwarded["path"] = it->second.path;

    if (it->second.isBoolean)
    {
        forwarded["value"] = raw > 0.5;
    }
    else if (it->second.isInteger)
    {
        forwarded["value"] = static_cast<int>(std::llround(raw));
    }
    else
    {
        forwarded["value"] = raw;
    }

    HandleSetGlobalChainParamRequest(forwarded);
}

void PluginController::HandleSetGlobalChainParamRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    auto value = payload.value("value", nlohmann::json());
    bool persistGlobalFx = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        // Route paramPath strings to the corresponding mixer methods
        if (path == "gate.enabled")
        {
            mPresetMixer.SetGlobalGateEnabled(value.get<bool>());
            persistGlobalFx = true;
        }
        else if (path == "gate.threshold")
        {
            mPresetMixer.SetGlobalGateThreshold(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "gate.attack")
        {
            mPresetMixer.SetGlobalGateAttack(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "gate.hold")
        {
            mPresetMixer.SetGlobalGateHold(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "gate.release")
        {
            mPresetMixer.SetGlobalGateRelease(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "transpose.enabled")
        {
            mPresetMixer.SetGlobalTransposeEnabled(value.get<bool>());
            UpdateHostLatency();
            persistGlobalFx = true;
        }
        else if (path == "transpose.semitones")
        {
            mPresetMixer.SetGlobalTranspose(std::clamp(value.get<int>(), -12, 12));
            UpdateHostLatency();
            persistGlobalFx = true;
        }
        else if (path == "eq.enabled")
        {
            mPresetMixer.SetGlobalEQEnabled(value.get<bool>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.enabled")
        {
            mPresetMixer.SetGlobalDoublerEnabled(value.get<bool>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.delay")
        {
            mPresetMixer.SetGlobalDoublerDelay(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.mix")
        {
            mPresetMixer.SetGlobalDoublerMix(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.detune")
        {
            mPresetMixer.SetGlobalDoublerDetune(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "input.gain")
        {
            mPresetMixer.SetGlobalInputGain(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "output.gain")
        {
            mPresetMixer.SetGlobalOutputGain(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "limiter.enabled")
        {
            // Not part of the global FX blob (see SerializeGlobalFxSettings): the limiter is
            // an output-stage scalar, and its persistence is the app setting the Settings
            // toggle writes. This path stays for host state and any existing caller.
            mPresetMixer.SetLimiterEnabled(value.get<bool>());
        }
        else if (path == "eq.lowGain")
        {
            mPresetMixer.SetGlobalEQBandGain(0, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(0, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowMidGain")
        {
            mPresetMixer.SetGlobalEQBandGain(1, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowMidFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(1, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowMidQ")
        {
            mPresetMixer.SetGlobalEQBandQ(1, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highMidGain")
        {
            mPresetMixer.SetGlobalEQBandGain(2, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highMidFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(2, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highMidQ")
        {
            mPresetMixer.SetGlobalEQBandQ(2, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highGain")
        {
            mPresetMixer.SetGlobalEQBandGain(3, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(3, value.get<double>());
            persistGlobalFx = true;
        }
    }

    if (persistGlobalFx)
    {
        PersistGlobalFxSettingsToAppSettings();
    }

    // No echo: the UI already owns the values it sent.
    // Full state is pushed via HandleGetGlobalChainRequest / HandleSetGlobalChainRequest.
}

void PluginController::HandleSetInputModeRequest(const nlohmann::json& payload)
{
    // In hosted-plugin mode the DAW owns the input configuration; ignore
    // UI overrides and report the effective (host-controlled) state.
    if (mHost.IsStandalone())
    {
        if (payload.contains("monoMode"))
        {
            mPresetMixer.SetMonoMode(payload["monoMode"].get<bool>());
        }
        else if (payload.contains("mono"))
        {
            mPresetMixer.SetMonoMode(payload["mono"].get<bool>());
        }

        if (payload.contains("inputChannel"))
        {
            mPresetMixer.SetInputChannel(payload["inputChannel"].get<int>());
        }
        else if (payload.contains("channel"))
        {
            mPresetMixer.SetInputChannel(payload["channel"].get<int>());
        }
    }
    else
    {
        AppendSessionLog("Ignoring setInputMode request: input is host-controlled in plugin mode");
    }

    nlohmann::json message;
    message["type"] = "inputModeChanged";
    message["monoMode"] = mPresetMixer.IsMonoMode();
    message["inputChannel"] = mPresetMixer.GetInputChannel();
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetAmpCabStateRequest(const nlohmann::json& payload)
{
    bool ampEnabled = true;
    bool cabEnabled = true;

    if (payload.contains("ampEnabled"))
    {
        ampEnabled = payload.value("ampEnabled", true);
    }

    if (payload.contains("cabEnabled"))
    {
        cabEnabled = payload.value("cabEnabled", true);
    }

    nlohmann::json message;
    message["type"] = "ampCabStateChanged";
    message["ampEnabled"] = ampEnabled;
    message["cabEnabled"] = cabEnabled;
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetAutoLevelRequest(const nlohmann::json& payload)
{
    (void)payload;

    // Mixer-wide peak auto-leveling is retired in favor of model metadata plus
    // explicit input/output controls. Keep the message for compatibility but
    // force the legacy path off.
    mPresetMixer.SetAutoLevelInput(false);
    mPresetMixer.SetAutoLevelOutput(false);

    nlohmann::json message;
    message["type"] = "autoLevelChanged";
    message["autoInput"] = false;
    message["autoOutput"] = false;
    SendMessageToUI(message.dump());
}
} // namespace guitarfx
