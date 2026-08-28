/**
 * PluginControllerRiffs.cpp - Capturing, editing and playing back riff takes.
 *
 * Takes are stored on disk under the riff library folder with an index
 * alongside them; the index is the source of truth for what exists and is
 * rewritten whole on each change, under mRiffLibraryMutex.
 */

#include "PluginController.h"

#include "controller/MetronomeService.h"

#include "controller/DemoPreviewService.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "controller/internal/RiffSupport.h"
#include "util/AudioDecoder.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

void PluginController::HandleGetRiffLibraryRequest()
{
    SendRiffLibraryStateToUI();
}

void PluginController::HandleSetRiffLibraryPathRequest(const nlohmann::json& payload)
{
    const std::string requestedPath = payload.value("path", "");
    if (requestedPath.empty())
    {
        ReportErrorToUI("Riff Library", "Path is required");
        return;
    }

    try
    {
        const std::filesystem::path libraryPath = util::PathFromUtf8(requestedPath);
        std::filesystem::create_directories(libraryPath);
            mAppSettings[kRiffLibraryPathSettingKey] = util::PathToUtf8(libraryPath);
        SaveAppSettings();

        {
            std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
            mRiffLibraryIndex = LoadRiffLibraryIndex();
            mRiffLibraryIndex["path"] = util::PathToUtf8(libraryPath);
            if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
                mRiffLibraryIndex["riffs"] = nlohmann::json::array();
            SaveRiffLibraryIndex(mRiffLibraryIndex);
        }

        SendRiffLibraryStateToUI();
    }
    catch (const std::exception& ex)
    {
        ReportErrorToUI("Riff Library", ex.what());
    }
}


void PluginController::HandleStartRiffCaptureRequest(const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    if (mRiffCapture.active)
    {
        ReportErrorToUI("Riff Capture", "Capture is already running");
        return;
    }

    const double sampleRate = mHost.GetSampleRate();
    if (sampleRate <= 0.0)
    {
        ReportErrorToUI("Riff Capture", "Audio device sample rate is unavailable");
        return;
    }

    RiffCaptureConfig config;
    config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    config.bars = std::max(1, payload.value("bars", 1));
    config.countInBars = std::max(0, payload.value("countInBars", 1));
    config.metronomeClickEnabled = payload.value("metronomeClickEnabled", true);
    config.patternType = payload.value("patternType", std::string("click"));
    config.patternId = payload.value("patternId", std::string{});
    config.beatPattern = payload.value("beatPattern", mMetronome->BeatPattern()); // use UI value or fall back to global
    config.presetId = mActivePresetId;
    config.presetName = mActivePreset ? mActivePreset->name : std::string{};

    const double beatScale = 4.0 / static_cast<double>(config.timeSigDen);
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, config.tempoBpm)) * beatScale;
    const std::size_t captureSamples = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.bars))));
    const std::size_t countInSamples = config.countInBars > 0
        ? std::max<std::size_t>(0,
            static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.countInBars))))
        : 0;

    mRiffCapture = RiffCaptureRuntime{};
    mRiffCapture.active = true;
    mRiffCapture.complete = false;
    mRiffCapture.takeId = BuildRiffTakeId();
    mRiffCapture.config = config;
    mRiffCapture.left.assign(captureSamples, 0.0f);
    mRiffCapture.right.assign(captureSamples, 0.0f);
    mRiffCapture.writeIndex = 0;
    mRiffCapture.targetSamples = captureSamples + countInSamples;
    mRiffCapture.countInSamples = countInSamples;
    mRiffCapture.sampleRate = sampleRate;
    mRiffCapture.bitsPerSample = 16;
    constexpr std::size_t kLivePeakBuckets = 256;
    mRiffCapture.livePeaks.assign(kLivePeakBuckets, 0.0f);
    mRiffCapture.livePeakBucketSize = std::max<std::size_t>(1, captureSamples / kLivePeakBuckets);
    mRiffCapture.lastProgressSample = 0;
    mRiffCapture.startedAt = std::chrono::steady_clock::now();
    ActivateRiffGuidance(config, false);

    nlohmann::json msg;
    msg["type"] = "riffCaptureStarted";
    msg["takeId"] = mRiffCapture.takeId;
    msg["bars"] = config.bars;
    msg["tempoBpm"] = config.tempoBpm;
    msg["timeSigNum"] = config.timeSigNum;
    msg["timeSigDen"] = config.timeSigDen;
    msg["countInBars"] = config.countInBars;
    msg["metronomeClickEnabled"] = config.metronomeClickEnabled;
    msg["estimatedSeconds"] = static_cast<double>(captureSamples) / sampleRate;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleArmRiffCaptureRequest(const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    if (mRiffCapture.active || mRiffCapture.armed)
    {
        ReportErrorToUI("Riff Capture", "Capture or arm is already active");
        return;
    }

    const double sampleRate = mHost.GetSampleRate();
    if (sampleRate <= 0.0)
    {
        ReportErrorToUI("Riff Capture", "Audio device sample rate is unavailable");
        return;
    }

    RiffCaptureConfig config;
    config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    // ARM mode: no fixed bar count; allocate 16 bars max
    config.bars = 16;
    config.bars = std::max(1, std::min(64, payload.value("bars", 16)));
    config.countInBars = std::max(0, payload.value("countInBars", 1));
    config.metronomeClickEnabled = payload.value("metronomeClickEnabled", true);
    config.patternType = payload.value("patternType", std::string("click"));
    config.patternId = payload.value("patternId", std::string{});
    config.beatPattern = payload.value("beatPattern", mMetronome->BeatPattern());
    config.presetId = mActivePresetId;
    config.presetName = mActivePreset ? mActivePreset->name : std::string{};

    const double beatScale = 4.0 / static_cast<double>(config.timeSigDen);
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, config.tempoBpm)) * beatScale;
    const std::size_t maxCaptureSamples = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.bars))));
    const std::size_t countInSamples = config.countInBars > 0
        ? std::max<std::size_t>(0,
            static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.countInBars))))
        : 0;

    constexpr std::size_t kLivePeakBuckets = 256;

    mRiffCapture = RiffCaptureRuntime{};
    mRiffCapture.armed = true;
    mRiffCapture.active = false;
    mRiffCapture.complete = false;
    mRiffCapture.takeId = BuildRiffTakeId();
    mRiffCapture.config = config;
    mRiffCapture.left.assign(maxCaptureSamples, 0.0f);
    mRiffCapture.right.assign(maxCaptureSamples, 0.0f);
    mRiffCapture.writeIndex = 0;
    mRiffCapture.targetSamples = maxCaptureSamples + countInSamples;
    mRiffCapture.countInSamples = countInSamples;
    mRiffCapture.sampleRate = sampleRate;
    mRiffCapture.bitsPerSample = 16;
    mRiffCapture.livePeaks.assign(kLivePeakBuckets, 0.0f);
    mRiffCapture.livePeakBucketSize = std::max<std::size_t>(1, maxCaptureSamples / kLivePeakBuckets);
    mRiffCapture.lastProgressSample = 0;
    mRiffCapture.armPostCountInSamples = 0;
    mRiffCapture.startedAt = std::chrono::steady_clock::now();
    // Start click playing via guidance (count-in pattern), don't start recording yet
    ActivateRiffGuidance(config, false);

    nlohmann::json msg;
    msg["type"] = "riffCaptureArmed";
    msg["takeId"] = mRiffCapture.takeId;
    msg["tempoBpm"] = config.tempoBpm;
    msg["timeSigNum"] = config.timeSigNum;
    msg["timeSigDen"] = config.timeSigDen;
    msg["countInBars"] = config.countInBars;
    msg["bars"] = config.bars;
    msg["metronomeClickEnabled"] = config.metronomeClickEnabled;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleStopRiffCaptureRequest(const nlohmann::json& payload)
{
    const bool canceled = payload.value("canceled", false);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    FinalizeRiffCaptureLocked(canceled);
}

void PluginController::HandleImportRiffWavRequest(const nlohmann::json& payload)
{
    const std::string base64 = payload.value("data", std::string{});
    if (base64.empty())
    {
        ReportErrorToUI("Riff Library", "Dropped audio data is missing");
        return;
    }

    const auto bytes = util::DecodeBase64(base64);
    if (bytes.empty())
    {
            ReportErrorToUI("Riff Library", "Failed to decode dropped audio data");
        return;
    }

        const auto decodedOpt = util::DecodeAudioBytes(bytes);
    if (!decodedOpt)
    {
            ReportErrorToUI("Riff Library", "Unsupported audio format (expected WAV, AIFF, or MP3)");
        return;
    }

    const auto& decoded = *decodedOpt;
    if (decoded.channelSamples.empty() || decoded.channelSamples.front().empty())
    {
            ReportErrorToUI("Riff Library", "Dropped audio file has no audio samples");
        return;
    }

    const std::size_t frameCount = decoded.channelSamples.front().size();
    if (frameCount == 0)
    {
            ReportErrorToUI("Riff Library", "Dropped audio file has no audio frames");
        return;
    }

    RiffCaptureRuntime imported;
    imported.active = false;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    imported.config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    imported.config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    imported.config.countInBars = 0;
    imported.config.patternType = payload.value("patternType", std::string("click"));
    imported.config.patternId = payload.value("patternId", std::string{});
    imported.config.presetId = mActivePresetId;
    imported.config.presetName = mActivePreset ? mActivePreset->name : std::string{};
    imported.sampleRate = decoded.sampleRate > 0.0 ? decoded.sampleRate : mHost.GetSampleRate();
    imported.bitsPerSample = decoded.bitsPerSample > 0 ? decoded.bitsPerSample : 16;
    imported.config.bars = payload.contains("bars")
        ? std::max(1, payload.value("bars", 1))
        : ComputeBarsFromFrames(frameCount,
                                imported.sampleRate,
                                imported.config.tempoBpm,
                                imported.config.timeSigNum,
                                imported.config.timeSigDen);
    imported.left.resize(frameCount, 0.0f);
    imported.right.resize(frameCount, 0.0f);

    const std::size_t rightChannelIndex = decoded.channelSamples.size() > 1 ? 1u : 0u;
    for (std::size_t i = 0; i < frameCount; ++i)
    {
        imported.left[i] = static_cast<float>(std::clamp(decoded.channelSamples[0][i], -1.0, 1.0));
        imported.right[i] = static_cast<float>(std::clamp(decoded.channelSamples[rightChannelIndex][i], -1.0, 1.0));
    }

    imported.writeIndex = frameCount;
    imported.targetSamples = frameCount;
    imported.countInSamples = 0;
    imported.startedAt = std::chrono::steady_clock::now();
    imported.endedAt = imported.startedAt;

    RiffCaptureRuntime captureSnapshot;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = std::move(imported);
        captureSnapshot = mRiffCapture;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot.takeId;
    msg["capturedSamples"] = captureSnapshot.left.size();
    msg["sampleRate"] = captureSnapshot.sampleRate;
    msg["hasAudio"] = !captureSnapshot.left.empty() && !captureSnapshot.right.empty();
    msg["waveformPeaks"] = BuildWaveformPeaks(captureSnapshot.left, captureSnapshot.right, 256);
    msg["bars"] = captureSnapshot.config.bars;
    msg["tempoBpm"] = captureSnapshot.config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot.config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot.config.timeSigDen;
    msg["metronomeClickEnabled"] = captureSnapshot.config.metronomeClickEnabled;
    msg["patternType"] = captureSnapshot.config.patternType;
    if (!captureSnapshot.config.patternId.empty())
        msg["patternId"] = captureSnapshot.config.patternId;
    msg["source"] = "import";
    SendMessageToUI(msg.dump());
}

void PluginController::HandleTrimCapturedRiffRequest(const nlohmann::json& payload)
{
    RiffCaptureRuntime captureSnapshot;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (!mRiffCapture.complete || mRiffCapture.left.empty() || mRiffCapture.right.empty())
        {
            ReportErrorToUI("Riff Library", "No captured take available to trim");
            return;
        }

        const std::size_t totalSamples = mRiffCapture.left.size();
        const double startRatioRaw = payload.value("startRatio", 0.0);
        const double endRatioRaw = payload.value("endRatio", 1.0);
        const double startRatio = std::clamp(startRatioRaw, 0.0, 1.0);
        const double endRatio = std::clamp(endRatioRaw, 0.0, 1.0);

        std::size_t startSample = static_cast<std::size_t>(std::floor(startRatio * static_cast<double>(totalSamples)));
        std::size_t endSample = static_cast<std::size_t>(std::ceil(endRatio * static_cast<double>(totalSamples)));
        startSample = std::min(startSample, totalSamples > 0 ? totalSamples - 1 : 0);
        endSample = std::max(endSample, startSample + 1);
        endSample = std::min(endSample, totalSamples);

        if (startSample >= endSample)
        {
            ReportErrorToUI("Riff Library", "Invalid trim markers");
            return;
        }

        std::vector<float> trimmedLeft(mRiffCapture.left.begin() + startSample,
                           mRiffCapture.left.begin() + endSample);
        std::vector<float> trimmedRight(mRiffCapture.right.begin() + startSample,
                        mRiffCapture.right.begin() + endSample);

        mRiffCapture.left = std::move(trimmedLeft);
        mRiffCapture.right = std::move(trimmedRight);
        mRiffCapture.writeIndex = mRiffCapture.left.size();
        mRiffCapture.targetSamples = mRiffCapture.left.size();
        mRiffCapture.countInSamples = 0;
        mRiffCapture.endedAt = std::chrono::steady_clock::now();

        captureSnapshot = mRiffCapture;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot.takeId;
    msg["capturedSamples"] = captureSnapshot.left.size();
    msg["sampleRate"] = captureSnapshot.sampleRate;
    msg["hasAudio"] = !captureSnapshot.left.empty() && !captureSnapshot.right.empty();
    msg["waveformPeaks"] = BuildWaveformPeaks(captureSnapshot.left, captureSnapshot.right, 256);
    msg["bars"] = captureSnapshot.config.bars;
    msg["metronomeClickEnabled"] = captureSnapshot.config.metronomeClickEnabled;
    msg["tempoBpm"] = captureSnapshot.config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot.config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot.config.timeSigDen;
    msg["patternType"] = captureSnapshot.config.patternType;
    if (!captureSnapshot.config.patternId.empty())
        msg["patternId"] = captureSnapshot.config.patternId;
    msg["source"] = "trim";
    SendMessageToUI(msg.dump());
}

void PluginController::HandleLoadRiffTakeForEditRequest(const nlohmann::json& payload)
{
    const std::string takeId = payload.value("takeId", std::string{});
    if (takeId.empty())
    {
        ReportErrorToUI("Riff Library", "Missing takeId for edit");
        return;
    }

    const auto take = FindRiffTakeById(takeId);
    if (!take)
    {
        ReportErrorToUI("Riff Library", "Take not found");
        return;
    }

    const std::string filePath = take->value("filePath", std::string{});
    if (filePath.empty() || !std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Riff Library", "Take WAV file is missing");
        return;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input)
    {
        ReportErrorToUI("Riff Library", "Unable to open take WAV file");
        return;
    }

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        ReportErrorToUI("Riff Library", "Take WAV file is empty");
        return;
    }

    const auto decodedOpt = util::DecodePcmWav(bytes);
    if (!decodedOpt)
    {
        ReportErrorToUI("Riff Library", "Unable to decode take WAV file");
        return;
    }

    const auto& decoded = *decodedOpt;
    if (decoded.channelSamples.empty() || decoded.channelSamples.front().empty())
    {
        ReportErrorToUI("Riff Library", "Take WAV has no audio samples");
        return;
    }

    const std::size_t frameCount = decoded.channelSamples.front().size();
    const std::size_t rightChannelIndex = decoded.channelSamples.size() > 1 ? 1u : 0u;

    RiffCaptureRuntime imported;
    imported.active = false;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm = ClampValue(take->value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    imported.config.timeSigNum = std::max(1, take->value("timeSigNum", 4));
    imported.config.timeSigDen = std::max(1, take->value("timeSigDen", 4));
    imported.config.bars = std::max(1, take->value("bars", 1));
    imported.config.countInBars = 0;
    imported.config.metronomeClickEnabled = take->value("metronomeClickEnabled", true);
    imported.config.patternType = take->value("patternType", std::string("click"));
    imported.config.patternId = take->value("patternId", std::string{});
    imported.config.presetId = take->value("presetId", std::string{});
    imported.config.presetName = take->value("presetName", std::string{});
    imported.sampleRate = decoded.sampleRate > 0.0 ? decoded.sampleRate : mHost.GetSampleRate();
    imported.bitsPerSample = decoded.bitsPerSample > 0 ? decoded.bitsPerSample : 16;
    imported.left.resize(frameCount, 0.0f);
    imported.right.resize(frameCount, 0.0f);
    for (std::size_t i = 0; i < frameCount; ++i)
    {
        imported.left[i] = static_cast<float>(std::clamp(decoded.channelSamples[0][i], -1.0, 1.0));
        imported.right[i] = static_cast<float>(std::clamp(decoded.channelSamples[rightChannelIndex][i], -1.0, 1.0));
    }
    imported.writeIndex = frameCount;
    imported.targetSamples = frameCount;
    imported.countInSamples = 0;
    imported.startedAt = std::chrono::steady_clock::now();
    imported.endedAt = imported.startedAt;

    RiffCaptureRuntime captureSnapshot;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = std::move(imported);
        captureSnapshot = mRiffCapture;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot.takeId;
    msg["capturedSamples"] = captureSnapshot.left.size();
    msg["sampleRate"] = captureSnapshot.sampleRate;
    msg["hasAudio"] = !captureSnapshot.left.empty() && !captureSnapshot.right.empty();
    msg["waveformPeaks"] = BuildWaveformPeaks(captureSnapshot.left, captureSnapshot.right, 256);
    msg["bars"] = captureSnapshot.config.bars;
    msg["tempoBpm"] = captureSnapshot.config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot.config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot.config.timeSigDen;
    msg["metronomeClickEnabled"] = captureSnapshot.config.metronomeClickEnabled;
    msg["patternType"] = captureSnapshot.config.patternType;
    if (!captureSnapshot.config.patternId.empty())
        msg["patternId"] = captureSnapshot.config.patternId;
    msg["source"] = "editLoad";
    msg["originalTakeId"] = takeId;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSaveRiffTakeRequest(const nlohmann::json& payload)
{
    RiffCaptureRuntime capture;
    nlohmann::json updatedLibrary = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (!mRiffCapture.complete || mRiffCapture.left.empty() || mRiffCapture.right.empty())
        {
            ReportErrorToUI("Riff Library", "No completed take to save");
            return;
        }
        capture = mRiffCapture;
    }

    if (payload.contains("tempoBpm"))
        capture.config.tempoBpm = ClampValue(payload.value("tempoBpm", capture.config.tempoBpm), kMetronomeMinBpm, kMetronomeMaxBpm);
    if (payload.contains("timeSigNum"))
        capture.config.timeSigNum = std::max(1, payload.value("timeSigNum", capture.config.timeSigNum));
    if (payload.contains("timeSigDen"))
        capture.config.timeSigDen = std::max(1, payload.value("timeSigDen", capture.config.timeSigDen));
    if (payload.contains("bars"))
        capture.config.bars = std::max(1, payload.value("bars", capture.config.bars));
    if (payload.contains("metronomeClickEnabled"))
        capture.config.metronomeClickEnabled = payload.value("metronomeClickEnabled", capture.config.metronomeClickEnabled);
    if (payload.contains("patternType") && payload["patternType"].is_string())
        capture.config.patternType = payload.value("patternType", capture.config.patternType);
    if (payload.contains("patternId") && payload["patternId"].is_string())
        capture.config.patternId = payload.value("patternId", std::string{});
    if (payload.contains("presetId") && payload["presetId"].is_string())
        capture.config.presetId = payload.value("presetId", capture.config.presetId);

    const std::string riffId = payload.value("riffId", std::string{}).empty() ? BuildRiffId() : payload.value("riffId", std::string{});
    const std::string baseTitle = payload.value("title", std::string("New Riff"));
    const std::string safeTitle = util::SanitizeFilename(baseTitle.empty() ? "New Riff" : baseTitle);
    const auto libraryPath = ResolveRiffLibraryPath();
    const auto takesDir = libraryPath / "takes" / riffId;
    const auto fileName = safeTitle + "_" + capture.takeId + ".wav";
    const auto wavPath = takesDir / fileName;

    if (!util::WriteStereo16BitWav(wavPath, capture.left, capture.right, static_cast<int>(std::llround(capture.sampleRate))))
    {
        ReportErrorToUI("Riff Library", "Failed to write WAV file");
        return;
    }

    nlohmann::json takeJson;
    takeJson["id"] = capture.takeId;
    takeJson["filePath"] = util::PathToUtf8(wavPath);
    takeJson["durationSec"] = capture.sampleRate > 0.0
        ? static_cast<double>(capture.left.size()) / capture.sampleRate
        : 0.0;
    takeJson["bars"] = capture.config.bars;
    takeJson["tempoBpm"] = capture.config.tempoBpm;
    takeJson["timeSigNum"] = capture.config.timeSigNum;
    takeJson["timeSigDen"] = capture.config.timeSigDen;
    takeJson["metronomeClickEnabled"] = capture.config.metronomeClickEnabled;
    takeJson["patternType"] = capture.config.patternType;
    if (!capture.config.patternId.empty())
        takeJson["patternId"] = capture.config.patternId;
    if (!capture.config.beatPattern.empty())
        takeJson["beatPattern"] = capture.config.beatPattern;
    if (!capture.config.presetId.empty())
        takeJson["presetId"] = capture.config.presetId;
    if (!capture.config.presetName.empty())
        takeJson["presetName"] = capture.config.presetName;
    takeJson["sampleRate"] = capture.sampleRate;
    takeJson["bitsPerSample"] = capture.bitsPerSample;
    takeJson["createdAt"] = BuildTimestampUtcIso();

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.is_object())
            mRiffLibraryIndex = nlohmann::json::object();

        mRiffLibraryIndex["path"] = util::PathToUtf8(libraryPath);
        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
            mRiffLibraryIndex["riffs"] = nlohmann::json::array();

        auto& riffs = mRiffLibraryIndex["riffs"];
        nlohmann::json* riffEntry = nullptr;
        for (auto& riff : riffs)
        {
            if (riff.is_object() && riff.value("id", std::string{}) == riffId)
            {
                riffEntry = &riff;
                break;
            }
        }

        if (!riffEntry)
        {
            nlohmann::json newRiff;
            newRiff["id"] = riffId;
            newRiff["title"] = baseTitle;
            newRiff["categories"] = nlohmann::json::array();
            newRiff["tags"] = nlohmann::json::array();
            newRiff["notes"] = "";
            newRiff["favorite"] = payload.value("favorite", false);
            newRiff["used"] = false;
            newRiff["createdAt"] = BuildTimestampUtcIso();
            newRiff["updatedAt"] = newRiff["createdAt"];
            newRiff["takes"] = nlohmann::json::array();
            riffs.push_back(std::move(newRiff));
            riffEntry = &riffs.back();
        }

        if (riffEntry)
        {
            (*riffEntry)["title"] = baseTitle;
            (*riffEntry)["updatedAt"] = BuildTimestampUtcIso();
            if (payload.contains("categories") && payload["categories"].is_array())
                (*riffEntry)["categories"] = payload["categories"];
            if (payload.contains("tags") && payload["tags"].is_array())
                (*riffEntry)["tags"] = payload["tags"];
            if (payload.contains("notes") && payload["notes"].is_string())
                (*riffEntry)["notes"] = payload["notes"];
            if (payload.contains("favorite") && payload["favorite"].is_boolean())
                (*riffEntry)["favorite"] = payload["favorite"];
            if (!(*riffEntry).contains("takes") || !(*riffEntry)["takes"].is_array())
                (*riffEntry)["takes"] = nlohmann::json::array();
            (*riffEntry)["takes"].push_back(takeJson);
            (*riffEntry)["preferredTakeId"] = capture.takeId;
        }

        SaveRiffLibraryIndex(mRiffLibraryIndex);
        updatedLibrary = LoadRiffLibraryIndex();
        mRiffLibraryIndex = updatedLibrary;
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = RiffCaptureRuntime{};
    }

    nlohmann::json msg;
    msg["type"] = "riffSaved";
    msg["riffId"] = riffId;
    msg["takeId"] = capture.takeId;
    msg["path"] = util::PathToUtf8(wavPath);
    msg["library"] = updatedLibrary;
    SendMessageToUI(msg.dump());
    SendRiffLibraryStateToUI();
}

void PluginController::HandleDeleteRiffRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    std::vector<std::filesystem::path> takeFiles;
    std::filesystem::path takesDirToRemove;

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
            return;

        auto& riffs = mRiffLibraryIndex["riffs"];
        for (const auto& riff : riffs)
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
                continue;

            const auto takes = riff.value("takes", nlohmann::json::array());
            if (takes.is_array())
            {
                for (const auto& take : takes)
                {
                    if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                        continue;
                    const auto runtimePath = util::PathFromUtf8(take["filePath"].get<std::string>());
                    if (!runtimePath.empty())
                        takeFiles.push_back(runtimePath);
                }
            }
            break;
        }

        riffs.erase(std::remove_if(riffs.begin(), riffs.end(),
            [&](const nlohmann::json& riff) { return riff.value("id", std::string{}) == riffId; }), riffs.end());
        SaveRiffLibraryIndex(mRiffLibraryIndex);
        takesDirToRemove = ResolveRiffLibraryPath() / "takes" / riffId;
    }

    for (const auto& path : takeFiles)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    if (!takesDirToRemove.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(takesDirToRemove, ec);
    }

    SendRiffLibraryStateToUI();
}

void PluginController::HandleSetRiffFavoriteRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    const bool favorite = payload.value("favorite", false);
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
            return;

        for (auto& riff : mRiffLibraryIndex["riffs"])
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
                continue;
            riff["favorite"] = favorite;
            riff["updatedAt"] = BuildTimestampUtcIso();
            break;
        }

        SaveRiffLibraryIndex(mRiffLibraryIndex);
    }
    SendRiffLibraryStateToUI();
}

void PluginController::HandleMarkRiffUsedRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    const bool used = payload.value("used", false);
    const std::string songTitle = payload.value("songTitle", std::string{});
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
            return;

        for (auto& riff : mRiffLibraryIndex["riffs"])
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
                continue;
            riff["used"] = used;
            if (used)
            {
                riff["usedSongTitle"] = songTitle;
                riff["usedAt"] = BuildTimestampUtcIso();
            }
            else
            {
                riff.erase("usedSongTitle");
                riff.erase("usedAt");
            }
            riff["updatedAt"] = BuildTimestampUtcIso();
            break;
        }

        SaveRiffLibraryIndex(mRiffLibraryIndex);
    }
    SendRiffLibraryStateToUI();
}

void PluginController::HandlePreviewRiffTakeRequest(const nlohmann::json& payload)
{
    const std::string takeId = payload.value("takeId", "");
    const bool enableGuidance = payload.value("enableGuidance", true);
    if (takeId.empty())
    {
        ReportErrorToUI("Riff preview", "Missing takeId");
        return;
    }

    const auto take = FindRiffTakeById(takeId);
    if (!take)
    {
        ReportErrorToUI("Riff preview", "Take not found");
        return;
    }

    const std::string filePath = take->value("filePath", std::string{});
    if (filePath.empty() || !std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Riff preview", "Take WAV file is missing");
        return;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input)
    {
        ReportErrorToUI("Riff preview", "Unable to open take WAV file");
        return;
    }

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        ReportErrorToUI("Riff preview", "Take WAV file is empty");
        return;
    }

    nlohmann::json preview;
    preview["audio"] = {
        {"id", takeId},
        {"title", take->value("title", std::string("Riff Take"))},
        {"data", util::EncodeBase64(bytes)},
        {"contentType", "audio/wav"}
    };

    RiffCaptureConfig guideConfig;
    guideConfig.tempoBpm = ClampValue(take->value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    guideConfig.timeSigNum = std::max(1, take->value("timeSigNum", 4));
    guideConfig.timeSigDen = std::max(1, take->value("timeSigDen", 4));
    guideConfig.metronomeClickEnabled = take->value("metronomeClickEnabled", true);
    guideConfig.patternType = take->value("patternType", std::string("click"));
    guideConfig.patternId = take->value("patternId", std::string{});
    guideConfig.beatPattern = take->value("beatPattern", mMetronome->BeatPattern());

    if (mDemoPreview)
    {
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            if (enableGuidance)
                ActivateRiffGuidance(guideConfig, true);
            else
                DeactivateRiffGuidance(true);
        }
        mDemoPreview->StartPreview(preview);
    }
}

void PluginController::HandlePreviewCapturedRiffRequest(const nlohmann::json& payload)
{
    RiffCaptureRuntime capture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (!mRiffCapture.complete || mRiffCapture.left.empty() || mRiffCapture.right.empty())
        {
            ReportErrorToUI("Riff preview", "No captured take available yet");
            return;
        }
        capture = mRiffCapture;
    }

    std::vector<float> previewLeft = capture.left;
    std::vector<float> previewRight = capture.right;
    if (!previewLeft.empty() && !previewRight.empty())
    {
        const std::size_t totalSamples = previewLeft.size();
        const double startRatioRaw = payload.value("startRatio", 0.0);
        const double endRatioRaw = payload.value("endRatio", 1.0);
        const double startRatio = std::clamp(startRatioRaw, 0.0, 1.0);
        const double endRatio = std::clamp(endRatioRaw, 0.0, 1.0);

        std::size_t startSample = static_cast<std::size_t>(std::floor(startRatio * static_cast<double>(totalSamples)));
        std::size_t endSample = static_cast<std::size_t>(std::ceil(endRatio * static_cast<double>(totalSamples)));
        startSample = std::min(startSample, totalSamples > 0 ? totalSamples - 1 : 0);
        endSample = std::max(endSample, startSample + 1);
        endSample = std::min(endSample, totalSamples);

        previewLeft = std::vector<float>(previewLeft.begin() + startSample,
                         previewLeft.begin() + endSample);
        previewRight = std::vector<float>(previewRight.begin() + startSample,
                          previewRight.begin() + endSample);
    }

    const auto wavBytes = util::EncodeStereo16BitWav(
        previewLeft,
        previewRight,
        static_cast<int>(std::llround(capture.sampleRate)));
    if (wavBytes.empty())
    {
        ReportErrorToUI("Riff preview", "Unable to encode captured take");
        return;
    }

    nlohmann::json preview;
    preview["audio"] = {
        {"id", capture.takeId.empty() ? std::string("captured-take") : capture.takeId},
        {"title", std::string("Captured Riff")},
        {"data", util::EncodeBase64(wavBytes)},
        {"contentType", "audio/wav"}
    };

    if (mDemoPreview)
    {
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            ActivateRiffGuidance(capture.config, true);
        }
        mDemoPreview->StartPreview(preview);
    }
}


std::filesystem::path PluginController::ResolveRiffLibraryPath() const
{
    if (mAppSettings.contains(kRiffLibraryPathSettingKey) && mAppSettings[kRiffLibraryPathSettingKey].is_string())
    {
        const auto configured = util::PathFromUtf8(mAppSettings[kRiffLibraryPathSettingKey].get<std::string>());
        if (!configured.empty())
            return configured;
    }

    return mFileSystem.ResolveSettingsDirectory() / kRiffLibraryDefaultFolder;
}

nlohmann::json PluginController::LoadRiffLibraryIndex() const
{
    const auto path = ResolveRiffLibraryPath();

    // The riff audio files stay on disk under the (user-configurable) riff
    // library folder; only the index moves into the store.
    std::error_code dirEc;
    std::filesystem::create_directories(path, dirEc);

    nlohmann::json index = Store().Get(storage::ItemType::kDocument, kRiffLibraryDocumentId)
                             .value_or(nlohmann::json::object());
    if (!index.is_object())
        index = nlohmann::json::object();

    index["path"] = util::PathToUtf8(path);
    if (!index.contains("riffs") || !index["riffs"].is_array())
        index["riffs"] = nlohmann::json::array();

    for (auto& riff : index["riffs"])
    {
        if (!riff.is_object() || !riff.contains("takes") || !riff["takes"].is_array())
            continue;

        for (auto& take : riff["takes"])
        {
            if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                continue;

            const auto storedPath = util::PathFromUtf8(take["filePath"].get<std::string>());
            if (storedPath.empty())
                continue;

            const auto resolvedPath = ResolveRiffTakePathForRuntime(storedPath, path);
            const bool resolvedExists = !resolvedPath.empty() && resolvedPath != storedPath && std::filesystem::exists(resolvedPath);
            const bool storedExists = std::filesystem::exists(storedPath);

            if (resolvedExists)
                take["filePath"] = util::PathToUtf8(resolvedPath);
            else if (!storedExists && !resolvedPath.empty())
                take["filePath"] = util::PathToUtf8(resolvedPath);
        }
    }

    return index;
}

bool PluginController::SaveRiffLibraryIndex(const nlohmann::json& payload) const
{
    const auto libraryPath = ResolveRiffLibraryPath();
    nlohmann::json normalizedPayload = payload;

    normalizedPayload["path"] = util::PathToUtf8(libraryPath);
    if (!normalizedPayload.contains("riffs") || !normalizedPayload["riffs"].is_array())
        normalizedPayload["riffs"] = nlohmann::json::array();

    for (auto& riff : normalizedPayload["riffs"])
    {
        if (!riff.is_object() || !riff.contains("takes") || !riff["takes"].is_array())
            continue;

        for (auto& take : riff["takes"])
        {
            if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                continue;

            const auto runtimePath = util::PathFromUtf8(take["filePath"].get<std::string>());
            const auto storedPath = BuildRiffTakePathForStorage(runtimePath, libraryPath);
            take["filePath"] = util::PathToUtf8(storedPath);
        }
    }

    if (!Store().Put(storage::ItemType::kDocument, kRiffLibraryDocumentId, normalizedPayload))
        return false;

    TouchSharedSyncState({"riffLibrary"});
    return true;
}

std::string PluginController::BuildRiffTakeId() const
{
    return "take-" + GenerateGuidV4String();
}

std::string PluginController::BuildRiffId() const
{
    return "riff-" + GenerateGuidV4String();
}

std::string PluginController::BuildTimestampUtcIso() const
{
    return BuildUtcIsoTimestamp();
}

std::optional<nlohmann::json> PluginController::FindRiffTakeById(const std::string& takeId) const
{
    std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
    if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        return std::nullopt;

    for (const auto& riff : mRiffLibraryIndex["riffs"])
    {
        if (!riff.is_object())
            continue;
        const std::string riffTitle = riff.value("title", std::string("Riff"));
        const auto takes = riff.value("takes", nlohmann::json::array());
        if (!takes.is_array())
            continue;
        for (const auto& take : takes)
        {
            if (!take.is_object() || take.value("id", std::string{}) != takeId)
                continue;
            nlohmann::json result = take;
            result["title"] = riffTitle;
            return result;
        }
    }

    return std::nullopt;
}

void PluginController::FinalizeRiffCaptureLocked(bool canceled)
{
    if (!mRiffCapture.active && !mRiffCapture.armed && !mRiffCapture.complete)
        return;

    if (canceled)
    {
        const std::string takeId = mRiffCapture.takeId;
        mRiffCapture = RiffCaptureRuntime{};
        DeactivateRiffGuidance(false);
        nlohmann::json msg;
        msg["type"] = "riffCaptureCanceled";
        msg["takeId"] = takeId;
        SendMessageToUI(msg.dump());
        return;
    }

    // If still armed (never triggered), cancel instead of producing empty audio
    if (mRiffCapture.armed && !mRiffCapture.active)
    {
        const std::string takeId = mRiffCapture.takeId;
        mRiffCapture = RiffCaptureRuntime{};
        DeactivateRiffGuidance(false);
        nlohmann::json msg;
        msg["type"] = "riffCaptureCanceled";
        msg["takeId"] = takeId;
        SendMessageToUI(msg.dump());
        return;
    }

    const std::size_t written = std::min(mRiffCapture.writeIndex, mRiffCapture.targetSamples);
    const std::size_t captured = written > mRiffCapture.countInSamples ? (written - mRiffCapture.countInSamples) : 0;
    if (captured < mRiffCapture.left.size())
        mRiffCapture.left.resize(captured);
    if (captured < mRiffCapture.right.size())
        mRiffCapture.right.resize(captured);

    mRiffCapture.active = false;
    mRiffCapture.armed = false;
    mRiffCapture.complete = captured > 0;
    mRiffCapture.endedAt = std::chrono::steady_clock::now();
    DeactivateRiffGuidance(false);

    // Compute bars from actual captured length
    const double samplesPerBeat = mRiffCapture.sampleRate
        * (60.0 / std::max(1.0, mRiffCapture.config.tempoBpm))
        * (4.0 / static_cast<double>(std::max(1, mRiffCapture.config.timeSigDen)));
    const double samplesPerBar = samplesPerBeat * static_cast<double>(std::max(1, mRiffCapture.config.timeSigNum));
    const int computedBars = std::max(1, static_cast<int>(
        std::round(static_cast<double>(captured) / std::max(1.0, samplesPerBar))));

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = mRiffCapture.takeId;
    msg["bars"] = computedBars;
    msg["capturedSamples"] = captured;
    msg["sampleRate"] = mRiffCapture.sampleRate;
    msg["hasAudio"] = captured > 0;
    msg["metronomeClickEnabled"] = mRiffCapture.config.metronomeClickEnabled;
    msg["waveformPeaks"] = BuildWaveformPeaks(mRiffCapture.left, mRiffCapture.right, 256);
    SendMessageToUI(msg.dump());
}

// ── Messaging helpers ──────────────────────────────────────────────

void PluginController::SendRiffLibraryStateToUI()
{
    nlohmann::json msg;
    msg["type"] = "riffLibraryState";
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
        msg["library"] = mRiffLibraryIndex;
    }

    nlohmann::json capture;
    capture["active"] = mRiffCapture.active;
    capture["complete"] = mRiffCapture.complete;
    capture["takeId"] = mRiffCapture.takeId;
    capture["bars"] = mRiffCapture.config.bars;
    capture["tempoBpm"] = mRiffCapture.config.tempoBpm;
    capture["timeSigNum"] = mRiffCapture.config.timeSigNum;
    capture["timeSigDen"] = mRiffCapture.config.timeSigDen;
    capture["capturedSamples"] = mRiffCapture.left.size();
    capture["sampleRate"] = mRiffCapture.sampleRate;
    capture["hasAudio"] = !mRiffCapture.left.empty() && !mRiffCapture.right.empty();
    capture["waveformPeaks"] = BuildWaveformPeaks(mRiffCapture.left, mRiffCapture.right, 256);
    msg["capture"] = capture;

    SendMessageToUI(msg.dump());
}

} // namespace guitarfx
