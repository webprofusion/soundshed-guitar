#pragma once

// Include all effect implementations
#include "dsp/effects/GainEffect.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/DoublerEffect.h"
#include "dsp/effects/PitchShiftEffect.h"
#include "dsp/effects/ReverbEffect.h"
#include "dsp/effects/CompressorEffect.h"
#include "dsp/effects/NAMAmpEffect.h"
#include "dsp/effects/IRCabEffect.h"
#include "dsp/effects/SimpleCabEffect.h"

namespace guitarfx
{
  /**
   * Registers all built-in effects with the EffectRegistry.
   * Call this once at application startup before loading any presets.
   */
  inline void RegisterAllEffects()
  {
    // Utility effects
    RegisterGainEffect();

    // Dynamics
    RegisterNoiseGateEffect();
    RegisterCompressorEffects(); // VCA and Opto compressors

    // EQ
    RegisterParametricEQEffect();

    // Amp models
    RegisterNAMAmpEffect();

    // Cabinet simulation
    RegisterIRCabEffect();
    RegisterSimpleCabEffect();

    // Time-based effects
    RegisterDelayEffect();
    RegisterDoublerEffect();
    RegisterReverbEffect();

    // Modulation effects
    RegisterPitchShiftEffect();
  }

} // namespace guitarfx
