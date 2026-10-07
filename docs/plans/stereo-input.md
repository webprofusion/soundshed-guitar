# Stereo Input: Channel Layout From Effect Types

Status: **implemented** (2026-10-07). The live behaviour is described in
docs/signal-chain.md ("Channel Layout" and "Dual Mono"); this file keeps the review and the
reasoning. Where the build differs from the plan below:

- **An input or bus change re-resolves a running graph in place** rather than rebuilding it:
  the layout is a few flags per node, and nothing else needs rebuilding. A NAM node that turns
  stereo builds its right model off the DSP lock (`TakeDeferredRebuild`) and the left model
  covers both sides until it lands; other mono-capable nodes resume their right side as they are.
- **NAM loads one model on a mono connection** for the NAM amp and NAM FX types. Multi-Model NAM
  still loads both lanes.
- **The wahs get a second instance in dual mono** instead of a switch: Auto Wah shares its whole
  filter control path between the sides, not just a detector.
- **Hosted plugins run shared in dual mono**; the input status marks it (`dualMonoShared`).
- **Problem 5** is fixed in the plugin host: a plugin with a mono input hears both sides summed,
  and one with a mono output has it copied to both sides.
- Tests: `ChannelLayoutTests` (every type that says it cannot widen, under random settings; layout
  resolution; channel mode; a stereo input that comes and goes), `DualMonoTests` (every type,
  nothing crossing between the sides), and the layout cases in `MultiPresetMixerTests`.

## The design in one paragraph

Whether a connection in a chain is mono or stereo — its **layout** — is decided when the chain
is built, from the input and the effect types along the path. It never depends on measuring the
audio, nor on any knob or switch.

- **A stereo input** (a DAW stereo track, or Stereo selected in the standalone app) makes the
  whole chain stereo.
- **A mono input** keeps the chain mono until the first effect whose *type* can widen (a chorus,
  delay, reverb, cab, tremolo, mixer node, hosted plugin and so on), whatever its current
  settings. From there on it is stereo.
- **Amps, drives and the gate** run their cheap mono path only where nothing upstream could
  widen.

Because everything after anything that *could* become stereo already runs stereo, panning,
widening, switching to ping-pong or tremolo Pan, loading a stereo IR, or switching on a chorus
from a footswitch all take effect instantly. Nothing changes layout while the chain runs.
Layout changes only through a rebuild, for a different input or a structural edit.

Part 1 is that design. Part 2 is the optional dual-mono input mode.

## How stereo works today

**Input modes.**
- **Standalone** has two modes, stored in app settings as `inputChannel.monoMode` and
  `inputChannel.mono` ([PluginControllerSettings.cpp:125](../../core/src/controller/PluginControllerSettings.cpp)).
  Mono mode is the default.
  - **Mono:** the chosen input (1 or 2) is copied to both channels before anything runs
    ([MultiPresetMixer.cpp:849](../../core/src/dsp/MultiPresetMixer.cpp)).
  - **Stereo:** the pair passes through as it is.
- **Plugin (DAW) mode** is always "as provided" (`SetHostControlledInput`). The adapter accepts
  mono/mono or stereo/stereo buses, but not mono-in/stereo-out
  ([PluginProcessorAdapter.cpp:311](../../juce/Source/PluginProcessorAdapter.cpp)). A mono bus
  arrives as a null right pointer on both input and output.

**The per-block guess.** Every executor — the global pre-chain, each rig, the global
post-chain and each composite's inner graph — decides at its input node, every block, whether
its input is stereo. It calls a block stereo only when the right channel is above about
-80 dBFS *and* differs from the left (`InputPairIsStereo`, [SignalGraphExecutor.cpp:98](../../core/src/dsp/SignalGraphExecutor.cpp)).
There is no hysteresis. When a block is judged mono:
- a mono-capable node (NAM, Heavy American, the drives, the gate) processes the left channel
  only and copies the result to the right ([SignalGraphExecutorPlan.cpp:469](../../core/src/dsp/SignalGraphExecutorPlan.cpp));
- the output node copies left over right ([SignalGraphExecutor.cpp:874](../../core/src/dsp/SignalGraphExecutor.cpp)).

**Per-block stereo claims.** After an effect runs, the executor treats its output as stereo if
either:
- it reports `ProducesStereoOutput()`, from its current parameters;
- it is in the `delay` or `reverb` category, through `NodeMayProduceStereo`
  ([SignalGraphExecutorInternal.h:98](../../core/src/dsp/SignalGraphExecutorInternal.h)).

Either way it is re-decided every block, in both directions.

**Other places that decide from the audio.**
- Digital Delay compares L and R each block to pick its ping-pong input
  ([DelayEffect.h:74](../../core/src/dsp/effects/DelayEffect.h)).
- The Input Analyzer measures channels too, but it is a meter and nothing acts on it.

**Mono mode forces NAM to mono.** In standalone Mono mode, NAM nodes run their mono path even
when their input is stereo (`mNamInputModeMono`, [MultiPresetMixer.cpp:918](../../core/src/dsp/MultiPresetMixer.cpp)).

**Switching back to stereo loses state.** On each mono→stereo switch, NAM's right model, its
filters and the drives' right-channel state resume from whatever they held last time. Only
Heavy American copies its left state across (`CopyChannelState`).

## Problems found

| # | Problem | This design |
|---|---|---|
| 1 | Left bleeds into right when the right input goes quiet | Fixed: no guess |
| 2 | Mono mode collapses stereo in front of an NAM amp | Fixed: the forcing goes |
| 3 | A stereo IR file in the IR cab loses its right side | Fixed: the cab is a widening type |
| 4 | The delay-category rule overrides the delays' own answers | Fixed: types declare for themselves |
| 5 | A mono-only hosted plugin probably passes R through dry | A local fix in the plugin host |
| 6 | `setInputMode` writes without the DSP lock | Needs its own fix |
| 7 | A DAW mono track cannot get stereo out | Part of the input/output stage |
| 8 | A mono output drops the right side | Part of the input/output stage |

1. **Left bleeds into right whenever the right input goes quiet.** With two different sources
   on inputs 1 and 2 (stereo mode, or any DAW stereo track), every block in which the right
   source is under -80 dBFS is treated as mono, and the processed left plays on both outputs.
   - A quiet second source near the threshold flips the chain between paths block by block.
   - Each flip back to stereo resumes stale right-side state.
2. **Mono mode collapses stereo placed in front of an NAM amp.** The forcing only changes
   anything when something upstream has widened the signal:
   - a chorus, flanger, doubler or tremolo-pan before an NAM amp comes out as its left channel;
   - a ping-pong delay before an NAM amp loses its right-hand echoes.

   Heavy American is not forced, and a composite does not pass the flag on.
   `ModulationStereoTests` checks the built-in amp and the overdrive outside mono mode, so it
   does not cover this.
3. **A stereo IR file in the IR cab loses its right channel on a mono guitar.** The cab loads
   different left and right impulses, but `ProducesStereoOutput()` reports only slot pan and
   L/R split ([IRCabEffect.h:1080](../../core/src/dsp/effects/IRCabEffect.h)). The output node
   then copies left over right, unless a delay or reverb follows and carries the stereo on.
4. **The delay-category rule overrides the delays' own answers.** Every `delay`-category node is
   treated as stereo, so an amp after a delay always runs both channels, even with Spread at 0.
   The docs say delays are stereo "while Spread is on", which is not what happens.
5. **A mono-only hosted plugin probably passes the right channel through dry.** The plugin
   never touches work-buffer channel 1, so output R is input R times the gains
   ([JuceHostedPluginEffect.cpp:1330-1369](../../juce/Source/JuceHostedPluginEffect.cpp)).
   This is unverified: it depends on whether JUCE's format wrapper clears unused channels.
6. **`setInputMode` writes `mMonoMode` and `mInputChannel` without the DSP lock**
   ([PluginControllerGlobalChain.cpp:266](../../core/src/controller/PluginControllerGlobalChain.cpp)).
   The audio thread reads both every block. The app-settings path does take the lock.
7. **A DAW mono track cannot get stereo out.** Mono-in/stereo-out is rejected, so on a mono
   track every widening effect is reduced to its left channel. This matters most in Logic,
   where "Mono → Stereo" is the usual way to insert a guitar plugin.
8. **A mono output drops the right side instead of summing it.** On a mono DAW bus the output
   right pointer is null, so only the left side is written. A standalone output device with one
   active channel plays only channel 0 of the stereo buffer. Either way, anything on the right
   is lost rather than folded in: half of a ping-pong's echoes, the right side of a reverb or
   stereo cab, a rig panned right.

---

# Part 1: Layout from effect types

## Where the input layout comes from

**DAW: the host's bus decides.** There is no input-mode setting in the plugin.

| Track's bus | Input | Output |
|---|---|---|
| mono in, mono out | mono | mono (final mix folded, see below) |
| mono in, stereo out (newly accepted, problem 7) | mono | stereo |
| stereo in, stereo out | stereo | stereo |

The bus layout is read in `prepareToPlay`, and a change rebuilds the chains.

**Standalone: the input mode decides.**
- **Mono — input 1** (the default)
- **Mono — input 2**
- **Mono — both summed**, ½(L+R): it keeps a genuinely stereo source whole. A guitar on one input
  of a two-input interface arrives 6 dB down, which is why it is not the default.
- **Stereo**
- **Dual mono** (Part 2)

The mode is stored in app settings. Stereo and Dual mono are offered only when the device
has two active inputs, which the device manager reports. Changing the mode rebuilds the
chains.

**Output capability** comes from configuration too: the bus's output channel count, or the
device's active output channels.

## Which effect types can widen

Each effect type declares one fixed fact: whether it can ever turn a mono input into a stereo
output, under any settings.

```cpp
// True if any settings of this effect can make its two outputs differ on a mono input.
// A fixed fact about the type, not its current settings. Defaults to true: a type that is
// wrongly left at true costs CPU after it, never audio.
[[nodiscard]] virtual bool CanWiden() const { return true; }
```

**Declared false — "keeps"** (identical sides in, identical sides out):
- **Amps and drives:** NAM Amp, NAM FX, Multi-Model NAM, Heavy American, Overdrive,
  Distortion, Fuzz.
- **Level and EQ:** Parametric EQ, Graphic EQ, both compressors, Noise Gate, Limiter, Gain,
  Input Analyzer.
- **Modulation that moves both sides together:** Phaser, Vibe, Wah, Auto Wah.
- **Pitch and synth:** Octave, Pitch Shift, Transpose (all three), Auto Arp, Synth Saw,
  Guitar to MIDI.
- **Graph nodes:** the splitter.

**Left at true — can widen:**
- **Delays:** Digital, Analog, Tape, Doubler.
- **Modulation:** Chorus, Flanger, Tremolo, Rotary, Ring Mod, Harmonizer.
- **Reverbs:** all six; also 3D Spatial.
- **Cabs:** Simple Cab, IR Cab.
- **Graph nodes:** the Mixer node.
- **Opaque:** WASM effects, hosted plugins.

**Composite:** can widen if any node in its inner graph can.

The failure modes are lopsided, which is why the default is true:
- a widening type wrongly declared "keeps" would let an amp after it run mono and drop the
  right side, so a test covers every "keeps" declaration (see Tests);
- a "keeps" type left at the default only makes what follows it run stereo.

`ProducesStereoOutput()` and `NodeMayProduceStereo` are deleted. No effect has to report what
it is doing at any moment.

## How layout is resolved

Each executor resolves its graph in `SetGraph()`, in topological order. The executor's input
node has the executor's input layout. For every other node:

- **its input layout** is stereo if any incoming connection is stereo;
- **its output layout** is:
  - **mono** if the node's channel mode is Mono (below);
  - otherwise **stereo** if its input is stereo or its type can widen;
  - otherwise **mono**;
- **it runs its mono path** (`ProcessMono` on one channel, copied to the other) only when its
  input is mono, it supports mono processing, and it cannot widen.

Everything else runs `Process` on both channels. On a mono connection both channel buffers
hold the same samples, so a "keeps" effect without a mono path simply processes two identical
channels, as it does today.

**Between executors:**
- **pre-chain:** the input layout;
- **each rig:** the pre-chain's output layout;
- **post-chain:** stereo, because the Multi-Rig mix bus is stereo (rig pan lives there);
- **composite:** its node's input layout in the parent graph.

A change to any of these rebuilds what depends on it, through the existing build-and-crossfade
path.

**The output node** writes both channels as they are. A mono connection already holds
identical sides, so nothing is copied over anything.

**Deleted:**
- `InputPairIsStereo` and the per-block `hasStereoSignal`;
- `ProducesStereoOutput`, `NodeMayProduceStereo` and the mixer node's per-block pan check;
- `mNamInputModeMono`;
- `LastOutputWasStereo` (a composite's layout is known at build);
- Digital Delay's sample comparison;
- the mono paths on widening types (Analog Delay, Tape Delay, Ring Mod), which can no longer
  run.

`channelCount` telemetry reads the resolved layout.

## Why this meets "adapt live"

A user can only make something stereo through an effect whose type can widen, and everything
after such an effect is already stereo. Panning a cab slot, raising a delay's Spread, switching
a tremolo to Pan, loading a stereo IR, or enabling a bypassed chorus from a footswitch: the
right side is already being processed, so the change is heard in that block. Nothing switches,
so nothing needs a handover, nothing is left stale, and no tail is cut off.

## Node channel mode

A new optional field on `GraphNode`, `channelMode`. It is additive, so old presets read as
Follow.

- **Follow** (the default): the node takes whatever layout arrives.
- **Mono**: the node folds a stereo input, runs mono, and outputs mono. The fold is a choice:
  - **sum** (the default);
  - **left**;
  - **right**.

  A widening type set to Mono has its output summed too. A Doubler then correctly cancels to
  its dry signal, as it would on a mono speaker. Connections after a Mono node are mono again,
  until the next widening type.

This is how you put one amp after a stereo chorus, the way a single real amp would hear it, and
it is the CPU control for an amp placed after a widening effect. It appears in the node's params
panel. Changing it rebuilds the chain.

## NAM loads one model on a mono connection

A NAM node whose input is mono needs only its left model. Hand effects their resolved layout
before `Prepare` (`SetChannelLayout`), and have NAM skip the right model there. That halves NAM
memory and load work for an amp with nothing that can widen in front of it, which covers 83% of
the preset graphs measured (below). A structural edit that puts a widening effect in front of
the amp rebuilds, and loads the second model then.

## Hosted plugins that only load in mono

A hosted plugin is treated as able to widen, so the path after it is stereo. When the plugin
would only load with a mono layout, `JuceHostedPluginEffect` copies its output channel 0 into
channel 1, instead of passing input R through (problem 5). That is a local fix, independent of
the rest.

## Input and output stage

**Input fold.** In the standalone app, the input fold happens once, before the pre-chain, per
the input mode. The tuner reads the folded input: input 1 when Stereo is selected. The demo
preview and signal-test injectors already write both channels before the fold, so they need no
change.

**Mono output.** On a mono output (a one-channel bus or device), the final mix is folded
½(L+R) into the one channel (problem 8). A mono chain (L = R) folds to exactly itself, so its
level doesn't move.

## Ping-pong with identical sides

A ping-pong delay sits on a stereo path whenever it is downstream of a stereo input, and on a
mono path it receives identical sides. Today it compares the samples to decide; under this
design it would receive identical sides in more places, for example a mono clip on a DAW stereo
track.

Build the delay's input from mid and side, so it needs no decision:
- **delay in L = 0.8·M + S**
- **delay in R = 0.2·M − S**

where M = (L+R)/2 and S = (L−R)/2.
- With identical sides, S is 0 and this is exactly today's mono behaviour.
- With a stereo input, the side content keeps its place and the mid still bounces.

The mid of a genuinely stereo input enters at half today's total level; tune the skew gain if
that matters.

## Reporting

- **Signal path view.** Each node shows its resolved layout, mono or stereo, which only changes
  when the chain is rebuilt. The input stage shows where its layout came from: "Stereo (track)",
  "Mono — input 1".
- **Capabilities.** Show Stereo and Dual mono only when two inputs exist, and show a mono tag
  when the output is mono.
- **Input Analyzer "Channels"** shows the configured layout. Its measurement becomes a hint,
  never a decision:
  - "input 2 is silent" while Stereo is selected;
  - "both inputs are identical", on a DAW stereo track carrying a mono clip, as a pointer
    that a mono track would halve the amp cost.
- **Protocol.** `setInputMode` gains a `mode` field; `monoMode` stays for compatibility, and
  `inputModeChanged` reports the mode, its source and the capabilities. Update
  `core/protocol/ui-messages.json` and `docs/user-interface.md`. Do it in both UIs: WebView
  `controls.ts`, and Nano's `ControlsSheet.cpp`.
- **Docs:**
  - `signal-chain.md`: replace "Stereo Preservation" with the resolution rules.
  - `fx-library.md`: which types can widen, the analyzer's Channels row, ping-pong.
  - `data-models.md`: `channelMode`.

## What it costs, measured

The 455 preset graphs in the preset packs being built (327 presets and their scenes), sorted by
what sits in front of their amps:

| Graphs | Situation | Amp layout here | Against today |
|---|---|---|---|
| 378 (83%) | Nothing that can widen before any amp | mono | same |
| 43 (9%) | Amp after an enabled delay or reverb | stereo | already stereo in a DAW today |
| 12 (3%) | Amp after a widening effect that is active | stereo | already stereo in a DAW today |
| 22 (5%) | Amp after only bypassed chorus, flanger or delay, or Ring Mod at Spread 0 | stereo | newly stereo while those are off |

- **Standalone Mono mode.** Today the NAM amps in the lower three rows (77 graphs) are forced
  to mono, collapsing whatever stereo was in front of them. Those presets get their stereo
  back, and those NAM amps roughly double in cost: about +60 µs per NAM model per 64-sample block at
  48 kHz, against a 1333 µs deadline.
- **Peak cost.** In the 22, the peak is the same as a design that switches live: those effects
  are there to be switched on, and when they are, the amp has to run stereo anyway. Real-time
  headroom is set by the peak.
- **The CPU control.** Setting that amp's channel mode to Mono restores today's cost, with a
  summed input rather than the left side.

## What users will notice

- **Most presets: no change.**
- **Amps after a widening effect** keep the stereo and cost more, as above.
- **DAW stereo tracks are fully stereo, so every amp runs both sides.** For a mono clip on a
  stereo track the sound is unchanged, but amp CPU roughly doubles against today, where the
  guess ran it mono. A mono track, or mono-in/stereo-out, gives mono processing. Existing
  projects behave by their tracks' layouts, with nothing to migrate.
- **Standalone Stereo:** both sides are processed. A guitar on input 1 only plays on the left,
  and the analyzer hint says so.
- **Live changes** to anything that widens are heard at once.
- **A mono output sums** instead of dropping the right side.

## Tests

**Every "keeps" declaration is checked.** For each type declared unable to widen, process
identical left and right input under many random settings (seeded), and the two outputs must be
bit-identical every time. A type that secretly widens — a per-channel random seed, an LFO
offset — fails here instead of dropping audio in use.

**Resolution:** a pure function of the graph and its input layout, so test it without audio:
- mono input through drive → amp, and chorus → amp;
- stereo input;
- mixer and splitter nodes;
- a composite;
- each channel mode, including a widening effect in Mono and the path after it.

**Live changes** with an amp after a bypassed chorus: enabling it and raising Depth gives stereo
output in that block, with no step in the amp's output.

**Never from audio.** In Stereo, the right input alternates between silence and signal every few
blocks:
- no node's layout changes;
- nothing from the left input reaches the right output through a chain of "keeps" nodes.

**Inputs and outputs:**
- DAW buses: mono, mono-in/stereo-out, stereo;
- the mono-output fold at unchanged level;
- ping-pong with identical sides and with a stereo input;
- a mono-only hosted plugin outputs identical sides.

**Regression across revisions.** Run `tools/audio-ab` against HEAD. Every single effect at
defaults should come out identical. The chains that change should be only those with a widening
effect before an amp.

**Existing tests that change:**
- `SignalGraphExecutorTests` 7b-2 and 7b-3, which pin the guess and the NAM forcing;
- `ModulationStereoTests`, which tests per-block claims; it moves to type capability and
  resolution;
- `MultiPresetMixerTests`, the input-mode cases.

## Alternatives considered

- **The per-block guess** (today): problems 1 and 2.
- **Re-deciding both ways every block from parameters.** Going back to mono needs hold timers so
  a stereo tail isn't cut off, waiting out each downstream node's settle time, and protection
  against a swept knob flipping paths repeatedly.
- **Switching live, one way, from parameters,** with a handover at each downstream node.
  Rejected after measuring:
  - it saves CPU only while a widening effect in front of an amp is off (22 of 455 graphs),
    and never lowers the peak;
  - it needs exact "am I stereo now" answers from 17 effects, where a wrong one drops the
    right side;
  - it needs right-side handovers in 8 effect types, including an approximate crossfade for
    NAM, which has no state copy;
  - it makes layout depend on history (stereo until the next rebuild).
- **Rebuilding whenever a control changes the layout.** Not live: a pan or width move would wait
  for a rebuild and a crossfade.

---

# Part 2: Dual mono

## Verdict

Feasible on top of Part 1. Dual mono is a stereo layout with one more rule: **nothing crosses
between sides at all, not even through a pan or a reverb tank.** It is an input choice:
- standalone: an input mode;
- DAW: a per-instance option on a stereo bus. The bus still says stereo; dual mono says how to
  process it.

Switching into or out of it rebuilds. The path is stereo from the input, as in Part 1's
stereo case.

- **Already fine:** 25 of the 46 node types surveyed already keep left and right apart.
- **Small switch:** 5 more need a small switch inside the effect.
- **Second instance:** fourteen mix the sides, on purpose (reverb tanks, rotary, 3D spatial)
  or through a shared detector (pitch effects, synth, ring mod tracking). Those get a second
  instance when the chain is built.

Hosted third-party plugins are the one real obstacle. A second instance cannot be kept in step
with the plugin's own editor, so the first version runs them shared and says so in the UI.

**Dual mono needs both two inputs and two outputs.** On a mono output it can't be heard, so it
falls back to Mono — both summed.

## What dual mono means

**Each output side is what that side would sound like if its own input were the only, mono,
input.** For every node:
- the left lane runs on the left input duplicated to both channels, and keeps its left output;
- the right lane runs on the right input duplicated, and keeps its right output.

This definition ("side semantics") has three useful properties:
- No signal can pass between sides, by construction.
- For a single node fed identical inputs, dual mono gives exactly its stereo output, so it can
  be tested bit-exact.
- Effects that already process each channel separately are already dual mono, including the
  ones that voice the right side differently on purpose (chorus's +90° LFO, Spread, the
  doubler's polarity). They need no change and no second instance.

Two consequences need accepting:
- ping-pong cannot bounce between sides, so each side gets a plain delay of its own signal;
- a preset with two amps panned hard left and right gives each side one amp, which is what
  each side of that rig sounds like on a stereo path.

The alternative is to fold each lane to mono, (L+R)/2. That gives each side a complete mono rig
but cancels polarity-based wideners like the doubler, and it needs two whole executors per rig
(below).

## Effect survey

How each effect's `Process` treats a genuinely stereo input (three code readers, spot-checked).
"Twin" means it needs a second instance in dual mono. This is a different question from Part 1's
"can it widen": a "keeps" type can still mix two *different* sides, through a linked detector.

**Already separate per channel — no change.**
- **Amps and drives:** Distortion, Fuzz, Overdrive, Heavy American, NAM Amp, NAM FX,
  Multi-Model NAM. NAM's stereo path is two full models on `DualLaneExecutor`.
- **Level and EQ:** Gain, Graphic EQ, Parametric EQ, Limiter, Input Analyzer (its telemetry
  sums, its audio does not).
- **Cabs:** Simple Cab, Spread included.
- **Delays:** Analog Delay, Tape Delay, Digital Delay in normal mode (Spread delays only R).
- **Modulation:** Chorus, Flanger, Phaser, Tremolo (all modes), Vibe, Doubler.
- **Pitch:** STFT Transpose, Hybrid Transpose.
- **Executor nodes:** the Mixer node. Its pan is a balance: L stays L.

**Coupled, but a small dual-mono switch inside the effect removes it — no twin.**
- **Compressor (both types) and Noise Gate:** linked by default, one detector from the louder
  side. Both already have an unlinked path (`stereoLink` = 0). Dual mono forces it, and gives
  the gate's swell detector one per side.
- **Wah, Auto mode:** one envelope from max(|L|,|R|). Needs an envelope per channel (small).
- **IR Cab with a slot panned:** folds the slot to mono, then pans it. In dual mono, apply pan
  as a balance instead.
- **Digital Delay, ping-pong:** swaps feedback between sides. In dual mono, keep feedback per
  side, so each side gets a plain delay of its own signal.
  - This departs from strict side semantics on purpose: a second instance would give each side
    only every other echo.
- **IR Reverb with a 4-channel true-stereo IR:** adds the LR/RL paths. In dual mono, drop them.
  Mono and 2-channel IRs are already separate.

**Mix the sides by design — twin.**
- **Reverbs:** Reverb (Room/Chamber/Advanced), Spring and Ambient all sum the input into a
  shared tank and cross-feed it. The Room/Chamber/Advanced and Spring reverbs also share a
  ducking or drip envelope.
- **Rotary:** one rotor fed (L+R)/2.
- **3D Spatial:** one point source fed (L+R)/2.
- **Pitch:**
  - Octave, Pitch Shift and Transpose in High Quality mode use Signalsmith Stretch with 2
    channels, which shares peaks and phase between them.
  - Pitch Shift and Transpose in Low Latency mode, Harmonizer and Auto Arp use
    SpliceTransposer or TimeDomainPitchShifter, which share one tap and analyse the mid
    signal. Auto Arp's trigger reads the left channel only.
  - Ring Mod's Tracking mode feeds one pitch tracker (L+R)/2.
- **Synth:** Synth Saw tracks (L+R)/2 and plays the same synth on both sides.
- **Guitar to MIDI:** detects from (L+R)/2 and makes one note stream. Its audio passes through
  per channel.
- **WASM effects:** opaque. The guest sees both channels, so assume they mix.

**Special cases.**
- **Composite:** resolves its inner graph in dual mono, so it is separate per channel by
  delegation.
- **Hosted plugin:** opaque. A second instance can be created (state round-trips through
  `getStateInformation`), but the plugin's editor drives only one instance. Licence-limited
  plugins may refuse a second copy, and free-running LFOs drift apart.

**Global chains.**
- **Pre-chain:** the gate is linked (switch it off as above); the transpose is a twin.
- **Post-chain:** the EQ and doubler are already separate.
- **Output fold:** runs per side.
- **Tuner:** reads input 1.

## Design

### Second instances, decided at build time (recommended)

Each effect type also declares whether it keeps the channels separate, as a second fixed fact
beside `CanWiden()`. **The default is coupled**, so a new effect is correct (if costlier) until
someone declares it separate. Effects in the "small switch" group get a `SetDualMono(bool)`
call before `Prepare`.

When `SetGraph()` builds a graph for dual mono:
- **Separate nodes** run their normal stereo `Process`, which already is dual mono.
- **Coupled nodes** get a second instance, stored in `NodeState` with its own scratch pair.
  Each block:
  - the primary runs on (L,L) and keeps its left output;
  - the second instance runs on (R,R) and keeps its right output.

  Heavy nodes can run their two halves on `DualLaneExecutor`, as NAM already does.

**Every node operation reaches both instances.** The executor applies them:
- enable, `SetParam`, `SetConfig`, type config defaults, resource loads;
- deferred rebuilds, both take and commit;
- tempo, `Reset`, `Prepare`, notes;
- the realtime automation target, which gets a second pointer.

The second instance's runtime-config callback is not attached. Only four controller call sites
reach a node's processor directly, and all of them are hosted-plugin or read-only paths.

### Alternative: two whole executors per rig

Run the preset twice, each copy fed one input as mono, and combine their outputs. Only this
allows the "fold" semantics, or keeping each lane's full stereo and panning the two (two
players into one PA). But:
- every node that is not mono-capable runs twice;
- every route in `MultiPresetMixer`, `PresetVoicePool` and `GlobalChainEngine` has to fan out:
  node edits, automation targets, deferred rebuilds, telemetry, spectrum taps and latency;
- preset build time doubles.

Not recommended unless fold or two-players-one-PA is the real goal.

### Not the same feature: per-rig input routing in the Multi-Rig

Letting each Multi-Rig slot take input 1, input 2 or both would serve "two players, two
different presets" — two rigs from two inputs — directly. It does not give one preset two
locked lanes. It is a separate feature, and with Part 1's input fold in place it is mostly a
per-slot input choice.

## Cost

Against stereo, dual mono adds only the second instances:
- each algorithmic reverb, pitch effect or WASM node doubles;
- memory: each second instance is ~0.4–0.5 MB for an algorithmic reverb, a few hundred KB for
  the pitch effects, and a full module for WASM;
- NAM and the IR cab need no second instance.

## Tests

- **No leakage, across the registry.** For every registered effect (at defaults, and with its
  stereo controls engaged), in a dual-mono executor, changing input R must leave output L
  bit-identical, and the reverse. Also run it through a composite and the default global
  chains.
- **Equal to stereo on identical inputs.** For a single node with left = right, dual-mono
  output equals stereo output on both sides. This holds per node, not per chain.
- **Both instances stay in step.** After each node operation the executor supports, the primary
  and the second instance report the same parameters, enable state and config.

---

## Decided

- **The DAW's bus decides the input layout.** There is no plugin input-mode setting.
- **Anything a user can make stereo responds live.** This holds by construction: everything
  after a type that can widen is already stereo.
- **Layout comes from effect types, not their settings,** and changes only through a rebuild.

## Open decisions

1. **Fold for a node in Mono channel mode:** sum (recommended), or left.
2. **Existing presets with an amp after a widening effect.** Leave them on Follow
   (recommended): they regain their stereo and cost more CPU. The alternative is to set those
   amps to Mono on load, which keeps today's CPU but sums what reaches the amp.
3. **Dual mono:**
   - in a DAW, a per-instance option on a stereo bus (recommended);
   - side semantics (recommended);
   - hosted plugins shared and flagged in v1 (recommended);
   - Guitar to MIDI from input 1 (recommended).

## Suggested order

1. **Fixes that don't depend on the design:**
   - problem 6 (the lock);
   - verify problem 5 with a mono-only plugin, then copy channel 0 to 1 in the plugin host;
   - the ping-pong mid/side input.
2. **Layout core:**
   - the "keeps" declarations, with the random-settings test written first;
   - resolution in `SetGraph()`, and layouts handed to each executor at build;
   - the deletions.

   Then the resolution tests and an `audio-ab` comparison against HEAD. The IR cab needs no
   edit: it keeps the default.
3. **Input and output stage:**
   - DAW bus layouts, including mono-in/stereo-out;
   - standalone input modes and capabilities;
   - the mono-output fold;
   - protocol, both UIs, the analyzer, docs.
4. **Node channel mode:** schema, params-panel control, rebuild.
5. **NAM loads one model on a mono connection.**
6. **Dual mono** (Part 2): the leakage test first, then second instances, the small switches,
   and the settings.
7. **Later:** one Signalsmith channel per side; hosted-plugin second instances; per-slot input
   routing in the Multi-Rig.
