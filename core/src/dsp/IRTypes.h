#pragma once

#include <cstddef>

namespace guitarfx
{
  /**
   * IR quality modes control the tradeoff between CPU usage and audio fidelity.
   * Lower quality modes truncate the impulse response to reduce computation.
   *
   * Most cabinet IRs have 90%+ of their energy in the first 20-50ms.
   * Truncating the tail has minimal audible impact for most use cases.
   */
  enum class IRQuality
  {
    Economy,  // ~500 samples (~10ms @ 48kHz) - Lowest CPU, live performance
    Standard, // ~2048 samples (~43ms @ 48kHz) - Balanced quality/CPU
    High,     // ~8192 samples (~170ms @ 48kHz) - Studio quality
    Full      // Unlimited - Maximum fidelity, highest CPU
  };

  /**
   * Get the maximum IR length in samples for a given quality mode.
   * Returns 0 for Full quality (unlimited).
   */
  constexpr size_t GetMaxIRSamples(IRQuality quality, double sampleRate = 48000.0)
  {
    switch (quality)
    {
    case IRQuality::Economy:
      // ~10ms at any sample rate
      return static_cast<size_t>(sampleRate * 0.010);
    case IRQuality::Standard:
      // ~43ms at any sample rate
      return static_cast<size_t>(sampleRate * 0.043);
    case IRQuality::High:
      // ~170ms at any sample rate
      return static_cast<size_t>(sampleRate * 0.170);
    case IRQuality::Full:
    default:
      return 0; // Unlimited
    }
  }

  /**
   * Get a human-readable name for the quality mode.
   */
  constexpr const char *GetIRQualityName(IRQuality quality)
  {
    switch (quality)
    {
    case IRQuality::Economy:
      return "Economy";
    case IRQuality::Standard:
      return "Standard";
    case IRQuality::High:
      return "High";
    case IRQuality::Full:
      return "Full";
    default:
      return "Unknown";
    }
  }

  /**
   * Get quality mode from integer value (for parameter conversion).
   */
  constexpr IRQuality GetIRQualityFromInt(int value)
  {
    switch (value)
    {
    case 0:
      return IRQuality::Economy;
    case 1:
      return IRQuality::Standard;
    case 2:
      return IRQuality::High;
    case 3:
      return IRQuality::Full;
    default:
      return IRQuality::Standard;
    }
  }

  /**
   * Get integer value from quality mode (for parameter conversion).
   */
  constexpr int GetIntFromIRQuality(IRQuality quality)
  {
    switch (quality)
    {
    case IRQuality::Economy:
      return 0;
    case IRQuality::Standard:
      return 1;
    case IRQuality::High:
      return 2;
    case IRQuality::Full:
      return 3;
    default:
      return 1;
    }
  }

} // namespace guitarfx
