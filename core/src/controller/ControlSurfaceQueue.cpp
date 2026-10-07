#include "controller/ControlSurfaceQueue.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
namespace
{
/// Human-readable name for a MIDI status nibble, for the UI's log panel.
const char* MidiTypeName(const MidiEvent& event)
{
    switch ((event.status >> 4) & 0x0F)
    {
    case 0x08:
        return "NoteOff";
    case 0x09:
        return event.data2 > 0 ? "NoteOn" : "NoteOff";
    case 0x0A:
        return "Aftertouch";
    case 0x0B:
        return "CC";
    case 0x0C:
        return "ProgramChange";
    case 0x0D:
        return "ChanPress";
    case 0x0E:
        return "PitchBend";
    default:
        return "Unknown";
    }
}
} // namespace

ControlSurfaceQueue::ControlSurfaceQueue(SendMessageFn sendMessage) : mSendMessage(std::move(sendMessage))
{
    mMidiToApply.reserve(kMaxMidiToApply);
    mMidiLog.reserve(kMaxMidiLog);
}

void ControlSurfaceQueue::RequestSetlistPreset(int index)
{
    Park(Request::Kind::SetlistPreset, index, false);
}

void ControlSurfaceQueue::AddSetlistBankDelta(int delta)
{
    Park(Request::Kind::SetlistBankDelta, delta, true);
}

void ControlSurfaceQueue::RequestSetlistBankSelect(int bankNumber)
{
    Park(Request::Kind::SetlistBankSelect, bankNumber, false);
}

void ControlSurfaceQueue::RequestScene(int index)
{
    Park(Request::Kind::Scene, index, false);
}

void ControlSurfaceQueue::Park(Request::Kind kind, int value, bool accumulate)
{
    std::lock_guard<std::mutex> lock(mPendingMutex);
    auto* const first = mPending.requests.data();
    auto* const last = first + mPending.count;
    auto* const earlier = std::find_if(first, last, [kind](const Request& request) { return request.kind == kind; });

    if (earlier != last)
    {
        if (accumulate)
        {
            value += earlier->value;
        }

        std::copy(earlier + 1, last, earlier);
        --mPending.count;
    }

    mPending.requests[mPending.count++] = Request{kind, value};
    mHasPending.store(true, std::memory_order_release);
}

ControlSurfaceQueue::PendingRequests ControlSurfaceQueue::TakePending()
{
    std::lock_guard<std::mutex> lock(mPendingMutex);
    PendingRequests taken = mPending;
    mPending = {};
    mHasPending.store(false, std::memory_order_release);
    return taken;
}

void ControlSurfaceQueue::EnqueueMidi(const MidiEvent& event)
{
    if (mMidiLogEnabled.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(mMidiLogMutex);

        if (mMidiLog.size() < kMaxMidiLog)
        {
            mMidiLog.push_back(event);
        }
    }

    if (mMidiToApply.size() < kMaxMidiToApply)
    {
        mMidiToApply.push_back(event);
    }
}

void ControlSurfaceQueue::DrainMidiForApply(const std::function<void(const MidiEvent&)>& apply)
{
    for (const auto& event : mMidiToApply)
    {
        apply(event);
    }

    mMidiToApply.clear();
}

void ControlSurfaceQueue::SetMidiLogEnabled(bool enabled)
{
    mMidiLogEnabled.store(enabled, std::memory_order_relaxed);

    if (!enabled)
    {
        std::lock_guard<std::mutex> lock(mMidiLogMutex);
        mMidiLog.clear();
    }
}

void ControlSurfaceQueue::PublishMidiLog()
{
    if (!mMidiLogEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    std::vector<MidiEvent> events;
    {
        // Copy out and clear rather than swapping: a swap would hand the
        // reserved capacity to `events` and leave mMidiLog empty, so the next
        // audio-thread push_back would allocate.
        std::lock_guard<std::mutex> lock(mMidiLogMutex);
        events.assign(mMidiLog.begin(), mMidiLog.end());
        mMidiLog.clear();
    }

    if (!mSendMessage)
    {
        return;
    }

    for (const auto& event : events)
    {
        nlohmann::json logMsg;
        logMsg["type"] = "midiLog";
        logMsg["midiType"] = MidiTypeName(event);
        logMsg["channel"] = event.status & 0x0F;
        logMsg["data1"] = static_cast<int>(event.data1);
        logMsg["data2"] = static_cast<int>(event.data2);
        mSendMessage(logMsg.dump());
    }
}
} // namespace guitarfx
