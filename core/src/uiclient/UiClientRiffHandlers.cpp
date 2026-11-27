/**
 * UiClientRiffHandlers.cpp - Riff capture and the riff library (Topic::Riffs): the engine's
 * messages, and the commands that answer them, as the web UI's riffLibrary.ts and
 * messages/captureHandlers.ts use them.
 */

#include "uiclient/Riffs.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"
#include "util/Base64.h"

#include <algorithm>

namespace guitarfx::uiclient
{
namespace
{
std::vector<float> PeaksOf(const nlohmann::json& m, const char* key)
{
    std::vector<float> peaks;

    if (const auto it = m.find(key); it != m.end() && it->is_array())
    {
        peaks.reserve(it->size());

        for (const auto& v : *it)
        {
            peaks.push_back(v.is_number() ? static_cast<float>(v.get<double>()) : 0.0f);
        }
    }

    return peaks;
}

/// The take's tempo and meter, where the message carries them.
void ReadTiming(RiffCapture& capture, const nlohmann::json& m)
{
    capture.tempoBpm = m.value("tempoBpm", capture.tempoBpm);
    capture.timeSigNum = m.value("timeSigNum", capture.timeSigNum);
    capture.timeSigDen = m.value("timeSigDen", capture.timeSigDen);
}
} // namespace

void UiClient::RegisterRiffHandlers()
{
    // Counting in, or waiting for the first note. The take is not recording yet: a stop now drops it.
    On("riffCaptureArmed", [this](const nlohmann::json& m) {
        auto& c = mState.riffs.capture;
        c = RiffCapture{};
        c.phase = RiffCapture::Phase::Armed;
        c.takeId = m.value("takeId", std::string{});
        c.bars = m.value("bars", kRiffCaptureMaxBars);
        ReadTiming(c, m);
        Notify(Topic::Riffs);
    });

    // Recording; from an armed take, once the first note arrived.
    On("riffCaptureStarted", [this](const nlohmann::json& m) {
        auto& c = mState.riffs.capture;
        c.phase = RiffCapture::Phase::Recording;
        c.takeId = m.value("takeId", c.takeId);
        c.bars = m.value("bars", c.bars);
        c.capturedSamples = 0;
        c.hasAudio = false;
        c.peaks.clear();
        c.barAlignOffsetSamples = m.value("barAlignOffsetSamples", static_cast<std::int64_t>(0));
        ReadTiming(c, m);
        Notify(Topic::Riffs);
    });

    // About every 250 ms of audio: the whole buffer's peaks so far, each message replacing the last.
    On("riffCaptureProgress", [this](const nlohmann::json& m) {
        auto& c = mState.riffs.capture;

        if (c.phase != RiffCapture::Phase::Recording)
        {
            return;
        }

        c.capturedSamples = m.value("capturedSamples", c.capturedSamples);
        c.peaks = PeaksOf(m, "waveformPeaks");
        Notify(Topic::Riffs);
    });

    // A take to trim, preview and save: captured, imported, cropped, loaded for editing, or a
    // looper layer exported as a riff (`source`).
    On("riffCaptureStopped", [this](const nlohmann::json& m) {
        auto& c = mState.riffs.capture;
        const std::string source = m.value("source", std::string{});
        c.phase = RiffCapture::Phase::Complete;
        c.takeId = m.value("takeId", c.takeId);
        c.bars = m.value("bars", c.bars);
        c.capturedSamples = m.value("capturedSamples", static_cast<std::int64_t>(0));
        c.sampleRate = m.value("sampleRate", c.sampleRate);
        c.hasAudio = m.value("hasAudio", false);
        c.peaks = PeaksOf(m, "waveformPeaks");
        c.source = source;
        c.originalTakeId = m.value("originalTakeId", std::string{});
        ReadTiming(c, m);

        // Where the first bar starts is only known for a take the engine armed itself.
        if (!source.empty())
        {
            c.barAlignOffsetSamples = 0;
        }

        ++c.serial;
        Notify(Topic::Riffs);
    });

    On("riffCaptureCanceled", [this](const nlohmann::json&) {
        mState.riffs.capture = RiffCapture{};
        Notify(Topic::Riffs);
    });

    // The pending take is in the library now; the library itself follows as riffLibraryState.
    On("riffSaved", [this](const nlohmann::json& m) {
        auto& riffs = mState.riffs;
        riffs.capture = RiffCapture{};
        riffs.lastSavedRiffId = m.value("riffId", std::string{});
        ++riffs.savedSerial;
        PostNotification({Notification::Kind::Info, "Riff saved", m.value("path", std::string{})});
        Notify(Topic::Riffs);
    });
}

// ── Commands ────────────────────────────────────────────────────────────────────

void UiCommands::GetRiffLibrary()
{
    mClient.Send("getRiffLibrary");
}

void UiCommands::SetRiffLibraryPath(const std::string& path)
{
    mClient.Send("setRiffLibraryPath", {{"path", path}});
}

void UiCommands::ImportRiffWav(const std::vector<std::uint8_t>& bytes, const std::string& fileName, double tempoBpm,
                               int timeSigNum, int timeSigDen)
{
    mClient.Send("importRiffWav", {{"data", util::EncodeBase64(bytes)},
                                   {"fileName", fileName},
                                   {"tempoBpm", std::clamp(tempoBpm, 30.0, 300.0)},
                                   {"timeSigNum", std::max(1, timeSigNum)},
                                   {"timeSigDen", std::max(1, timeSigDen)},
                                   {"patternType", "click"}});
}

void UiCommands::BrowseRiffImportFile(double tempoBpm, int timeSigNum, int timeSigDen)
{
    mClient.Send("browseRiffImportFile", {{"tempoBpm", std::clamp(tempoBpm, 30.0, 300.0)},
                                          {"timeSigNum", std::max(1, timeSigNum)},
                                          {"timeSigDen", std::max(1, timeSigDen)},
                                          {"patternType", "click"}});
}

void UiCommands::ArmRiffCapture(double tempoBpm, int timeSigNum, int timeSigDen, int countInBars, bool click)
{
    mClient.Send("armRiffCapture", {{"tempoBpm", tempoBpm},
                                    {"timeSigNum", timeSigNum},
                                    {"timeSigDen", timeSigDen},
                                    {"bars", kRiffCaptureMaxBars},
                                    {"countInBars", countInBars},
                                    {"metronomeClickEnabled", click},
                                    {"patternType", "click"}});
}

void UiCommands::StopRiffCapture(bool cancel)
{
    mClient.Send("stopRiffCapture", {{"canceled", cancel}});
}

void UiCommands::TrimCapturedRiff(double startRatio, double endRatio)
{
    mClient.Send("trimCapturedRiff", {{"startRatio", startRatio}, {"endRatio", endRatio}});
}

void UiCommands::PreviewCapturedRiff(double startRatio, double endRatio, bool repeat)
{
    mClient.Send("previewCapturedRiff", {{"startRatio", startRatio}, {"endRatio", endRatio}, {"repeat", repeat}});
}

void UiCommands::SetRiffPreviewRegion(double startRatio, double endRatio, bool repeat)
{
    mClient.Send("setRiffPreviewRegion", {{"startRatio", startRatio}, {"endRatio", endRatio}, {"repeat", repeat}});
}

void UiCommands::SaveRiffTake(const RiffSave& save)
{
    nlohmann::json payload = {{"title", save.title}, {"categories", save.categories}, {"tags", save.tags},
                              {"notes", save.notes}, {"favorite", save.favorite},     {"patternType", "click"}};

    if (!save.riffId.empty())
    {
        payload["riffId"] = save.riffId;
    }

    if (save.tempoBpm > 0.0)
    {
        payload["tempoBpm"] = save.tempoBpm;
        payload["timeSigNum"] = save.timeSigNum;
        payload["timeSigDen"] = save.timeSigDen;
    }

    if (save.bars > 0)
    {
        payload["bars"] = save.bars;
    }

    if (!save.presetId.empty())
    {
        payload["presetId"] = save.presetId;
    }

    mClient.Send("saveRiffTake", payload);
}

void UiCommands::DeleteRiff(const std::string& riffId)
{
    mClient.Send("deleteRiff", {{"riffId", riffId}});
}

void UiCommands::SetRiffFavorite(const std::string& riffId, bool favorite)
{
    mClient.Send("setRiffFavorite", {{"riffId", riffId}, {"favorite", favorite}});
}

void UiCommands::MarkRiffUsed(const std::string& riffId, bool used, const std::string& songTitle)
{
    mClient.Send("markRiffUsed", {{"riffId", riffId}, {"used", used}, {"songTitle", songTitle}});
}

void UiCommands::PreviewRiffTake(const std::string& takeId)
{
    mClient.Send("previewRiffTake", {{"takeId", takeId}, {"enableGuidance", true}});
}

void UiCommands::LoadRiffTakeForEdit(const std::string& takeId)
{
    mClient.Send("loadRiffTakeForEdit", {{"takeId", takeId}});
}
} // namespace guitarfx::uiclient
