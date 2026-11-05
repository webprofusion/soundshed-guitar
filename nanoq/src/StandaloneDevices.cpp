#include "StandaloneDevices.h"

#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace nanoq
{
namespace
{
// The level feed, as the JUCE standalone's: a lease that lapses 5 s after the last renewal, sent
// at 15 Hz while held.
constexpr double kLevelLeaseSeconds = 5.0;
constexpr double kLevelInterval = 1.0 / 15.0;
constexpr double kLevelFloorDb = -100.0;

// The test tone: a second of A440 at -12 dBFS, faded in and out so it does not click.
constexpr double kToneHz = 440.0;
constexpr float kToneGain = 0.25f;
constexpr double kToneSeconds = 1.0;
constexpr double kToneFadeSeconds = 0.01;

// Kept in clap-wrapper's settings file, beside its own keys (it carries keys it does not know).
constexpr const char* kInputMutedKey = "soundshedInputMuted";

std::atomic<DeviceBackend*> gBackend{nullptr};

double Now()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ContainsWord(const std::string& text, const std::string& word)
{
    for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
    {
        const bool startOk = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
        const std::size_t end = at + word.size();
        const bool endOk = end >= text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));

        if (startOk && endOk)
        {
            return true;
        }
    }

    return false;
}

// The JUCE standalone's feedback heuristic: a built-in microphone playing into speakers is the
// setup where an unmuted input howls, and device names are all the OS offers.
bool LooksLikeMicrophone(const std::string& name)
{
    const auto n = Lower(name);
    return n.find("microphone") != std::string::npos || n.find("mic array") != std::string::npos ||
           ContainsWord(n, "mic");
}

bool LooksLikeSpeakers(const std::string& name)
{
    const auto n = Lower(name);
    return n.find("speaker") != std::string::npos || n.find("built-in output") != std::string::npos;
}

std::string StringArg(const nlohmann::json& request, const char* key)
{
    const auto it = request.find(key);
    return it != request.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double NumberArg(const nlohmann::json& request, const char* key, double fallback)
{
    const auto it = request.find(key);
    return it != request.end() && it->is_number() ? it->get<double>() : fallback;
}

bool BoolArg(const nlohmann::json& request, const char* key, bool fallback)
{
    const auto it = request.find(key);
    return it != request.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

// The channels clap-wrapper opens: always the first ones, as many as the plugin's stereo bus takes.
nlohmann::json ChannelGroups(const std::string& device, int channels)
{
    auto groups = nlohmann::json::array();

    if (!device.empty() && channels > 0)
    {
        groups.push_back({{"name", channels >= 2 ? "1 + 2" : "1"}, {"active", true}});
    }

    return groups;
}
} // namespace

void RegisterDeviceBackend(DeviceBackend* backend)
{
    gBackend.store(backend);
}

DeviceBackend* RegisteredDeviceBackend()
{
    return gBackend.load();
}

StandaloneDevices::StandaloneDevices(Send send) : mSend(std::move(send)), mBackend(RegisteredDeviceBackend())
{
    if (const char* path = std::getenv("SOUNDSHED_NANOQ_AUDIOSTATS"))
    {
        mBlockStatsPath = path;
    }
}

bool StandaloneDevices::Supported() const
{
    return mBackend != nullptr;
}

void StandaloneDevices::Queue(const std::string& requestJson)
{
    if (mBackend == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mQueueMutex);

    // A UI renews its lease every two seconds and asks again on every change; a queue this long
    // means nothing is pumping it.
    if (mQueue.size() < 256)
    {
        mQueue.push_back(requestJson);
    }
}

void StandaloneDevices::Pump()
{
    if (mBackend == nullptr)
    {
        return;
    }

    if (!mStarted)
    {
        // The host has loaded its settings file by the time a view is up.
        mStarted = true;
        mInputMuted.store(mBackend->ReadSetting(kInputMutedKey) == "true");
    }

    std::vector<std::string> requests;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        requests.swap(mQueue);
    }

    for (const auto& request : requests)
    {
        Handle(request);
    }

    const double now = Now();

    if (mLeaseUntil > 0.0)
    {
        if (now > mLeaseUntil)
        {
            WatchLevels(false);
        }
        else if (now >= mNextLevels)
        {
            mNextLevels = now + kLevelInterval;
            SendLevels();
        }
    }

    if (!mBlockStatsPath.empty() && now >= mNextBlockStats)
    {
        mNextBlockStats = now + 1.0;
        WriteBlockStats();
    }
}

void StandaloneDevices::Handle(const std::string& requestJson)
{
    const auto request = nlohmann::json::parse(requestJson, nullptr, false);

    if (!request.is_object())
    {
        Log("[audio] ignoring a malformed audioDevice request");
        return;
    }

    const auto action = StringArg(request, "action");
    std::string error;

    if (action == "getState")
    {
        SendState({}, BoolArg(request, "rescan", false));
        return;
    }

    if (action == "watchLevels")
    {
        WatchLevels(BoolArg(request, "enabled", true));
        return;
    }

    if (action == "setDeviceType")
    {
        error = mBackend->SetApi(StringArg(request, "deviceType"));
    }
    else if (action == "setDevice")
    {
        const auto kind = StringArg(request, "kind");
        const auto name = StringArg(request, "name");

        if (kind == "linked")
        {
            error = mBackend->SetLinkedDevice(name);
        }
        else if (kind == "input" || kind == "output")
        {
            error = mBackend->SetDevice(kind == "input", name);
        }
        else
        {
            error = "Unknown device kind: " + kind;
        }
    }
    else if (action == "setInputChannels" || action == "setOutputChannels")
    {
        // clap-wrapper opens the first channels of a device and no others, so the one choice is
        // between those and none, which is the same as no device on that side.
        const bool input = action == "setInputChannels";
        const int group = static_cast<int>(NumberArg(request, "group", -1.0));

        if (group < 0)
        {
            error = mBackend->SetDevice(input, "");
        }
        else if (group > 0)
        {
            error = "The standalone app opens a device's first two channels only.";
        }
    }
    else if (action == "setSampleRate")
    {
        error = mBackend->SetSampleRate(static_cast<int>(NumberArg(request, "sampleRate", 0.0)));
    }
    else if (action == "setBufferSize")
    {
        error = mBackend->SetBufferSize(static_cast<int>(NumberArg(request, "bufferSize", 0.0)));
    }
    else if (action == "setInputMuted")
    {
        const bool muted = BoolArg(request, "muted", true);
        mInputMuted.store(muted);
        mBackend->WriteSetting(kInputMutedKey, muted ? "true" : "false");
    }
    else if (action == "playTestTone")
    {
        const auto snapshot = mBackend->Snapshot(false);

        if (!snapshot.open || snapshot.output.empty())
        {
            error = "No output device is open.";
        }
        else
        {
            mToneRequests.fetch_add(1);
        }
    }
    else if (action == "showControlPanel")
    {
        error = "This driver has no control panel.";
    }
    else if (action == "resetDevice")
    {
        error = mBackend->Restart();
    }
    else if (action == "requestInputPermission")
    {
        // Desktop audio needs no permission here.
    }
    else if (action == "setMidiInputEnabled")
    {
        error = mBackend->SetMidiInput(StringArg(request, "identifier"), BoolArg(request, "enabled", true));
    }
    else if (action == "setMidiOutput")
    {
        // clap-wrapper's standalone host drops the MIDI a plugin sends.
        if (!StringArg(request, "identifier").empty())
        {
            error = "The standalone app has no MIDI output.";
        }
    }
    else
    {
        error = "Unknown audio device action: " + action;
    }

    if (!error.empty())
    {
        Log("[audio] " + action + " failed: " + error);
    }

    SendState(error, false);
}

void StandaloneDevices::SendState(const std::string& error, bool rescan)
{
    const auto s = mBackend->Snapshot(rescan);

    nlohmann::json state;
    state["deviceTypes"] = s.apis;
    state["deviceType"] = s.api;
    state["separateIO"] = s.separateIO;
    state["inputDevices"] = s.inputs;
    state["outputDevices"] = s.outputs;
    state["inputDevice"] = s.input;
    state["outputDevice"] = s.output;
    state["deviceOpen"] = s.open;
    state["inputChannels"] = ChannelGroups(s.input, s.inputChannels);
    state["outputChannels"] = ChannelGroups(s.output, s.outputChannels);
    state["sampleRates"] = s.sampleRates;
    state["bufferSizes"] = s.bufferSizes;
    state["sampleRate"] = s.open ? s.sampleRate : 0;
    state["bufferSize"] = s.open ? s.bufferSize : 0;
    // RtAudio reports one figure for the stream; it is put on the output side.
    state["inputLatency"] = 0;
    state["outputLatency"] = s.open ? s.latency : 0;
    state["hasControlPanel"] = false;
    state["canMuteInput"] = true;
    state["inputMuted"] = mInputMuted.load();
    state["feedbackRisk"] =
        !s.input.empty() && !s.output.empty() && LooksLikeMicrophone(s.input) && LooksLikeSpeakers(s.output);
    state["inputPermission"] = "granted";

    auto midiInputs = nlohmann::json::array();

    for (std::size_t i = 0; i < s.midiInputs.size(); ++i)
    {
        // RtMidi has no stable port id; the name is what clap-wrapper saves and matches by.
        const bool enabled = i < s.midiEnabled.size() && s.midiEnabled[i];
        midiInputs.push_back({{"identifier", s.midiInputs[i]}, {"name", s.midiInputs[i]}, {"enabled", enabled}});
    }

    state["midiInputs"] = std::move(midiInputs);
    state["midiOutputs"] = nlohmann::json::array();
    state["midiOutput"] = "";
    state["showMidiOutput"] = false;

    nlohmann::json message;
    message["type"] = "audioDeviceState";
    message["available"] = true;
    message["error"] = error;
    message["state"] = std::move(state);
    mSend(message.dump());
}

void StandaloneDevices::WatchLevels(bool enabled)
{
    if (!enabled)
    {
        mLeaseUntil = 0.0;
        mMeterInput.store(false);
        mInputPeak.store(0.0f);
        return;
    }

    mLeaseUntil = Now() + kLevelLeaseSeconds;
    mMeterInput.store(true);
}

void StandaloneDevices::SendLevels()
{
    const float peak = mInputPeak.exchange(0.0f);
    const double db =
        peak > 0.0f ? std::max(kLevelFloorDb, 20.0 * std::log10(static_cast<double>(peak))) : kLevelFloorDb;

    nlohmann::json message;
    message["type"] = "audioDeviceLevels";
    message["input"] = db;
    // clap-wrapper ignores the stream status RtAudio hands its callback, so dropouts cannot be counted.
    message["xruns"] = -1;
    mSend(message.dump());
}

void StandaloneDevices::WriteBlockStats()
{
    const auto blocks = mBlocks.exchange(0);
    const auto over = mBlocksOver.exchange(0);
    const auto late = mBlocksLate.exchange(0);
    const auto givenUp = mBlocksGivenUp.exchange(0);
    const auto maxUs = mBlockMaxUs.exchange(0);
    const auto gapMaxUs = mBlockGapMaxUs.exchange(0);
    const auto busyUs = mBlockBusyUs.exchange(0);
    const auto timeUs = mBlockTimeUs.exchange(0);

    if (std::FILE* f = std::fopen(mBlockStatsPath.c_str(), "a"))
    {
        // Load is the time spent as a share of the time the blocks gave; max is the slowest block
        // against what one block has; gap is the longest from one block's start to the next.
        std::fprintf(f, "blocks %u  over %u  late %u  givenup %u  load %.0f%%  max %.2f of %.2f ms  gap %.2f ms\n",
                     blocks, over, late, givenUp,
                     timeUs > 0 ? 100.0 * static_cast<double>(busyUs) / static_cast<double>(timeUs) : 0.0,
                     maxUs / 1000.0, mBlockBudgetUs.load() / 1000.0, gapMaxUs / 1000.0);
        std::fclose(f);
    }
}

void StandaloneDevices::NoteBlockGivenUp()
{
    mBlocksGivenUp.fetch_add(1, std::memory_order_relaxed);
}

void StandaloneDevices::ProcessInput(float* const* channels, int numChannels, int frames)
{
    if (mBackend == nullptr || frames <= 0)
    {
        return;
    }

    if (!mBlockStatsPath.empty())
    {
        const auto start = std::chrono::steady_clock::now();

        if (mBlockStart != std::chrono::steady_clock::time_point{})
        {
            const auto gapUs = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(start - mBlockStart).count());

            if (gapUs > mBlockGapMaxUs.load(std::memory_order_relaxed))
            {
                mBlockGapMaxUs.store(gapUs, std::memory_order_relaxed);
            }

            // Two blocks' time between starts: this one's thread was held off for a whole block.
            if (gapUs > 2 * mBlockBudgetUs.load(std::memory_order_relaxed))
            {
                mBlocksLate.fetch_add(1, std::memory_order_relaxed);
            }
        }

        mBlockStart = start;
    }

    if (mMeterInput.load(std::memory_order_relaxed))
    {
        float peak = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (channels[ch] == nullptr)
            {
                continue;
            }

            for (int i = 0; i < frames; ++i)
            {
                peak = std::max(peak, std::fabs(channels[ch][i]));
            }
        }

        float seen = mInputPeak.load(std::memory_order_relaxed);

        while (peak > seen && !mInputPeak.compare_exchange_weak(seen, peak, std::memory_order_relaxed))
        {
        }
    }

    if (mInputMuted.load(std::memory_order_relaxed))
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (channels[ch] != nullptr)
            {
                std::memset(channels[ch], 0, sizeof(float) * static_cast<std::size_t>(frames));
            }
        }
    }
}

void StandaloneDevices::ProcessOutput(float* const* channels, int numChannels, int frames, double sampleRate)
{
    if (mBackend == nullptr || frames <= 0 || sampleRate <= 0.0)
    {
        return;
    }

    if (!mBlockStatsPath.empty())
    {
        const auto tookUs = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - mBlockStart)
                .count());
        const auto budgetUs = static_cast<std::uint32_t>(1.0e6 * frames / sampleRate);

        mBlockBudgetUs.store(budgetUs, std::memory_order_relaxed);
        mBlocks.fetch_add(1, std::memory_order_relaxed);
        mBlockBusyUs.fetch_add(tookUs, std::memory_order_relaxed);
        mBlockTimeUs.fetch_add(budgetUs, std::memory_order_relaxed);

        if (tookUs > mBlockMaxUs.load(std::memory_order_relaxed))
        {
            mBlockMaxUs.store(tookUs, std::memory_order_relaxed);
        }

        if (tookUs > budgetUs)
        {
            mBlocksOver.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const auto requests = mToneRequests.load(std::memory_order_relaxed);

    if (requests != mToneSeen)
    {
        mToneSeen = requests;
        mToneLength = static_cast<std::int64_t>(sampleRate * kToneSeconds);
        mToneRemaining = mToneLength;
        mTonePhase = 0.0;
    }

    if (mToneRemaining <= 0)
    {
        return;
    }

    const double step = 2.0 * std::numbers::pi * kToneHz / sampleRate;
    const double fade = std::max(1.0, sampleRate * kToneFadeSeconds);
    const int count = static_cast<int>(std::min<std::int64_t>(frames, mToneRemaining));

    for (int i = 0; i < count; ++i)
    {
        const auto played = static_cast<double>(mToneLength - mToneRemaining + i);
        const auto left = static_cast<double>(mToneRemaining - i);
        const double envelope = std::min({1.0, played / fade, left / fade});
        const float sample = kToneGain * static_cast<float>(envelope * std::sin(mTonePhase));
        mTonePhase += step;

        if (mTonePhase > 2.0 * std::numbers::pi)
        {
            mTonePhase -= 2.0 * std::numbers::pi;
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (channels[ch] != nullptr)
            {
                channels[ch][i] += sample;
            }
        }
    }

    mToneRemaining -= count;
}
} // namespace nanoq
