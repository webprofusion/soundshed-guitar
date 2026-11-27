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
#include "controller/LooperRender.h"
#include "controller/LooperService.h"
#include "controller/RiffRecorder.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "controller/internal/RiffSupport.h"
#include "util/AudioDecoder.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// The waveform of the take being recorded so far: the recorder's peak buckets, an equal share each
/// of the longest take allowed.
nlohmann::json LivePeaksToJson(const RiffRecorder& recorder)
{
    nlohmann::json peaks = nlohmann::json::array();

    for (int bucket = 0; bucket < looper::kPeakBuckets; ++bucket)
    {
        peaks.push_back(recorder.LivePeak(bucket));
    }

    return peaks;
}

/// Bars in `samples` of a take at its tempo and meter, at least one.
int BarsInTake(std::size_t samples, double sampleRate, const RiffCaptureConfig& config)
{
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, config.tempoBpm)) *
                                  (4.0 / static_cast<double>(std::max(1, config.timeSigDen)));
    const double samplesPerBar = samplesPerBeat * static_cast<double>(std::max(1, config.timeSigNum));
    return std::max(1, static_cast<int>(std::round(static_cast<double>(samples) / std::max(1.0, samplesPerBar))));
}

/// riffCaptureStopped for a finished take: its length in samples and bars, and its waveform.
nlohmann::json BuildRiffStoppedMessage(const RiffTake& take)
{
    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = take.takeId;
    msg["bars"] = BarsInTake(take.left.size(), take.sampleRate, take.config);
    msg["capturedSamples"] = take.left.size();
    msg["sampleRate"] = take.sampleRate;
    msg["hasAudio"] = !take.left.empty() && !take.right.empty();
    msg["metronomeClickEnabled"] = take.config.metronomeClickEnabled;
    msg["waveformPeaks"] = BuildWaveformPeaks(take.left, take.right, 256);
    return msg;
}
} // namespace

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
            {
                mRiffLibraryIndex["riffs"] = nlohmann::json::array();
            }

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
    // Whatever the last take still owes the UI goes ahead of this one's messages.
    PublishRiffCaptureNotices();

    bool captureBusy = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        captureBusy = mRiffRecorder->Busy();
    }

    if (captureBusy)
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
    const std::size_t captureSamples = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.bars))));

    // Allocate potentially multi-megabyte capture buffers before taking the
    // audio lock. The lock is only needed to hand them to the recorder.
    auto capture = std::make_shared<RiffTake>();
    capture->takeId = BuildRiffTakeId();
    capture->config = config;
    capture->sampleRate = sampleRate;
    capture->startedAt = std::chrono::steady_clock::now();
    std::vector<float> left(captureSamples, 0.0f);
    std::vector<float> right(captureSamples, 0.0f);

    if (!BeginRiffRecording(capture, std::move(left), std::move(right), false,
                            samplesPerBeat * static_cast<double>(config.timeSigNum)))
    {
        ReportErrorToUI("Riff Capture", "Capture or arm is already active");
        return;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStarted";
    msg["takeId"] = capture->takeId;
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
    // Whatever the last take still owes the UI goes ahead of this one's messages.
    PublishRiffCaptureNotices();

    bool captureBusy = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        captureBusy = mRiffRecorder->Busy();
    }

    if (captureBusy)
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
    const std::size_t maxCaptureSamples = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.bars))));
    // The longest take allowed; the take is as long as it runs before a stop.
    auto capture = std::make_shared<RiffTake>();
    capture->takeId = BuildRiffTakeId();
    capture->config = config;
    capture->sampleRate = sampleRate;
    capture->startedAt = std::chrono::steady_clock::now();
    std::vector<float> left(maxCaptureSamples, 0.0f);
    std::vector<float> right(maxCaptureSamples, 0.0f);

    if (!BeginRiffRecording(capture, std::move(left), std::move(right), true,
                            samplesPerBeat * static_cast<double>(config.timeSigNum)))
    {
        ReportErrorToUI("Riff Capture", "Capture or arm is already active");
        return;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureArmed";
    msg["takeId"] = capture->takeId;
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
    FinalizeRiffCapture(canceled);
}

void PluginController::HandleBrowseRiffImportFileRequest(const nlohmann::json& payload)
{
    // The same import as a dropped file's: the file's bytes travel as importRiffWav's data, with
    // the tempo and time signature the request carried.
    mHost.BrowseFileAsync(BrowseFileType::AudioFile, "Import Riff Audio",
                          [this, payload](const BrowseFileResult& result) {
                              if (!result.success)
                              {
                                  return;
                              }

                              const auto bytes = util::ReadFileBytes(result.path);

                              if (bytes.empty())
                              {
                                  ReportErrorToUI("Riff Library", "The audio file could not be read");
                                  return;
                              }

                              auto request = payload;
                              request["data"] = util::EncodeBase64(bytes);
                              request["fileName"] = util::PathToUtf8(result.path.filename());
                              HandleImportRiffWavRequest(request);
                          });
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

    RiffTake imported;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm =
        ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
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
                               : ComputeBarsFromFrames(frameCount, imported.sampleRate, imported.config.tempoBpm,
                                                       imported.config.timeSigNum, imported.config.timeSigDen);
    imported.left.resize(frameCount, 0.0f);
    imported.right.resize(frameCount, 0.0f);

    const std::size_t rightChannelIndex = decoded.channelSamples.size() > 1 ? 1u : 0u;

    for (std::size_t i = 0; i < frameCount; ++i)
    {
        imported.left[i] = static_cast<float>(std::clamp(decoded.channelSamples[0][i], -1.0, 1.0));
        imported.right[i] = static_cast<float>(std::clamp(decoded.channelSamples[rightChannelIndex][i], -1.0, 1.0));
    }

    imported.startedAt = std::chrono::steady_clock::now();
    imported.endedAt = imported.startedAt;

    auto captureSnapshot = std::make_shared<RiffTake>(std::move(imported));
    const auto waveformPeaks = BuildWaveformPeaks(captureSnapshot->left, captureSnapshot->right, 256);
    std::shared_ptr<RiffTake> retiredCapture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        retiredCapture = std::move(mRiffCapture);
        mRiffCapture = captureSnapshot;
    }
    retiredCapture.reset();

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot->takeId;
    msg["capturedSamples"] = captureSnapshot->left.size();
    msg["sampleRate"] = captureSnapshot->sampleRate;
    msg["hasAudio"] = !captureSnapshot->left.empty() && !captureSnapshot->right.empty();
    msg["waveformPeaks"] = waveformPeaks;
    msg["bars"] = captureSnapshot->config.bars;
    msg["tempoBpm"] = captureSnapshot->config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot->config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot->config.timeSigDen;
    msg["metronomeClickEnabled"] = captureSnapshot->config.metronomeClickEnabled;
    msg["patternType"] = captureSnapshot->config.patternType;

    if (!captureSnapshot->config.patternId.empty())
    {
        msg["patternId"] = captureSnapshot->config.patternId;
    }

    msg["source"] = "import";
    SendMessageToUI(msg.dump());
}

void PluginController::HandleTrimCapturedRiffRequest(const nlohmann::json& payload)
{
    std::shared_ptr<const RiffTake> sourceCapture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mRiffCapture && mRiffCapture->complete && !mRiffCapture->left.empty() && !mRiffCapture->right.empty())
        {
            sourceCapture = mRiffCapture;
        }
    }

    if (!sourceCapture)
    {
        ReportErrorToUI("Riff Library", "No captured take available to trim");
        return;
    }

    const std::size_t totalSamples = sourceCapture->left.size();
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

    auto captureSnapshot = std::make_shared<RiffTake>();
    captureSnapshot->complete = true;
    captureSnapshot->takeId = sourceCapture->takeId;
    captureSnapshot->config = sourceCapture->config;
    captureSnapshot->left.assign(sourceCapture->left.begin() + startSample, sourceCapture->left.begin() + endSample);
    captureSnapshot->right.assign(sourceCapture->right.begin() + startSample, sourceCapture->right.begin() + endSample);
    captureSnapshot->sampleRate = sourceCapture->sampleRate;
    captureSnapshot->bitsPerSample = sourceCapture->bitsPerSample;
    captureSnapshot->startedAt = sourceCapture->startedAt;
    captureSnapshot->endedAt = std::chrono::steady_clock::now();
    const auto waveformPeaks = BuildWaveformPeaks(captureSnapshot->left, captureSnapshot->right, 256);

    bool installed = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mRiffCapture == sourceCapture)
        {
            mRiffCapture = captureSnapshot;
            installed = true;
        }
    }

    if (!installed)
    {
        ReportErrorToUI("Riff Library", "Captured take changed while trimming; please try again");
        return;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot->takeId;
    msg["capturedSamples"] = captureSnapshot->left.size();
    msg["sampleRate"] = captureSnapshot->sampleRate;
    msg["hasAudio"] = !captureSnapshot->left.empty() && !captureSnapshot->right.empty();
    msg["waveformPeaks"] = waveformPeaks;
    msg["bars"] = captureSnapshot->config.bars;
    msg["metronomeClickEnabled"] = captureSnapshot->config.metronomeClickEnabled;
    msg["tempoBpm"] = captureSnapshot->config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot->config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot->config.timeSigDen;
    msg["patternType"] = captureSnapshot->config.patternType;

    if (!captureSnapshot->config.patternId.empty())
    {
        msg["patternId"] = captureSnapshot->config.patternId;
    }

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

    // Stored as UTF-8: as a narrow string the path would be read in the ANSI code page.
    const auto filePath = util::PathFromUtf8(take->value("filePath", std::string{}));

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

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

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

    RiffTake imported;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm =
        ClampValue(take->value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
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

    imported.startedAt = std::chrono::steady_clock::now();
    imported.endedAt = imported.startedAt;

    auto captureSnapshot = std::make_shared<RiffTake>(std::move(imported));
    const auto waveformPeaks = BuildWaveformPeaks(captureSnapshot->left, captureSnapshot->right, 256);
    std::shared_ptr<RiffTake> retiredCapture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        retiredCapture = std::move(mRiffCapture);
        mRiffCapture = captureSnapshot;
    }
    retiredCapture.reset();

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot->takeId;
    msg["capturedSamples"] = captureSnapshot->left.size();
    msg["sampleRate"] = captureSnapshot->sampleRate;
    msg["hasAudio"] = !captureSnapshot->left.empty() && !captureSnapshot->right.empty();
    msg["waveformPeaks"] = waveformPeaks;
    msg["bars"] = captureSnapshot->config.bars;
    msg["tempoBpm"] = captureSnapshot->config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot->config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot->config.timeSigDen;
    msg["metronomeClickEnabled"] = captureSnapshot->config.metronomeClickEnabled;
    msg["patternType"] = captureSnapshot->config.patternType;

    if (!captureSnapshot->config.patternId.empty())
    {
        msg["patternId"] = captureSnapshot->config.patternId;
    }

    msg["source"] = "editLoad";
    msg["originalTakeId"] = takeId;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSaveRiffTakeRequest(const nlohmann::json& payload)
{
    std::shared_ptr<const RiffTake> capture;
    nlohmann::json updatedLibrary = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mRiffCapture && mRiffCapture->complete && !mRiffCapture->left.empty() && !mRiffCapture->right.empty())
        {
            capture = mRiffCapture;
        }
    }

    if (!capture)
    {
        ReportErrorToUI("Riff Library", "No completed take to save");
        return;
    }

    RiffCaptureConfig config = capture->config;

    if (payload.contains("tempoBpm"))
    {
        config.tempoBpm = ClampValue(payload.value("tempoBpm", config.tempoBpm), kMetronomeMinBpm, kMetronomeMaxBpm);
    }

    if (payload.contains("timeSigNum"))
    {
        config.timeSigNum = std::max(1, payload.value("timeSigNum", config.timeSigNum));
    }

    if (payload.contains("timeSigDen"))
    {
        config.timeSigDen = std::max(1, payload.value("timeSigDen", config.timeSigDen));
    }

    if (payload.contains("bars"))
    {
        config.bars = std::max(1, payload.value("bars", config.bars));
    }

    if (payload.contains("metronomeClickEnabled"))
    {
        config.metronomeClickEnabled = payload.value("metronomeClickEnabled", config.metronomeClickEnabled);
    }

    if (payload.contains("patternType") && payload["patternType"].is_string())
    {
        config.patternType = payload.value("patternType", config.patternType);
    }

    if (payload.contains("patternId") && payload["patternId"].is_string())
    {
        config.patternId = payload.value("patternId", std::string{});
    }

    if (payload.contains("presetId") && payload["presetId"].is_string())
    {
        config.presetId = payload.value("presetId", config.presetId);
    }

    const std::string riffId =
        payload.value("riffId", std::string{}).empty() ? BuildRiffId() : payload.value("riffId", std::string{});
    const std::string baseTitle = payload.value("title", std::string("New Riff"));
    const std::string safeTitle = util::SanitizeFilename(baseTitle.empty() ? "New Riff" : baseTitle);
    const auto libraryPath = ResolveRiffLibraryPath();
    const auto takesDir = libraryPath / "takes" / riffId;
    const auto fileName = safeTitle + "_" + capture->takeId + ".wav";
    const auto wavPath = takesDir / fileName;

    if (!util::WriteStereo16BitWav(wavPath, capture->left, capture->right,
                                   static_cast<int>(std::llround(capture->sampleRate))))
    {
        ReportErrorToUI("Riff Library", "Failed to write WAV file");
        return;
    }

    nlohmann::json takeJson;
    takeJson["id"] = capture->takeId;
    takeJson["filePath"] = util::PathToUtf8(wavPath);
    takeJson["durationSec"] =
        capture->sampleRate > 0.0 ? static_cast<double>(capture->left.size()) / capture->sampleRate : 0.0;
    takeJson["bars"] = config.bars;
    takeJson["tempoBpm"] = config.tempoBpm;
    takeJson["timeSigNum"] = config.timeSigNum;
    takeJson["timeSigDen"] = config.timeSigDen;
    takeJson["metronomeClickEnabled"] = config.metronomeClickEnabled;
    takeJson["patternType"] = config.patternType;

    if (!config.patternId.empty())
    {
        takeJson["patternId"] = config.patternId;
    }

    if (!config.beatPattern.empty())
    {
        takeJson["beatPattern"] = config.beatPattern;
    }

    if (!config.presetId.empty())
    {
        takeJson["presetId"] = config.presetId;
    }

    if (!config.presetName.empty())
    {
        takeJson["presetName"] = config.presetName;
    }

    takeJson["sampleRate"] = capture->sampleRate;
    takeJson["bitsPerSample"] = capture->bitsPerSample;
    takeJson["createdAt"] = BuildTimestampUtcIso();

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);

        if (!mRiffLibraryIndex.is_object())
        {
            mRiffLibraryIndex = nlohmann::json::object();
        }

        mRiffLibraryIndex["path"] = util::PathToUtf8(libraryPath);

        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        {
            mRiffLibraryIndex["riffs"] = nlohmann::json::array();
        }

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
            {
                (*riffEntry)["categories"] = payload["categories"];
            }

            if (payload.contains("tags") && payload["tags"].is_array())
            {
                (*riffEntry)["tags"] = payload["tags"];
            }

            if (payload.contains("notes") && payload["notes"].is_string())
            {
                (*riffEntry)["notes"] = payload["notes"];
            }

            if (payload.contains("favorite") && payload["favorite"].is_boolean())
            {
                (*riffEntry)["favorite"] = payload["favorite"];
            }

            if (!(*riffEntry).contains("takes") || !(*riffEntry)["takes"].is_array())
            {
                (*riffEntry)["takes"] = nlohmann::json::array();
            }

            (*riffEntry)["takes"].push_back(takeJson);
            (*riffEntry)["preferredTakeId"] = capture->takeId;
        }

        SaveRiffLibraryIndex(mRiffLibraryIndex);
        updatedLibrary = LoadRiffLibraryIndex();
        mRiffLibraryIndex = updatedLibrary;
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mRiffCapture == capture)
        {
            mRiffCapture.reset();
        }
    }

    nlohmann::json msg;
    msg["type"] = "riffSaved";
    msg["riffId"] = riffId;
    msg["takeId"] = capture->takeId;
    msg["path"] = util::PathToUtf8(wavPath);
    msg["library"] = updatedLibrary;
    SendMessageToUI(msg.dump());
    SendRiffLibraryStateToUI();
}

void PluginController::HandleDeleteRiffRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");

    if (riffId.empty())
    {
        return;
    }

    std::vector<std::filesystem::path> takeFiles;
    std::filesystem::path takesDirToRemove;

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);

        if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") ||
            !mRiffLibraryIndex["riffs"].is_array())
        {
            return;
        }

        auto& riffs = mRiffLibraryIndex["riffs"];

        for (const auto& riff : riffs)
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
            {
                continue;
            }

            const auto takes = riff.value("takes", nlohmann::json::array());

            if (takes.is_array())
            {
                for (const auto& take : takes)
                {
                    if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                    {
                        continue;
                    }

                    const auto runtimePath = util::PathFromUtf8(take["filePath"].get<std::string>());

                    if (!runtimePath.empty())
                    {
                        takeFiles.push_back(runtimePath);
                    }
                }
            }

            break;
        }

        riffs.erase(
            std::remove_if(riffs.begin(), riffs.end(),
                           [&](const nlohmann::json& riff) { return riff.value("id", std::string{}) == riffId; }),
            riffs.end());
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
    {
        return;
    }

    const bool favorite = payload.value("favorite", false);
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);

        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        {
            return;
        }

        for (auto& riff : mRiffLibraryIndex["riffs"])
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
            {
                continue;
            }

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
    {
        return;
    }

    const bool used = payload.value("used", false);
    const std::string songTitle = payload.value("songTitle", std::string{});
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);

        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        {
            return;
        }

        for (auto& riff : mRiffLibraryIndex["riffs"])
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
            {
                continue;
            }

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

    // Stored as UTF-8: as a narrow string the path would be read in the ANSI code page.
    const auto filePath = util::PathFromUtf8(take->value("filePath", std::string{}));

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

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    if (bytes.empty())
    {
        ReportErrorToUI("Riff preview", "Take WAV file is empty");
        return;
    }

    nlohmann::json preview;
    preview["audio"] = {{"id", takeId},
                        {"title", take->value("title", std::string("Riff Take"))},
                        {"data", util::EncodeBase64(bytes)},
                        {"contentType", "audio/wav"}};
    // Library takes repeat until the user stops them. That used to be the UI
    // re-requesting the whole take — re-read from disk and re-encoded — every
    // time it ended; the engine wraps it in place instead. endSec 0 means "to
    // the end", so the whole take is the loop.
    preview["region"] = {{"startSec", 0.0}, {"endSec", 0.0}, {"looping", true}};

    RiffCaptureConfig guideConfig;
    guideConfig.tempoBpm =
        ClampValue(take->value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
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
            {
                ActivateRiffGuidance(guideConfig, true);
            }
            else
            {
                DeactivateRiffGuidance(true);
            }
        }
        mDemoPreview->StartPreview(preview);
    }
}

void PluginController::HandlePreviewCapturedRiffRequest(const nlohmann::json& payload)
{
    std::shared_ptr<const RiffTake> capture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mRiffCapture && mRiffCapture->complete && !mRiffCapture->left.empty() && !mRiffCapture->right.empty())
        {
            capture = mRiffCapture;
        }
    }

    if (!capture)
    {
        ReportErrorToUI("Riff preview", "No captured take available yet");
        return;
    }

    // The whole take goes over once; the trim markers travel as a region for
    // the engine to honour. Slicing and re-encoding here is what made "Repeat"
    // a retrigger — every cycle cost an engine->UI->engine round trip plus a
    // fresh multi-megabyte encode, and the gap between cycles was audible.
    const auto wavBytes =
        util::EncodeStereo16BitWav(capture->left, capture->right, static_cast<int>(std::llround(capture->sampleRate)));

    if (wavBytes.empty())
    {
        ReportErrorToUI("Riff preview", "Unable to encode captured take");
        return;
    }

    const double sampleRate = capture->sampleRate > 0.0 ? capture->sampleRate : 1.0;
    const double totalSec = static_cast<double>(capture->left.size()) / sampleRate;
    const double startRatio = std::clamp(payload.value("startRatio", 0.0), 0.0, 1.0);
    const double endRatio = std::clamp(payload.value("endRatio", 1.0), 0.0, 1.0);

    nlohmann::json preview;
    preview["audio"] = {{"id", capture->takeId.empty() ? std::string("captured-take") : capture->takeId},
                        {"title", std::string("Captured Riff")},
                        {"data", util::EncodeBase64(wavBytes)},
                        {"contentType", "audio/wav"}};
    preview["region"] = {{"startSec", startRatio * totalSec},
                         {"endSec", endRatio * totalSec},
                         {"looping", payload.value("repeat", false)}};

    if (mDemoPreview)
    {
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            ActivateRiffGuidance(capture->config, true);
        }
        mDemoPreview->StartPreview(preview);
    }
}

/// Retunes the loop of a preview that is already playing, so dragging the trim
/// markers mid-preview moves the loop instead of leaving the audio on the range
/// it started with while the on-screen markers say otherwise.
void PluginController::HandleSetRiffPreviewRegionRequest(const nlohmann::json& payload)
{
    if (!mDemoPreview)
    {
        return;
    }

    std::shared_ptr<const RiffTake> capture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        capture = mRiffCapture;
    }

    const double totalSec =
        capture && capture->sampleRate > 0.0 ? static_cast<double>(capture->left.size()) / capture->sampleRate : 0.0;

    if (totalSec <= 0.0)
    {
        return;
    }

    const double startRatio = std::clamp(payload.value("startRatio", 0.0), 0.0, 1.0);
    const double endRatio = std::clamp(payload.value("endRatio", 1.0), 0.0, 1.0);
    mDemoPreview->SetPreviewRegion(startRatio * totalSec, endRatio * totalSec, payload.value("repeat", false));
}

std::filesystem::path PluginController::ResolveRiffLibraryPath() const
{
    if (mAppSettings.contains(kRiffLibraryPathSettingKey) && mAppSettings[kRiffLibraryPathSettingKey].is_string())
    {
        const auto configured = util::PathFromUtf8(mAppSettings[kRiffLibraryPathSettingKey].get<std::string>());

        if (!configured.empty())
        {
            return configured;
        }
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

    nlohmann::json index =
        Store().Get(storage::ItemType::kDocument, kRiffLibraryDocumentId).value_or(nlohmann::json::object());

    if (!index.is_object())
    {
        index = nlohmann::json::object();
    }

    index["path"] = util::PathToUtf8(path);

    if (!index.contains("riffs") || !index["riffs"].is_array())
    {
        index["riffs"] = nlohmann::json::array();
    }

    for (auto& riff : index["riffs"])
    {
        if (!riff.is_object() || !riff.contains("takes") || !riff["takes"].is_array())
        {
            continue;
        }

        for (auto& take : riff["takes"])
        {
            if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
            {
                continue;
            }

            const auto storedPath = util::PathFromUtf8(take["filePath"].get<std::string>());

            if (storedPath.empty())
            {
                continue;
            }

            const auto resolvedPath = ResolveRiffTakePathForRuntime(storedPath, path);
            const bool resolvedExists =
                !resolvedPath.empty() && resolvedPath != storedPath && std::filesystem::exists(resolvedPath);
            const bool storedExists = std::filesystem::exists(storedPath);

            if (resolvedExists)
            {
                take["filePath"] = util::PathToUtf8(resolvedPath);
            }
            else if (!storedExists && !resolvedPath.empty())
            {
                take["filePath"] = util::PathToUtf8(resolvedPath);
            }
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
    {
        normalizedPayload["riffs"] = nlohmann::json::array();
    }

    for (auto& riff : normalizedPayload["riffs"])
    {
        if (!riff.is_object() || !riff.contains("takes") || !riff["takes"].is_array())
        {
            continue;
        }

        for (auto& take : riff["takes"])
        {
            if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
            {
                continue;
            }

            const auto runtimePath = util::PathFromUtf8(take["filePath"].get<std::string>());
            const auto storedPath = BuildRiffTakePathForStorage(runtimePath, libraryPath);
            take["filePath"] = util::PathToUtf8(storedPath);
        }
    }

    if (!Store().Put(storage::ItemType::kDocument, kRiffLibraryDocumentId, normalizedPayload))
    {
        return false;
    }

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

    if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") ||
        !mRiffLibraryIndex["riffs"].is_array())
    {
        return std::nullopt;
    }

    for (const auto& riff : mRiffLibraryIndex["riffs"])
    {
        if (!riff.is_object())
        {
            continue;
        }

        const std::string riffTitle = riff.value("title", std::string("Riff"));
        const auto takes = riff.value("takes", nlohmann::json::array());

        if (!takes.is_array())
        {
            continue;
        }

        for (const auto& take : takes)
        {
            if (!take.is_object() || take.value("id", std::string{}) != takeId)
            {
                continue;
            }

            nlohmann::json result = take;
            result["title"] = riffTitle;
            return result;
        }
    }

    return std::nullopt;
}

void PluginController::CreateLooper(const std::function<void(const std::string&)>& sendToUI)
{
    const auto tone = [this]() {
        return LooperService::Tone{mActivePresetId, mActivePreset ? mActivePreset->name : "", mActiveSceneId};
    };
    mLooper = std::make_unique<LooperService>(
        mHost, mPresetMixer, *mMetronome, mDSPMutex, mAppSettings, [this]() { SaveAppSettings(); }, tone,
        [this]() { mPendingStateBroadcast = true; }, sendToUI);
    mLooper->RegisterMessageHandlers(mMessageHandlers);

    LooperService::ProjectHooks hooks;
    hooks.libraryPath = [this]() { return ResolveRiffLibraryPath(); };
    hooks.loadIndex = [this]() {
        return Store().Get(storage::ItemType::kDocument, kLooperProjectsDocumentId).value_or(nlohmann::json::object());
    };
    hooks.saveIndex = [this](const nlohmann::json& index) {
        return Store().Put(storage::ItemType::kDocument, kLooperProjectsDocumentId, index);
    };
    // A layer exported as a riff becomes the take waiting to be saved, as an imported WAV does.
    hooks.offerRiffTake = [this](std::shared_ptr<RiffTake> take) {
        std::shared_ptr<RiffTake> retired;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            retired = std::move(mRiffCapture);
            mRiffCapture = take;
        }

        nlohmann::json msg = BuildRiffStoppedMessage(*take);
        msg["source"] = "looper";
        msg["tempoBpm"] = take->config.tempoBpm;
        msg["timeSigNum"] = take->config.timeSigNum;
        msg["timeSigDen"] = take->config.timeSigDen;
        SendMessageToUI(msg.dump());
    };
    hooks.buildRender = [this](const std::string& presetId, const std::string& sceneId, std::string& error) {
        return BuildLooperRenderGraphs(presetId, sceneId, error);
    };
    hooks.calibrationGain = [this]() {
        return static_cast<float>(std::pow(10.0, mPresetMixer.GetUserInputCalibrationGainDb() / 20.0));
    };
    // A riff take's audio for the looper's worker: found in the riff index, decoded and resampled.
    hooks.loadRiffTake = [this](const std::string& takeId, double sampleRate, std::vector<std::vector<float>>& audio,
                                std::string& title) {
        const auto take = FindRiffTakeById(takeId);

        if (!take || !take->contains("filePath") || !(*take)["filePath"].is_string())
        {
            return false;
        }

        const auto decoded =
            util::DecodePcmWav(util::ReadFileBytes(util::PathFromUtf8((*take)["filePath"].get<std::string>())));

        if (!decoded || decoded->channelSamples.empty())
        {
            return false;
        }

        audio = util::ConvertToSampleRate(*decoded, sampleRate);
        title = take->value("title", std::string("Riff"));
        return !audio.empty();
    };
    mLooper->SetProjectHooks(std::move(hooks));
}

bool PluginController::BeginRiffRecording(std::shared_ptr<RiffTake> take, std::vector<float> left,
                                          std::vector<float> right, bool armed, double samplesPerBar)
{
    RiffRecorder::Audio retiredAudio;
    std::shared_ptr<RiffTake> retiredTake;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (mRiffRecorder->Busy())
        {
            return false;
        }

        // Starting a take lets go of the last one, finished or not, as it always has.
        retiredAudio = mRiffRecorder->Release();
        retiredTake = std::move(mRiffCapture);
        mRiffRecorder->Start(std::move(left), std::move(right), armed, samplesPerBar, take->config.countInBars);
        ActivateRiffGuidance(take->config, false);
    }

    mRiffRecordingTake = std::move(take);
    mRiffRecordingArmed = armed;
    mRiffStartedSent = false;
    mRiffProgressSent = 0;
    return true;
}

void PluginController::FinalizeRiffCapture(bool canceled)
{
    if (!mRiffRecordingTake)
    {
        // Nothing recording: a stop for the finished take says again that it stopped.
        std::shared_ptr<const RiffTake> finished;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            finished = mRiffCapture;
        }

        if (finished && finished->complete)
        {
            SendMessageToUI(BuildRiffStoppedMessage(*finished).dump());
        }

        return;
    }

    // An armed take that never heard its first note is canceled rather than kept empty. Either way
    // this stop answers for the take: nothing its recording published follows it out.
    const bool started = !mRiffRecordingArmed || mRiffRecorder->Poll().started;

    if (canceled || !started)
    {
        RiffRecorder::Audio dropped;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            dropped = mRiffRecorder->Release();
            DeactivateRiffGuidance(false);
        }

        const std::shared_ptr<RiffTake> take = std::move(mRiffRecordingTake);
        mRiffRecordingTake.reset();
        nlohmann::json msg;
        msg["type"] = "riffCaptureCanceled";
        msg["takeId"] = take->takeId;
        SendMessageToUI(msg.dump());
        return;
    }

    FinishRecordedRiff();
}

void PluginController::FinishRecordedRiff()
{
    const std::shared_ptr<RiffTake> take = std::move(mRiffRecordingTake);
    mRiffRecordingTake.reset();

    if (!take)
    {
        return;
    }

    // Closing and handing over the buffers is all the lock is held for.
    RiffRecorder::Audio audio;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        (void)mRiffRecorder->Close();
        audio = mRiffRecorder->Release();
        DeactivateRiffGuidance(false);
    }

    audio.Rotate();
    take->left = std::move(audio.left);
    take->right = std::move(audio.right);
    take->complete = !take->left.empty() && !take->right.empty();
    take->endedAt = std::chrono::steady_clock::now();

    std::shared_ptr<RiffTake> retired;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        retired = std::move(mRiffCapture);
        mRiffCapture = take;
    }

    retired.reset();
    SendMessageToUI(BuildRiffStoppedMessage(*take).dump());
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

    // The take being recorded, if there is one, or else the finished one.
    std::shared_ptr<const RiffTake> take = mRiffRecordingTake;
    const bool recording = take != nullptr;
    RiffRecorder::Status status;

    if (recording)
    {
        status = mRiffRecorder->Poll();
    }
    else
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        take = mRiffCapture;
    }

    const RiffCaptureConfig config = take ? take->config : RiffCaptureConfig{};
    nlohmann::json capture;
    capture["active"] = recording && (!mRiffRecordingArmed || status.started);
    capture["complete"] = !recording && take && take->complete;
    capture["takeId"] = take ? take->takeId : std::string{};
    capture["bars"] = config.bars;
    capture["tempoBpm"] = config.tempoBpm;
    capture["timeSigNum"] = config.timeSigNum;
    capture["timeSigDen"] = config.timeSigDen;
    capture["capturedSamples"] = recording ? static_cast<std::size_t>(status.recorded) : (take ? take->left.size() : 0);
    capture["sampleRate"] = take ? take->sampleRate : 0.0;
    capture["hasAudio"] = recording ? status.recorded > 0 : (take && !take->left.empty() && !take->right.empty());
    capture["waveformPeaks"] =
        recording ? LivePeaksToJson(*mRiffRecorder)
                  : (take ? BuildWaveformPeaks(take->left, take->right, 256) : nlohmann::json::array());
    msg["capture"] = std::move(capture);

    SendMessageToUI(msg.dump());
}

void PluginController::PublishRiffCaptureNotices()
{
    if (!mRiffRecordingTake)
    {
        return;
    }

    const RiffRecorder::Status status = mRiffRecorder->Poll();
    const RiffTake& take = *mRiffRecordingTake;

    // An armed take has heard its first note.
    if (mRiffRecordingArmed && status.started && !mRiffStartedSent)
    {
        mRiffStartedSent = true;
        const double samplesPerBeat = take.sampleRate * (60.0 / std::max(1.0, take.config.tempoBpm)) *
                                      (4.0 / static_cast<double>(std::max(1, take.config.timeSigDen)));
        const auto barSamples = static_cast<std::int64_t>(
            std::max(1.0, samplesPerBeat * static_cast<double>(std::max(1, take.config.timeSigNum))));
        nlohmann::json msg;
        msg["type"] = "riffCaptureStarted";
        msg["takeId"] = take.takeId;
        msg["bars"] = take.config.bars;
        msg["tempoBpm"] = take.config.tempoBpm;
        msg["timeSigNum"] = take.config.timeSigNum;
        msg["timeSigDen"] = take.config.timeSigDen;
        msg["countInBars"] = 0;
        // Where in its bar the take starts, counted from the end of the count-in, for the trim.
        msg["barAlignOffsetSamples"] = static_cast<std::size_t>(std::max<std::int64_t>(0, status.armLead) % barSamples);
        SendMessageToUI(msg.dump());
    }

    // About every 250 ms of the take.
    const auto interval = std::max<std::int64_t>(1, static_cast<std::int64_t>(take.sampleRate * 0.25));

    if (status.started && status.recorded >= mRiffProgressSent + interval)
    {
        mRiffProgressSent = status.recorded;
        nlohmann::json msg;
        msg["type"] = "riffCaptureProgress";
        msg["capturedSamples"] = static_cast<std::size_t>(status.recorded);
        msg["waveformPeaks"] = LivePeaksToJson(*mRiffRecorder);
        SendMessageToUI(msg.dump());
    }

    // It filled up.
    if (status.finished)
    {
        FinishRecordedRiff();
    }
}
} // namespace guitarfx
