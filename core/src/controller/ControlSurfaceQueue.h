#pragma once

// ControlSurfaceQueue — the handoff between control input arriving on the
// audio thread and the work it asks for, which can only run on the message
// thread.
//
// MIDI arrives in the audio callback, and a MIDI-mapped setlist or scene
// change means loading a preset — which takes the DSP lock the audio thread is
// already holding. Doing it inline would deadlock, so every such request is
// parked here and drained on the message thread instead: by OnIdle while an
// editor drives it, and by PluginController::DrainControlSurfaceRequests(),
// which the plugin runs off a timer of its own, when none does. Only the
// newest request of each kind survives: a footswitch held down produces one
// preset load, not a hundred queued ones. They are drained in the order they
// were last asked for, so a bank change lands before the preset picked in that
// bank, as a controller sending Bank Select then Program Change means it to.
//
// Realtime rules for the audio-thread side. EnqueueMidi() never allocates:
// both vectors are reserved up front and capped, and events past the cap are
// dropped as a deliberate safety valve against a stalled message thread. The
// log queue's mutex is only taken while the UI's MIDI panel is open, and is
// held for a single push_back.

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "automation/AutomationTypes.h"

namespace guitarfx
{
class ControlSurfaceQueue
{
  public:
    /// A request parked for the message thread.
    struct Request
    {
        enum class Kind
        {
            SetlistPreset,     ///< value: the slot of the active setlist to load
            SetlistBankDelta,  ///< value: how many banks to move, up or down
            SetlistBankSelect, ///< value: the bank number to select
            Scene,             ///< value: the scene of the active preset to switch to
        };

        Kind kind = Kind::SetlistPreset;
        int value = 0;
    };

    static constexpr std::size_t kRequestKinds = 4;

    /// Deferred requests, oldest first: at most one of each kind, at the place it was last
    /// asked for. A bank delta accumulates; every other kind keeps only its newest value.
    struct PendingRequests
    {
        std::array<Request, kRequestKinds> requests{};
        std::size_t count = 0;

        [[nodiscard]] const Request* begin() const
        {
            return requests.data();
        }

        [[nodiscard]] const Request* end() const
        {
            return requests.data() + count;
        }
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

    /// Any thread: whether anything is parked, without taking the lock, so a timer polling
    /// for requests costs nothing while there are none. May briefly lag a request made on
    /// another thread; the next poll sees it.
    [[nodiscard]] bool HasPending() const
    {
        return mHasPending.load(std::memory_order_acquire);
    }

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

    /// Parks `value` for `kind` behind everything already parked, replacing an earlier request
    /// of that kind, or with `accumulate` adding the earlier one's value to it. Never allocates.
    void Park(Request::Kind kind, int value, bool accumulate);

    SendMessageFn mSendMessage;

    std::mutex mPendingMutex;
    PendingRequests mPending;
    /// Mirrors whether mPending holds anything. Written under mPendingMutex.
    std::atomic<bool> mHasPending{false};

    /// Audio thread only — no mutex, and none needed.
    std::vector<MidiEvent> mMidiToApply;

    std::atomic<bool> mMidiLogEnabled{false};
    std::mutex mMidiLogMutex;
    std::vector<MidiEvent> mMidiLog;
};
} // namespace guitarfx
