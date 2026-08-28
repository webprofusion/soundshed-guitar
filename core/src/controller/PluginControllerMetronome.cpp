/**
 * PluginControllerMetronome.cpp - The controller's side of the click track.
 *
 * The engine is MetronomeService; what is left here is the translation between
 * the controller's vocabulary and the service's. ActivateRiffGuidance is the
 * only place a RiffCaptureConfig is turned into a GuidanceConfig, so capture,
 * preview and playback all hand the click the same shape of request.
 */

#include "PluginController.h"

#include "controller/MetronomeService.h"

namespace guitarfx
{

double PluginController::GetEffectiveTempoBpm() const
{
    return mMetronome->EffectiveTempoBpm();
}

void PluginController::ActivateRiffGuidance(const RiffCaptureConfig& config, bool forPreview)
{
    MetronomeService::GuidanceConfig guidance;
    guidance.tempoBpm = config.tempoBpm;
    guidance.timeSigNum = config.timeSigNum;
    guidance.timeSigDen = config.timeSigDen;
    guidance.beatPattern = config.beatPattern;
    guidance.clickType = config.patternType;

    mMetronome->ActivateGuidance(guidance, config.metronomeClickEnabled, forPreview);
}

void PluginController::DeactivateRiffGuidance(bool previewOnly)
{
    mMetronome->DeactivateGuidance(previewOnly);
}

void PluginController::HandleSetMetronomeRequest(const nlohmann::json& payload)
{
    const auto outcome = mMetronome->ApplyRequest(payload);

    if (outcome.stateChanged)
        mPendingStateBroadcast = true;

    if (outcome.settingsChanged)
        SaveAppSettings();
}

} // namespace guitarfx
