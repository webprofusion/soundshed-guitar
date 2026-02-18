#pragma once

// Include all effect implementations
#include "dsp/EffectProcessor.h"
#include "dsp/effects/GainEffect.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/DoublerEffect.h"
#include "dsp/effects/PitchShiftEffect.h"
#include "dsp/effects/TransposeEffect.h"
#include "dsp/effects/ReverbEffect.h"
#include "dsp/effects/CompressorEffect.h"
#include "dsp/effects/OverdriveEffect.h"
#include "dsp/effects/DistortionEffect.h"
#include "dsp/effects/FuzzEffect.h"
#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/ChorusEffect.h"
#include "dsp/effects/FlangerEffect.h"
#include "dsp/effects/PhaserEffect.h"
#include "dsp/effects/TremoloEffect.h"
#include "dsp/effects/AutoWahEffect.h"
#include "dsp/effects/OctaveEffect.h"
#include "dsp/effects/SynthSawEffect.h"
#include "dsp/effects/LimiterEffect.h"
#include "dsp/effects/NAMAmpEffect.h"
#include "dsp/effects/OptimizedNAMAmpEffect.h"
#include "dsp/effects/MultiModelNAMAmpEffect.h"
#include "dsp/effects/IRCabEffect.h"
#include "dsp/effects/IRReverbEffect.h"
#include "dsp/effects/MixerEffect.h"
#include "dsp/effects/SimpleCabEffect.h"
#include "dsp/effects/CompositeEffectProcessor.h"

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
      mixerInfo.description = "Mix parallel branches with per-input level, pan, and delay";
      mixerInfo.requiresResource = false;
      mixerInfo.parameters = {
        {"masterLevel", "Master", 0.0, -60.0, 12.0, "dB"}
        // Per-input params (level_N, pan_N, delay_N, mute_N) are dynamic
      };
      EffectRegistry::Instance().Register("mixer", mixerInfo, []()
        { return std::make_unique<MixerEffect>(); });
    }

    // Dynamics
    RegisterNoiseGateEffect();
    RegisterCompressorEffects(); // VCA and Opto compressors
    RegisterLimiterEffect();
    RegisterOverdriveEffect();
    RegisterDistortionEffect();
    RegisterFuzzEffect();

    // EQ
    RegisterParametricEQEffect();

    // Amp models
    RegisterBuiltinAmpEffect();
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
    RegisterIRReverbEffect();

    // Modulation effects
    RegisterPitchShiftEffect();
    RegisterTransposeEffect();
    RegisterChorusEffect();
    RegisterFlangerEffect();
    RegisterPhaserEffect();
    RegisterTremoloEffect();
    RegisterAutoWahEffect();
    RegisterOctaveEffect();

    // Synth effects
    RegisterSynthSawEffect();

    // Note: Composite effects are registered dynamically by CompositeEffectLibrary
    // after loading definitions from disk. They are not part of static registration.
  }

} // namespace guitarfx
