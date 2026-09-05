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

#include <cmath>
#include <limits>

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

void PluginController::HandleSetNodeEnabledRequest(const nlohmann::json& payload)
{
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    std::string presetId = payload.value("presetId", fallbackId);
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
    UpdateHostLatency();
}

void PluginController::HandleSetNodeParamRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    std::string key = payload.value("key", "");
    double value = payload.value("value", 0.0);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetNodeParam(presetId, nodeId, key, value);
    UpdateHostLatency();
}

void PluginController::HandleLoadNodeResourceRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    ResourceRef ref;

    if (payload.contains("resourceType"))
    {
        ref.resourceType = payload["resourceType"].get<std::string>();
    }

    if (payload.contains("resourceId"))
    {
        ref.resourceId = payload["resourceId"].get<std::string>();
    }

    if (payload.contains("filePath"))
    {
        ref.filePath = payload["filePath"].get<std::string>();
    }

    const bool loaded = mPresetMixer.LoadNodeResource(presetId, nodeId, ref);

    if (!loaded && ReportHostedPluginResourceLoadFailure(nodeId, ref))
    {
        DiscardFailedHostedPluginResourceSelection(nodeId, ref);
    }
    else if (loaded)
    {
        NotifyHostedPluginResourceLoadCompleted(nodeId, ref);
    }

    UpdateHostLatency();
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
        const double clearValue = std::numeric_limits<double>::quiet_NaN();
        const auto useCalibrationIt = node->params.find("useCalibration");
        const double useCalibrationValue =
            (useCalibrationIt != node->params.end() && useCalibrationIt->second <= 0.5) ? 0.0 : 1.0;
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "useCalibration", useCalibrationValue);

        // Re-inject current interface calibration level for this NAM node.
        // Keep this value resident even if no model is currently resolved so
        // calibration takes effect immediately when a model loads.
        const double calLevelToInject =
            std::isfinite(mNamInterfaceCalibrationLevelDbu) ? mNamInterfaceCalibrationLevelDbu : clearValue;
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "calibrationInputLevel", calLevelToInject);
    }
}

void PluginController::ClearNamCalibrationParams(GraphNode& node) const
{
    node.params.erase("calibrationInputLevel");
    node.params.erase("calibrationOutputLevel");
}
} // namespace guitarfx
