/**
 * PluginControllerDemo.cpp - Demo preview playback and offline render. The practice
 * tool answers its own messages (controller/PracticeToolServiceMessages.cpp).
 *
 * Rendering a demo borrows the live mixer and must give it back unchanged; the
 * handler here owns that contract, while the mechanics live in
 * controller/internal/OfflineRenderSupport.
 */

#include "PluginController.h"

#include "controller/DemoPreviewService.h"
#include "controller/SignalTestService.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <cmath>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
nlohmann::json PluginController::LoadDemoClipManifest() const
{
    nlohmann::json clips = nlohmann::json::array();
    const auto root = mHost.GetBundledAssetsPath();

    if (root.empty())
    {
        return clips;
    }

    std::ifstream stream(root / "ui" / "demo" / "clips.json", std::ios::binary);
    const auto manifest = stream ? nlohmann::json::parse(stream, nullptr, false) : nlohmann::json();

    if (!manifest.is_object() || !manifest.contains("clips") || !manifest["clips"].is_array())
    {
        return clips;
    }

    for (const auto& clip : manifest["clips"])
    {
        if (clip.is_object() && !clip.value("id", std::string{}).empty())
        {
            clips.push_back({{"id", clip.value("id", std::string{})}, {"title", clip.value("title", std::string{})}});
        }
    }

    return clips;
}

std::filesystem::path PluginController::FindDemoClipFile(const std::string& clipId, std::string* title) const
{
    const auto root = mHost.GetBundledAssetsPath();

    if (root.empty() || clipId.empty())
    {
        return {};
    }

    const auto folder = root / "ui" / "demo";
    std::ifstream stream(folder / "clips.json", std::ios::binary);
    const auto manifest = stream ? nlohmann::json::parse(stream, nullptr, false) : nlohmann::json();

    if (!manifest.is_object() || !manifest.contains("clips") || !manifest["clips"].is_array())
    {
        return {};
    }

    for (const auto& clip : manifest["clips"])
    {
        if (clip.is_object() && clip.value("id", std::string{}) == clipId)
        {
            if (title != nullptr)
            {
                *title = clip.value("title", clipId);
            }

            // A manifest names files in its own folder, never a path out of it.
            const auto file = std::filesystem::path(util::PathFromUtf8(clip.value("file", std::string{}))).filename();
            return file.empty() ? std::filesystem::path{} : folder / file;
        }
    }

    return {};
}

void PluginController::HandlePreviewDemoRequest(const nlohmann::json& payload)
{
    if (!mDemoPreview)
    {
        return;
    }

    // By id: the engine reads the clip itself, rather than the UI sending its bytes.
    if (payload.contains("clipId"))
    {
        std::string title;
        const auto clipId = payload.value("clipId", std::string{});
        const auto file = FindDemoClipFile(clipId, &title);
        std::ifstream input(file, std::ios::binary);

        if (file.empty() || !input)
        {
            ReportErrorToUI("Demo preview unavailable", "Unknown demo clip: " + clipId);
            return;
        }

        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        mDemoPreview->StartPreviewFromBytes(bytes, clipId, title, payload.value("repeat", false));
        return;
    }

    mDemoPreview->StartPreview(payload);
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
            else if (payloadCopy.contains("clipId"))
            {
                std::string title;
                const auto clipId = payloadCopy.value("clipId", std::string{});
                const auto file = FindDemoClipFile(clipId, &title);
                std::ifstream input(file, std::ios::binary);

                if (file.empty() || !input)
                {
                    sendRenderFailure("Unknown demo clip: " + clipId);
                    return;
                }

                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                                      std::istreambuf_iterator<char>());
                auto prepared = PrepareOfflineRenderBuffer(bytes, renderSampleRate, clipId,
                                                           payloadCopy.value("title", title), error);

                if (!prepared)
                {
                    sendRenderFailure(error.empty() ? "Unable to prepare demo audio" : error);
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
} // namespace guitarfx
