/**
 * PluginControllerDiagnostics.cpp - Telling someone what the app was doing.
 *
 * Three separate feeds, none of them on the audio path:
 *   - the session log, an append-only text file next to the settings;
 *   - the debug snapshot, a one-shot dump of UI state plus backend state,
 *     scrubbed of anything sensitive and written to disk on request;
 *   - the live signal-diagnostics and performance meters, which are only
 *     forwarded from here — TelemetryPublisher owns their rate limits.
 */

#include "PluginController.h"

#include "controller/TelemetryPublisher.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"

#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::AppendSessionLog(const std::string& message) const
{
    if (message.empty())
    {
        return;
    }

    const auto settingsDir = GetEffectiveSettingsDirectory();
    const auto logPath = settingsDir / kSessionLogFileName;
    [[maybe_unused]] const auto ensuredLogDir = mFileSystem.EnsureDirectory(logPath.parent_path());

    std::ofstream output(logPath, std::ios::app);

    if (!output)
    {
        return;
    }

    output << FormatTimestamp() << " " << message << "\n";
}

void PluginController::HandleCaptureDebugSnapshotRequest(const nlohmann::json& payload)
{
    const std::string source = payload.value("source", "manual");

    if (!mUIReady)
    {
        HandleDebugReportUiStateRequest(nlohmann::json{
            {"source", source + ":backend-only"},
        });
        return;
    }

    SendMessageToUI(nlohmann::json{
        {"type", "captureDebugSnapshot"},
        {"source", source},
    }
                        .dump());
}

void PluginController::HandleDebugReportUiStateRequest(const nlohmann::json& payload)
{
    try
    {
        const std::string source = payload.value("source", "ui-auto");
        nlohmann::json snapshot = nlohmann::json::object();
        snapshot["type"] = "debugSnapshot";
        snapshot["capturedAt"] = FormatTimestamp();
        snapshot["source"] = source;
        snapshot["paths"] = {
            {"sessionLog", (mFileSystem.ResolveSettingsDirectory() / kSessionLogFileName).generic_string()},
            {"snapshot", ResolveDebugSnapshotPath(mFileSystem).generic_string()},
        };
        snapshot["session"] = {
            {"activePresetId", mActivePresetId},
            {"activeSceneId", GetResolvedActiveSceneId()},
            {"uiReady", mUIReady},
            {"pendingStateBroadcast", mPendingStateBroadcast},
            {"activePresetIds", mPresetMixer.GetActivePresetIds()},
        };

        if (payload.contains("snapshot"))
        {
            snapshot["ui"] = payload["snapshot"];
            ScrubSensitiveJson(snapshot["ui"]);
        }

        nlohmann::json backendState = nlohmann::json::parse(SerializeState());
        ScrubSensitiveJson(backendState);
        snapshot["backend"] = std::move(backendState);

        if (mActivePreset)
        {
            snapshot["activePresetSummary"] = SummarizeHostedPluginState(*mActivePreset);
        }

        const auto snapshotPath = ResolveDebugSnapshotPath(mFileSystem);
        SaveJsonFile(mFileSystem, snapshotPath, snapshot);

        SendMessageToUI(nlohmann::json{
            {"type", "debugSnapshotWritten"},
            {"path", snapshotPath.generic_string()},
            {"source", source},
        }
                            .dump());
    }
    catch (const std::exception& e)
    {
        AppendSessionLog("Debug snapshot write failed: " + std::string{e.what()});
    }
}

void PluginController::HandleGetSignalDiagnosticsRequest()
{
    // The UI only asks for this on startup or after a reload, when it has no roster to
    // resolve frames against, so always re-send the roster alongside the next frame.
    mTelemetry->MarkRosterDirty();
    mTelemetry->RequestSignalDiagnostics();
}

void PluginController::HandleGetPerformanceStatsRequest()
{
    mTelemetry->RequestPerformanceStats();
}

void PluginController::HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload)
{
    // No preference to store: diagnostics follow UI visibility (see "uiVisibility"). The
    // message survives as the UI's request for a fresh node roster, which implies it is up.
    (void)payload;
    mPresetMixer.SetSignalDiagnosticsEnabled(mTelemetry->IsUiVisible());
    mTelemetry->MarkRosterDirty();
}
} // namespace guitarfx
