#pragma once

/**
 * Optimized Neural FX Effect - inherits from OptimizedNAMAmpEffect.
 *
 * This is a variant of OptimizedNAMAmpEffect designed for general FX modeling
 * where the tone stack controls (bass, mid, treble, presence) are marked as
 * advanced parameters. This keeps the basic controls cleaner in the UI.
 *
 * Key improvements:
 * - Reuses all DSP processing from OptimizedNAMAmpEffect
 * - SIMD-vectorized activation functions (AVX/SSE) via inherited implementation
 * - No code duplication - only parameter registration differs
 */

#include "OptimizedNAMAmpEffect.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"

namespace guitarfx
{

/**
 * Optimized Neural FX effect variant.
 *
 * Inherits all processing from OptimizedNAMAmpEffect; only the effect type
 * identifier and parameter registration differ (tone controls marked as advanced).
 */
class OptimizedNAMFXEffect : public OptimizedNAMAmpEffect
{
public:
  [[nodiscard]] std::string GetType() const override { return "fx_nam"; }
};

inline void RegisterOptimizedNAMFXEffect()
{
  EffectTypeInfo info;
  info.type = EffectGuids::kFxNam;
  info.aliases = {"fx_nam"};
  info.displayName = "Neural FX (NAM)";
  info.category = "amp";
  info.description = "Neural FX Modeler (NAM)";
  info.requiresResource = true;
  info.resourceType = "nam";
  info.resourceFilterHint = {"pedal"};
  info.parameters = {
    {"inputGain",             "Input",               0.0,   -24.0, 24.0,  "dB",  "Level"},
    {"outputGain",            "Output",               0.0,   -24.0, 24.0,  "dB",  "Level"},
    {"bass",                  "Bass",                 0.0,   -10.0, 10.0,  "dB",  "Tone",     true},
    {"mid",                   "Mid",                  0.0,   -10.0, 10.0,  "dB",  "Tone",     true},
    {"treble",                "Treble",               0.0,   -10.0, 10.0,  "dB",  "Tone",     true},
    {"presence",              "Presence",             0.0,   -10.0, 10.0,  "dB",  "Tone",     true},
    {"mix",                   "Mix",                  1.0,    0.0,   1.0,  "amount", "Advanced", true},
    {"autoLevelOutput",       "Auto Level Output",    1.0,    0.0,   1.0,  "toggle", "Advanced", true}
  };

  EffectRegistry::Instance().Register(info.type, info, []()
  {
    return std::make_unique<OptimizedNAMFXEffect>();
  });
}

} // namespace guitarfx
