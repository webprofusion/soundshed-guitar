/**
 * PluginControllerDemo.cpp - Demo preview playback and offline render, plus the
 * practice-tool transport handlers.
 *
 * Rendering a demo borrows the live mixer and must give it back unchanged; the
 * handler here owns that contract, while the mechanics live in
 * controller/internal/OfflineRenderSupport.
 */

#include "PluginController.h"

#include "controller/DemoPreviewService.h"
#include "controller/SignalTestService.h"
#include "controller/PracticeToolService.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::HandlePreviewDemoRequest(const nlohmann::json& payload)
{
    if (mDemoPreview)
    {
        mDemoPreview->StartPreview(payload);
    }
}

void PluginController::HandleRenderDemoAudioRequest(const nlohmann::json& payload)
{
    auto sendRenderFailure = [this](const std::string& message) {
        SendMessageToUI(nlohmann::json{{"type", "demoAudioRenderFailed"}, {"message", message}}.dump());
    };

    const double hostSampleRate = mHost.GetSampleRate();
    std::string sampleRateError;
    const double renderSampleRate = ResolveDemoRenderSampleRate(payload, hostSampleRate, sampleRateError);

    if (renderSampleRate <= 0.0)
    {
        sendRenderFailure(sampleRateError.empty() ? "Render sample rate is invalid" : sampleRateError);
        return;
    }

    const std::string suggestedName = BuildDemoRenderSuggestedFilename(
        payload.value("suggestedName", std::string("demo-audio.wav")), renderSampleRate);

    const nlohmann::json payloadCopy = payload;
    mHost.SaveFileAsync(
        BrowseFileType::AudioFile, "Render Demo Audio", suggestedName,
        [this, payloadCopy, sendRenderFailure, renderSampleRate](const BrowseFileResult& result) {
            if (!result.success)
            {
                sendRenderFailure("Save cancelled");
                return;
            }

            if (mSignalTest->IsActive())
            {
                sendRenderFailure("Signal path test is currently running");
                return;
            }

            const double restoreSampleRate = mHost.GetSampleRate();

            if (restoreSampleRate <= 0.0)
            {
                sendRenderFailure("Audio device sample rate is unavailable");
                return;
            }

            const int hostBlockSize = std::max(1, mHost.GetBlockSize());

            OfflineRenderBuffer source;
            std::string error;

            if (payloadCopy.contains("takeId") && payloadCopy["takeId"].is_string())
            {
                const std::string takeId = payloadCopy.value("takeId", std::string{});
                const auto take = FindRiffTakeById(takeId);

                if (!take)
                {
                    sendRenderFailure("Take not found");
                    return;
                }

                const std::string filePath = take->value("filePath", std::string{});

                if (filePath.empty() || !std::filesystem::exists(filePath))
                {
                    sendRenderFailure("Take WAV file is missing");
                    return;
                }

                std::ifstream input(filePath, std::ios::binary);

                if (!input)
                {
                    sendRenderFailure("Unable to open take WAV file");
                    return;
                }

                std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                                std::istreambuf_iterator<char>());

                if (bytes.empty())
                {
                    sendRenderFailure("Take WAV file is empty");
                    return;
                }

                auto prepared = PrepareOfflineRenderBuffer(
                    bytes, renderSampleRate, takeId,
                    payloadCopy.value("title", take->value("title", std::string("Riff Take"))), error);

                if (!prepared)
                {
                    sendRenderFailure(error.empty() ? "Unable to prepare riff take audio" : error);
                    return;
                }

                source = std::move(*prepared);
            }
            else
            {
                const auto audioIter = payloadCopy.find("audio");

                if (audioIter == payloadCopy.end() || !audioIter->is_object())
                {
                    sendRenderFailure("Audio payload is missing");
                    return;
                }

                const std::string dataEncoded = audioIter->value("data", "");

                if (dataEncoded.empty())
                {
                    sendRenderFailure("Audio payload did not include data");
                    return;
                }

                const auto decodedBytes = util::DecodeBase64(dataEncoded);

                if (decodedBytes.empty())
                {
                    sendRenderFailure("Unable to decode audio data");
                    return;
                }

                auto prepared = PrepareOfflineRenderBuffer(
                    decodedBytes, renderSampleRate, audioIter->value("id", std::string{}),
                    payloadCopy.value("title", audioIter->value("title", std::string("Demo Audio"))), error);

                if (!prepared)
                {
                    sendRenderFailure(error.empty() ? "Unable to prepare demo audio" : error);
                    return;
                }

                source = std::move(*prepared);
            }

            if (mDemoPreview)
            {
                mDemoPreview->StopPreview();
            }

            {
                std::lock_guard<std::mutex> lock(mDSPMutex);
                DeactivateRiffGuidance(true);
            }

            std::vector<float> renderedLeft;
            std::vector<float> renderedRight;

            if (!RenderBufferThroughMixer(mPresetMixer, mDSPMutex, source, hostBlockSize, restoreSampleRate,
                                          hostBlockSize, GetEffectiveTempoBpm(), renderedLeft, renderedRight))
            {
                sendRenderFailure("Failed to render demo audio");
                return;
            }

            if (!util::WriteStereo16BitWav(result.path, renderedLeft, renderedRight,
                                           static_cast<int>(std::llround(renderSampleRate))))
            {
                sendRenderFailure("Failed to write WAV file");
                return;
            }

            SendMessageToUI(nlohmann::json{{"type", "demoAudioRenderSaved"},
                                           {"path", result.path.generic_string()},
                                           {"sampleRate", renderSampleRate}}
                                .dump());
            AppendSessionLog("Demo audio rendered (" +
                             std::to_string(static_cast<int>(std::llround(renderSampleRate))) +
                             " Hz): " + result.path.generic_string());
        });
}

void PluginController::HandleStopDemoRequest()
{
    if (mDemoPreview)
    {
        mDemoPreview->StopPreview();
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        DeactivateRiffGuidance(true);
    }
}

// ── Practice Tool (Jam panel backing-track player) ────────────

// Each fader sends its own field name ("ratio", "gain", ...); a generic
// "value" is accepted as a fallback so one slider binding can drive any of
// them. Ignores a present-but-non-numeric field rather than throwing, since
// MessageDispatcher calls handlers outside its JSON try/catch.
static double PracticeToolNumberField(const nlohmann::json& payload, const char* key, double fallback)
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

void PluginController::HandleBrowsePracticeToolFileRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::AudioFile, "Select Backing Track", [this](const BrowseFileResult& result) {
        if (!result.success)
        {
            return;
        }

        nlohmann::json payload;
        payload["path"] = util::PathToUtf8(result.path);
        HandleLoadPracticeToolFileRequest(payload);
    });
}

void PluginController::HandleLoadPracticeToolFileRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const std::string path = payload.value("path", "");

    if (path.empty())
    {
        ReportErrorToUI("Unable to load audio file", "No file path provided");
        return;
    }

    mPracticeTool->LoadFile(path);
}

// WebView2 is standard Chromium — a dropped File's real filesystem path is
// never available to JS (that's an Electron-only extension), so a file
// dropped on the waveform is sent here as base64 bytes instead of a path
// (see the "Dropped-file paths" note in .github/copilot-instructions.md).
void PluginController::HandleLoadPracticeToolFileDataRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const std::string fileName = payload.value("fileName", "");
    const std::string dataEncoded = payload.value("data", "");

    if (dataEncoded.empty())
    {
        ReportErrorToUI("Unable to load audio file", "Dropped file payload did not include data");
        return;
    }

    const auto decodedBytes = util::DecodeBase64(dataEncoded);

    if (decodedBytes.empty())
    {
        ReportErrorToUI("Unable to load audio file", "Unable to decode dropped file data");
        return;
    }

    mPracticeTool->LoadFileFromBytes(decodedBytes, fileName.empty() ? "Dropped file" : fileName);
}

void PluginController::HandleSetPracticeToolTransportRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const std::string action = payload.value("action", "");

    if (action == "play")
    {
        mPracticeTool->Play();
    }
    else if (action == "pause")
    {
        mPracticeTool->Pause();
    }
    else if (action == "stop")
    {
        mPracticeTool->Stop();
    }
}

void PluginController::HandleSeekPracticeToolFileRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const double seconds = payload.value("seconds", 0.0);
    mPracticeTool->SeekSeconds(seconds);
}

void PluginController::HandleSetPracticeToolSpeedRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const double ratio = PracticeToolNumberField(payload, "ratio", 1.0);
    mPracticeTool->SetSpeed(ratio);
}

void PluginController::HandleSetPracticeToolPitchRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const double semitones = PracticeToolNumberField(payload, "semitones", 0.0);
    mPracticeTool->SetPitchSemitones(semitones);
}

void PluginController::HandleSetPracticeToolGainRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const double gain = PracticeToolNumberField(payload, "gain", 1.0);
    mPracticeTool->SetGain(gain);
}

void PluginController::HandleSetPracticeToolBalanceRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const double balance = PracticeToolNumberField(payload, "balance", 0.0);
    mPracticeTool->SetBalance(balance);
}

void PluginController::HandleSetPracticeToolLoopRegionRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    if (payload.is_null() || !payload.contains("startSec") || !payload.contains("endSec"))
    {
        mPracticeTool->ClearLoopRegion();
        return;
    }

    const double startSec = payload.value("startSec", 0.0);
    const double endSec = payload.value("endSec", 0.0);
    mPracticeTool->SetLoopRegion(startSec, endSec);
}

void PluginController::HandleSetPracticeToolLoopingRequest(const nlohmann::json& payload)
{
    if (!mPracticeTool)
    {
        return;
    }

    const bool enabled = payload.value("enabled", false);
    mPracticeTool->SetLoopingEnabled(enabled);
}
} // namespace guitarfx
