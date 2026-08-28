#pragma once

// ControlSurfaceQueue — the handoff between control input arriving on the
// audio thread and the work it asks for, which can only run on the message
// thread.
//
// MIDI arrives in the audio callback, and a MIDI-mapped setlist or scene
// change means loading a preset — which takes the DSP lock the audio thread is
// already holding. Doing it inline would deadlock, so every such request is
// parked here and drained by OnIdle instead. Only the newest request of each
// kind survives: a footswitch held down produces one preset load, not a
// hundred queued ones.
//
// Realtime rules for the audio-thread side. EnqueueMidi() never allocates:
// both vectors are reserved up front and capped, and events past the cap are
// dropped as a deliberate safety valve against a stalled message thread. The
// log queue's mutex is only taken while the UI's MIDI panel is open, and is
// held for a single push_back.

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "automation/AutomationTypes.h"

namespace guitarfx
{
class ControlSurfaceQueue
{
  public:
    /// Deferred requests: at most one of each kind, except the bank delta,
    /// which accumulates.
    struct PendingRequests
    {
        std::optional<int> setlistPresetIndex;
        std::optional<int> setlistBankDelta;
        std::optional<int> setlistBankSelect;
        std::optional<int> sceneIndex;
    };

    using SendMessageFn = std::function<void(const std::string&)>;

    explicit ControlSurfaceQueue(SendMessageFn sendMessage);

    // ── Requests parked for the message thread ──────────────────────

    void RequestSetlistPreset(int index);
    /// Accumulates rather than replaces: two bank-up presses before the
    /// queue is drained should move two banks, not one.
    void AddSetlistBankDelta(int delta);
    void RequestSetlistBankSelect(int bankNumber);
    void RequestScene(int index);

    /// Message thread: takes everything parked, clearing it.
    [[nodiscard]] PendingRequests TakePending();

    // ── MIDI ────────────────────────────────────────────────────────

    /// Audio thread: queues an event for application, and for the UI log when
    /// that is switched on. Never blocks or allocates.
    void EnqueueMidi(const MidiEvent& event);

    /// Audio thread: hands each queued event to `apply` and clears the queue.
    /// The caller is responsible for holding the DSP lock; it should skip the
    /// call entirely rather than block for it.
    void DrainMidiForApply(const std::function<void(const MidiEvent&)>& apply);

    [[nodiscard]] bool HasMidiToApply() const
    {
        return !mMidiToApply.empty();
    }

    /// Turning the log off discards anything already queued — a panel that was
    /// closed does not want a backlog when it reopens.
    void SetMidiLogEnabled(bool enabled);

    [[nodiscard]] bool IsMidiLogEnabled() const
    {
        return mMidiLogEnabled.load(std::memory_order_relaxed);
    }

    /// Message thread: formats and sends whatever the log collected. Building
    /// JSON here is the whole point of the queue — it must never happen on the
    /// audio thread.
    void PublishMidiLog();

  private:
    /// Caps chosen so a stalled message thread costs bounded memory. Both
    /// vectors are reserved to these sizes in the constructor.
    static constexpr std::size_t kMaxMidiToApply = 256;
    static constexpr std::size_t kMaxMidiLog = 512;

    SendMessageFn mSendMessage;

    std::mutex mPendingMutex;
    PendingRequests mPending;

    /// Audio thread only — no mutex, and none needed.
    std::vector<MidiEvent> mMidiToApply;

    std::atomic<bool> mMidiLogEnabled{false};
    std::mutex mMidiLogMutex;
    std::vector<MidiEvent> mMidiLog;
};
} // namespace guitarfx
