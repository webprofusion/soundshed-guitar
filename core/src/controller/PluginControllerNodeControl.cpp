/**
 * PluginControllerNodeControl.cpp - Driving the nodes of the running graph.
 *
 * Enable, parameter and resource changes for a node that already exists, as
 * opposed to PluginControllerSignalPath.cpp, which is where the graph's shape
 * is edited. The model/IR entry points are the same operation reached from a
 * file browser: register the file in the resource library, then point every
 * node of the matching type at it.
 *
 * Anything that can change the chain's length calls UpdateHostLatency() on the
 * way out, so a DAW's delay compensation follows.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/NamResourceMetadata.h"
#include "dsp/EffectGuids.h"
#include "presets/PresetStorage.h"
#include "util/PathEncoding.h"

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::HandleBrowseModelRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::NAMModel, "Select NAM Model", [this](const BrowseFileResult& result) {
        if (result.success)
        {
            nlohmann::json payload;
            payload["path"] = util::PathToUtf8(result.path);
            HandleLoadModelRequest(payload);
        }
    });
}

void PluginController::HandleBrowseIRRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::IRFile, "Select IR File", [this](const BrowseFileResult& result) {
        if (result.success)
        {
            nlohmann::json payload;
            payload["path"] = util::PathToUtf8(result.path);
            HandleLoadIRRequest(payload);
        }
    });
}

void PluginController::HandleLoadModelRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");

    if (path.empty())
    {
        path = payload.value("filePath", "");
    }

    if (path.empty())
    {
        return;
    }

    std::filesystem::path filePath = util::PathFromUtf8(path);

    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Model file not found", path);
        return;
    }

    std::string resourceError;
    const auto savedResource = SaveLocalLibraryResource(
        nlohmann::json{{"resourceType", "nam"},
                       {"filePath", util::PathToUtf8(filePath)},
                       {"name", util::PathToUtf8(filePath.stem())},
                       {"category", "Local"},
                       {"metadata", nlohmann::json::object({{"provider", kLocalResourceProvider}})}},
        resourceError, true);

    if (!savedResource)
    {
        ReportErrorToUI("Model load failed",
                        resourceError.empty() ? "Could not register model in the resource library" : resourceError);
        return;
    }

    const bool updatedNamResource =
        UpdateResourceForNodeType(EffectGuids::kAmpNamOptimized, savedResource->type, filePath) ||
        UpdateResourceForNodeType(EffectGuids::kAmpNamBlend, savedResource->type, filePath) ||
        UpdateResourceForNodeType(EffectGuids::kFxNam, savedResource->type, filePath) ||
        UpdateResourceForNodeType(EffectGuids::kAmpNam, savedResource->type, filePath);

    if (updatedNamResource)
    {
        mAppSettings["lastModelPath"] = util::PathToUtf8(filePath.parent_path());
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "modelLoaded";
        message["path"] = util::PathToUtf8(filePath);
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleLoadIRRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");

    if (path.empty())
    {
        path = payload.value("filePath", "");
    }

    if (path.empty())
    {
        return;
    }

    std::filesystem::path filePath = util::PathFromUtf8(path);

    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("IR file not found", path);
        return;
    }

    if (UpdateResourceForNodeType(EffectGuids::kCabIr, "ir", filePath))
    {
        mAppSettings["lastIRPath"] = util::PathToUtf8(filePath.parent_path());
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "irLoaded";
        message["path"] = util::PathToUtf8(filePath);
        SendMessageToUI(message.dump());
    }
}

void PluginController::ResetNamNodeLevelState(const std::string& nodeId)
{
    if (nodeId.empty() || !mActivePreset)
    {
        return;
    }

    auto* node = mActivePreset->graph.FindNode(nodeId);

    if (!node || !IsNamEffectType(node->type))
    {
        return;
    }

    ClearNamCalibrationParams(*node);

    if (!node->params.contains("useCalibration"))
    {
        node->params["useCalibration"] = 1.0;
    }

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mPendingStateBroadcast = true;

    if (!mActivePresetId.empty())
    {
        const auto useCalibrationIt = node->params.find("useCalibration");
        const double useCalibrationValue =
            (useCalibrationIt != node->params.end() && useCalibrationIt->second <= 0.5) ? 0.0 : 1.0;
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "useCalibration", useCalibrationValue);

        // Re-inject current interface calibration level for this NAM node.
        // Keep this value resident even if no model is currently resolved so
        // calibration takes effect immediately when a model loads.
        InjectNamInterfaceCalibration(mActivePresetId, nodeId);
    }
}

void PluginController::InjectNamInterfaceCalibration(const std::string& presetId, const std::string& nodeId)
{
    // "No level" travels as a flag of its own. It used to be a NaN level, which the Release builds'
    // fast floating-point semantics cannot carry: clang assumed the NaN away, and the node applied
    // whatever was left of it as a real calibration level.
    if (mNamInterfaceCalibrationLevelDbu)
    {
        mPresetMixer.SetNodeParam(presetId, nodeId, "calibrationInputLevel", *mNamInterfaceCalibrationLevelDbu);
    }

    mPresetMixer.SetNodeParam(presetId, nodeId, "calibrationInputLevelEnabled",
                              mNamInterfaceCalibrationLevelDbu ? 1.0 : 0.0);
}

void PluginController::InjectNamInterfaceCalibrationIntoSlot(const std::string& presetId, const Preset& preset)
{
    // Injected even into a node whose model is not resolved yet, so the level is already in
    // place when one loads.
    for (const auto& node : preset.graph.nodes)
    {
        if (IsNamCalibratableEffectType(node.type))
        {
            InjectNamInterfaceCalibration(presetId, node.id);
        }
    }
}

void PluginController::ClearNamCalibrationParams(GraphNode& node) const
{
    node.params.erase("calibrationInputLevel");
    node.params.erase("calibrationInputLevelEnabled");
    node.params.erase("calibrationOutputLevel");
}

void PluginController::ApplyDeferredNodeRebuilds()
{
    // SetParam can run on the audio thread (MIDI and DAW automation apply there), so a parameter
    // whose change needs a rebuild, like the IR cab's Normalize and Low Latency, is only recorded
    // there. The rebuild is built here, off the DSP lock, and swapped in under it: the audio thread
    // outputs silence for any block that finds the lock held, so nothing holds it for the build.
    // The request count spares idle ticks with nothing requested from taking the lock at all.
    const auto requests = DeferredRebuild::RequestCount();

    if (requests == mDeferredRebuildRequestsSeen)
    {
        return;
    }

    mDeferredRebuildRequestsSeen = requests;
    std::vector<MultiPresetMixer::GraphRebuildWork> rebuilds;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.TakeDeferredRebuilds(rebuilds);
    }

    if (rebuilds.empty())
    {
        return;
    }

    for (auto& rebuild : rebuilds)
    {
        rebuild.work->Build();
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.CommitDeferredRebuilds(rebuilds);
    }

    // Frees what the rebuilds replaced, after the lock. Low Latency changes the chain's length.
    rebuilds.clear();
    UpdateHostLatency();
}
} // namespace guitarfx
