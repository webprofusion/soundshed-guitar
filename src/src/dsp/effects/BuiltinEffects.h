#pragma once

// Include all effect implementations
#include "dsp/EffectProcessor.h"
#include "dsp/effects/GainEffect.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/DoublerEffect.h"
#include "dsp/effects/PitchShiftEffect.h"
#include "dsp/effects/ReverbEffect.h"
#include "dsp/effects/CompressorEffect.h"
#include "dsp/effects/NAMAmpEffect.h"
#include "dsp/effects/OptimizedNAMAmpEffect.h"
#include "dsp/effects/MultiModelNAMAmpEffect.h"
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
    {
      EffectTypeInfo splitterInfo;
      splitterInfo.type = "splitter";
      splitterInfo.displayName = "Splitter";
      splitterInfo.category = "utility";
      splitterInfo.description = "Split signal into parallel branches";
      splitterInfo.requiresResource = false;
      EffectRegistry::Instance().Register("splitter", splitterInfo, []()
        { return std::make_unique<PassthroughProcessor>(); });

      EffectTypeInfo mixerInfo;
      mixerInfo.type = "mixer";
      mixerInfo.displayName = "Mixer";
      mixerInfo.category = "utility";
      mixerInfo.description = "Mix parallel branches";
      mixerInfo.requiresResource = false;
      EffectRegistry::Instance().Register("mixer", mixerInfo, []()
        { return std::make_unique<PassthroughProcessor>(); });
    }

    // Dynamics
    RegisterNoiseGateEffect();
    RegisterCompressorEffects(); // VCA and Opto compressors

    // EQ
    RegisterParametricEQEffect();

    // Amp models
    RegisterNAMAmpEffect();
    RegisterOptimizedNAMAmpEffect();  // SIMD-optimized version
    RegisterMultiModelNAMAmpEffect();  // Multi-model blend

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
