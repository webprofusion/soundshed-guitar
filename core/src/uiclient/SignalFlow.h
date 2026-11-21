#pragma once

/**
 * SignalFlow - which nodes of the active preset are passing signal, for lighting the chain's
 * cables (the web UI's signalPath/signalFlow.ts). A cable carries the signal of the node it
 * leaves. Not every node reports a level (a splitter may not), so a node without its own reading
 * is live when anything feeding it is. Each stays live a little through quiet, so a gap between
 * notes or a decaying note does not flicker it.
 *
 * It is on/off rather than a level, so the cables redraw only when a node starts or stops
 * passing signal. It is always on (it used to be the ui.signalFlowAnimation setting).
 */

#include "uiclient/ClientState.h"

#include <map>
#include <set>
#include <string>

namespace guitarfx::uiclient
{
class SignalFlow
{
  public:
    /// RMS above this counts as signal.
    static constexpr double kLiveThresholdDb = -54.0;
    /// How long a node stays live through quiet.
    static constexpr double kReleaseSeconds = 0.4;

    /// Re-reads the levels of the active preset's nodes. Returns true when any node's state flipped.
    bool Update(const ClientState& state, double nowSeconds);

    [[nodiscard]] bool IsLive(const std::string& nodeId) const
    {
        return mLive.count(nodeId) != 0;
    }

    [[nodiscard]] bool AnyLive() const
    {
        return !mLive.empty();
    }

  private:
    std::map<std::string, double> mLastHeard;
    std::set<std::string> mLive;
};
} // namespace guitarfx::uiclient
