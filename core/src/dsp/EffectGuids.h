#pragma once

/**
 * Stable UUID identifiers for all built-in effect types.
 *
 * These UUIDs are the canonical type IDs stored in preset JSON.
 * Human-readable legacy IDs (e.g. "amp_nam") are registered as
 * aliases in each effect's EffectTypeInfo::aliases so that presets
 * written before this change continue to load correctly.
 *
 * Rules:
 *  - Never change a UUID once assigned — it is the permanent identity
 *    of that effect in serialized presets.
 *  - When adding a new effect, generate a fresh UUID (v4) and add it here.
 *  - Keep the readable legacy string as the first alias entry.
 */
namespace guitarfx
{
  namespace EffectGuids
  {
    // ── Amp models ────────────────────────────────────────────────────────
    constexpr const char* kAmpBuiltin      = "1460a632-6690-4fef-ac6d-6432e3b983f8";
    constexpr const char* kAmpNam          = "2eb53b40-6139-4696-8820-387ac56ffa91";
    constexpr const char* kAmpNamOptimized = "49ea214c-91e6-41f9-bd27-ad6eec0ae90a";
    constexpr const char* kAmpNamBlend     = "8a22c0f8-413b-42c1-b9ba-d543cf011d9e";
    constexpr const char* kFxNam           = "c3263344-65e4-4b7e-b102-ea625700e12f";

    // ── Cabinet simulation ────────────────────────────────────────────────
    constexpr const char* kCabIr           = "94fa2577-e904-43b8-968b-9c569c511160";
    constexpr const char* kCabSimple       = "27e0eaa3-b023-4b5a-b783-cce65254c0d3";

    // ── Dynamics ──────────────────────────────────────────────────────────
    constexpr const char* kDynamicsGate    = "e8388de1-d262-4123-a123-8dbc56f657bc";
    constexpr const char* kCompressorVca   = "72af3541-2408-4a5c-a2dc-ba164f17eac9";
    constexpr const char* kCompressorOpto  = "9651c79e-6530-4c23-9150-aa4c0ff2f1d8";
    constexpr const char* kLimiterBrickwall= "f4094126-b5de-4c5d-8d05-d56bd8c312d1";

    // ── Distortion / drive ────────────────────────────────────────────────
    constexpr const char* kOverdrive       = "fa9e05a8-168a-4293-aa91-6b770de3da1d";
    constexpr const char* kDistortion      = "686773c9-30ac-4f33-b0f8-9222146d45b1";
    constexpr const char* kFuzz            = "3a38b19c-1d97-4989-b5bb-12bcc59d1e6b";

    // ── EQ ────────────────────────────────────────────────────────────────
    constexpr const char* kEqParametric    = "4b4025ca-64cd-4180-be79-81873b618dba";

    // ── Delay ─────────────────────────────────────────────────────────────
    constexpr const char* kDelayDigital    = "673d3e7a-e9ef-4c5d-a4c4-619dff3355ed";
    constexpr const char* kDelayDoubler    = "778aaef4-40e3-4efa-8782-6a8bfa1d1661";

    // ── Reverb ────────────────────────────────────────────────────────────
    constexpr const char* kReverbRoom      = "7467cbf1-6c7f-4f07-b5dd-a303d25b475c";
    constexpr const char* kReverbHall      = "a07ab1a5-37e5-4279-bd08-5ad640886709";
    constexpr const char* kReverbPlate     = "9e023b65-5431-48eb-95ff-4f13e7f864a2";
    constexpr const char* kReverbChamber   = "4ef25e86-9763-40bc-aca6-636b542df60b";
    constexpr const char* kReverbSpring    = "0df83b32-23d0-4530-a50e-e0824a5ccf01";
    constexpr const char* kReverbShimmer   = "7dcbb06d-8925-4f84-b412-232b7c02de26";
    constexpr const char* kReverbAmbient   = "d663f5d8-0f6e-4721-960d-81621fe41801";
    constexpr const char* kReverbAdvanced  = "92558944-f0da-4d97-ab75-bed8b63abc31";
    constexpr const char* kReverbIr        = "497d3c9d-ed6b-4c71-8e6d-0f9d61564dbc";

    // ── Modulation ────────────────────────────────────────────────────────
    constexpr const char* kChorus          = "decdd132-029a-46a5-a362-edcde007a450";
    constexpr const char* kFlanger         = "1a3f3793-7e80-4e3d-ab7b-3ce3ce032fe7";
    constexpr const char* kPhaser          = "3aa9dc81-31c2-40d5-9b1b-b0b9d1295e9b";
    constexpr const char* kTremolo         = "c9debb02-d7e7-43e3-8330-b387be46dcf4";
    constexpr const char* kAutoWah         = "b06c6d84-01b3-4d0a-ad98-40eecb64438e";
    constexpr const char* kAutoArp         = "e4a7c9d0-3b52-4f16-8a9e-2c7f1d0e5b83";

    // ── Pitch ─────────────────────────────────────────────────────────────
    constexpr const char* kPitchShift      = "0c15f065-8335-4932-9d2f-366d436ec30a";
    constexpr const char* kTranspose       = "9b89cc46-e05b-4f06-981e-1d74d1f628cf";
    constexpr const char* kOctave          = "2e4d5380-5a79-412f-bfc0-bf84ef74d561";

    // ── Utility ───────────────────────────────────────────────────────────
    constexpr const char* kGain            = "0bcd895e-5d36-4247-a351-6bed1fcb37a8";
    constexpr const char* kSplitter        = "f5f2541b-fcea-4cfd-9e62-eeddf583ef4e";
    constexpr const char* kMixer           = "d7d1e40f-9c79-4582-9a82-d5fa5bbbfb97";

    // ── Synth ─────────────────────────────────────────────────────────────
    constexpr const char* kSynthSaw        = "608e846e-0e60-4064-9c83-37c0df573c38";

  } // namespace EffectGuids
} // namespace guitarfx
