/**
 * PracticeToolServiceMessages.cpp - the Practice Tool's UI messages.
 *
 * The Jam panel's backing-track player is this service's alone, so it answers its own
 * messages: RegisterMessageHandlers() puts each one in the dispatcher's registry. They
 * used to be twelve PluginController::Handle* methods that only parsed the payload and
 * called the service.
 */

#include "controller/PracticeToolService.h"

#include "MessageHandlerRegistry.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"

namespace guitarfx
{
namespace
{
// Each fader sends its own field name ("ratio", "gain", ...); a generic
// "value" is accepted as a fallback so one slider binding can drive any of
// them. Ignores a present-but-non-numeric field rather than throwing.
double PracticeToolNumberField(const nlohmann::json& payload, const char* key, double fallback)
{
    for (const char* candidate : {key, "value"})
    {
        const auto it = payload.find(candidate);

        if (it != payload.end() && it->is_number())
        {
            return it->get<double>();
        }
    }

    return fallback;
}
} // namespace

void PracticeToolService::HandleLoadFileRequest(const nlohmann::json& payload)
{
    const std::string path = payload.value("path", "");

    if (path.empty())
    {
        mReportError("Unable to load audio file", "No file path provided");
        return;
    }

    LoadFile(path);
}

void PracticeToolService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("browsePracticeToolFile", [this](const nlohmann::json&) {
        mHost.BrowseFileAsync(BrowseFileType::AudioFile, "Select Backing Track",
                              [this](const BrowseFileResult& result) {
                                  if (!result.success)
                                  {
                                      return;
                                  }

                                  nlohmann::json payload;
                                  payload["path"] = util::PathToUtf8(result.path);
                                  HandleLoadFileRequest(payload);
                              });
    });

    registry.Register("loadPracticeToolFile",
                      [this](const nlohmann::json& payload) { HandleLoadFileRequest(payload); });

    // WebView2 is standard Chromium — a dropped File's real filesystem path is
    // never available to JS (that's an Electron-only extension), so a file
    // dropped on the waveform is sent here as base64 bytes instead of a path
    // (see the "Dropped-file paths" note in .github/copilot-instructions.md).
    registry.Register("loadPracticeToolFileData", [this](const nlohmann::json& payload) {
        const std::string fileName = payload.value("fileName", "");
        const std::string dataEncoded = payload.value("data", "");

        if (dataEncoded.empty())
        {
            mReportError("Unable to load audio file", "Dropped file payload did not include data");
            return;
        }

        const auto decodedBytes = util::DecodeBase64(dataEncoded);

        if (decodedBytes.empty())
        {
            mReportError("Unable to load audio file", "Unable to decode dropped file data");
            return;
        }

        LoadFileFromBytes(decodedBytes, fileName.empty() ? "Dropped file" : fileName);
    });

    registry.Register("setPracticeToolTransport", [this](const nlohmann::json& payload) {
        const std::string action = payload.value("action", "");

        if (action == "play")
        {
            Play();
        }
        else if (action == "pause")
        {
            Pause();
        }
        else if (action == "stop")
        {
            Stop();
        }
    });

    registry.Register("seekPracticeToolFile",
                      [this](const nlohmann::json& payload) { SeekSeconds(payload.value("seconds", 0.0)); });

    registry.Register("setPracticeToolSpeed", [this](const nlohmann::json& payload) {
        SetSpeed(PracticeToolNumberField(payload, "ratio", 1.0));
    });

    registry.Register("setPracticeToolPitch", [this](const nlohmann::json& payload) {
        SetPitchSemitones(PracticeToolNumberField(payload, "semitones", 0.0));
    });

    registry.Register("setPracticeToolGain", [this](const nlohmann::json& payload) {
        SetGain(PracticeToolNumberField(payload, "gain", 1.0));
    });

    registry.Register("setPracticeToolBalance", [this](const nlohmann::json& payload) {
        SetBalance(PracticeToolNumberField(payload, "balance", 0.0));
    });

    registry.Register("setPracticeToolLoopRegion", [this](const nlohmann::json& payload) {
        if (payload.is_null() || !payload.contains("startSec") || !payload.contains("endSec"))
        {
            ClearLoopRegion();
            return;
        }

        SetLoopRegion(payload.value("startSec", 0.0), payload.value("endSec", 0.0), payload.value("restart", false));
    });

    registry.Register("setPracticeToolLooping",
                      [this](const nlohmann::json& payload) { SetLoopingEnabled(payload.value("enabled", false)); });

    // Both fields are optional and applied independently, so the UI can send just
    // the toggle, just the band it is dragging, or the whole block when restoring
    // a saved project — one message shape for all three rather than a per-band
    // message type each. Unknown parameter names are ignored by the effect.
    registry.Register("setPracticeToolEq", [this](const nlohmann::json& payload) {
        if (payload.contains("params") && payload["params"].is_object())
        {
            for (const auto& [key, value] : payload["params"].items())
            {
                if (!key.empty() && value.is_number())
                {
                    SetEqParam(key, value.get<double>());
                }
            }
        }

        if (payload.contains("enabled") && payload["enabled"].is_boolean())
        {
            // After the params, so switching on never briefly runs the previous
            // curve: a payload carrying both is one atomic-looking change.
            SetEqEnabled(payload["enabled"].get<bool>());
        }
    });
}
} // namespace guitarfx
