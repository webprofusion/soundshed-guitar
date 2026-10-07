#pragma once

/**
 * BankSelectLatch.h — MIDI Bank Select, held for the Program Change it belongs to.
 *
 * A controller recalls "preset 3 of bank 5" by sending Bank Select (CC0, the MSB, or CC32, the
 * LSB, or both) and then a Program Change, on one channel. The bank takes effect with that
 * program change, not on its own. In Soundshed a bank is a setlist, found by its `bank` number,
 * so a program change mapped to a setlist preset selects the setlist its bank select named first.
 *
 * Controllers differ in which half they send the bank in: some use CC0, some CC32, and some send
 * both with the other half 0. So when only one half is non-zero, that half is the bank number;
 * when both are, they make the 14-bit number MSB x 128 + LSB.
 *
 * A Bank Select belongs to the next Program Change on its channel, whatever that is mapped to,
 * and that program change takes it. A program change sent on its own picks from the active
 * setlist: the bank last selected, unless the UI or another control has changed it since, as a
 * hardware unit's front panel would.
 *
 * Audio thread only, under mDSPMutex with the rest of AutomationSlotTable::HandleMidi. Allocates
 * and locks nothing.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace guitarfx
{
class BankSelectLatch
{
  public:
    static constexpr int kMsbController = 0;
    static constexpr int kLsbController = 32;

    /// Whether CC `controller` is a Bank Select.
    [[nodiscard]] static constexpr bool IsBankSelect(int controller)
    {
        return controller == kMsbController || controller == kLsbController;
    }

    /// Holds a Bank Select CC's value for `channel` (0-15). Any other CC is ignored.
    void Note(int channel, int controller, int value)
    {
        if (!IsChannel(channel))
        {
            return;
        }

        auto& held = mHeld[static_cast<std::size_t>(channel)];

        if (controller == kMsbController)
        {
            held.msb = value;
        }
        else if (controller == kLsbController)
        {
            held.lsb = value;
        }
    }

    /// For a Program Change on `channel`: the bank selected ahead of it, which it takes, or
    /// nothing when no Bank Select came since the channel's last Program Change.
    [[nodiscard]] std::optional<int> Take(int channel)
    {
        if (!IsChannel(channel))
        {
            return std::nullopt;
        }

        const Held held = std::exchange(mHeld[static_cast<std::size_t>(channel)], Held{});

        if (held.msb < 0 && held.lsb < 0)
        {
            return std::nullopt;
        }

        const int msb = std::max(held.msb, 0);
        const int lsb = std::max(held.lsb, 0);
        return msb > 0 && lsb > 0 ? msb * 128 + lsb : msb + lsb;
    }

  private:
    /// Each half as last sent, or -1 when it has not been.
    struct Held
    {
        int msb = -1;
        int lsb = -1;
    };

    static constexpr bool IsChannel(int channel)
    {
        return channel >= 0 && channel < 16;
    }

    std::array<Held, 16> mHeld{};
};
} // namespace guitarfx
