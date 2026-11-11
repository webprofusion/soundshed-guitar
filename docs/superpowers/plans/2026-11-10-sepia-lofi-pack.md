# Sepia: Lo-Fi Effects and Factory Preset Pack — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn this Soundshed Guitar fork into "Sepia": rebranded, offline, with five lo-fi cabinet voicings, a Tape effect, a Bitcrusher, and about 60 original factory presets that need no external files.

**Architecture:** Every addition follows an existing pattern in `core/`. Cabinets extend the `simple_cab::kCabinets` table. Each new effect is one header-only `EffectProcessor` registered through `EffectRegistry`, reusing `TapeTransport`/`TapeDropout` from `TapeTransport.h`. Presets are generated from a compact definition file into the existing factory archive format (`presets.json` inside a zip in `core/ui/presets/factory/`).

**Tech Stack:** C++20, CMake, CTest (plain executables with a `Check()` helper), TypeScript UI (`core/ui`), Python 3 for the preset pack generator.

**Spec:** `docs/superpowers/specs/2026-11-10-lofi-ve-preset-paketi-design.md`

## Global Constraints

- Licence stays AGPL-3.0; `LICENSE`, `THIRD_PARTY_NOTICES.md` and attribution to the upstream project are kept.
- The names "Soundshed" and "Soundshed Guitar" do not appear as product or company branding in the built app. Product name: `Sepia`. Company: `Sepia Audio Project`. Domain id: `org.sepia.guitar`. `PLUG_UNIQUE_ID 'SpFX'`, `PLUG_MFR_ID 'SpAP'`.
- No code path sends a request to `api-guitar.soundshed.com` or `soundshed.com`.
- No factory preset references a `.nam`, IR or third-party plugin resource. No preset, cabinet or effect is named after an artist, a song, or another product's preset.
- Audio thread: no allocation, no locks. Non-finite input must leave output finite (`dsp/FiniteCheck.h`; never `std::isnan`). Verify in Release (`-ffast-math`).
- Stored enum orders are append-only: existing cabinet indices 0..4 keep their meaning and their exact response.
- New source files stay under 800 lines (`node tools/check-cpp-file-sizes.js`).
- New files are registered in `core/CMakeLists.txt` (`GUITARFX_CORE_SOURCES`) and tests in `core/tests/CMakeLists.txt`.
- After every DSP task: full `ctest` in `core/build` compared with the Task 0 baseline. No test that passed at baseline may fail.
- Commits: one per task step group, on branch `lofi-pack-design`, pushed.

## Review Focus

1. **Old presets with a cabinet index.** A preset saved before this change with `cabinet` 0..4 must render bit-identically. Test in Task 2.
2. **Tape with wow and flutter at zero.** A player who only wants saturation must get no added latency and no pitch movement. Test in Task 3.
3. **Bitcrusher at extreme settings.** 1 bit and the maximum downsample factor at 44.1 kHz and 192 kHz must stay finite and bounded by the input peak. Test in Task 4.
4. **Hiss with no guitar plugged in.** Tape hiss in a high-gain preset must not open a noise gate or be amplified into a roar: hiss is added after saturation, and its level at maximum is at or below -50 dBFS RMS. Test in Task 3.
5. **Upgrading from an upstream install on the same machine.** Sepia must not read or overwrite Soundshed Guitar's settings and preset store: its data directory derives from the new product name. Test in Task 1.

---

### Task 0: Baseline

**Files:** none changed. Result recorded in `docs/superpowers/plans/baseline-2026-11-10.md`.

- [ ] **Step 1:** Configure and build `core` in Release with tests, unmodified:
  `cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release -DGUITARFX_CORE_BUILD_TESTS=ON` then `cmake --build core/build -j 4`.
  In this environment `sqlite.org` is blocked; pass `-DFETCHCONTENT_SOURCE_DIR_GUITARFX_SQLITE_SRC=<dir with sqlite3.c 3.50.4>`. Run `npm ci` in `core/ui` first.
- [ ] **Step 2:** `cd core/build && ctest -j 4 --output-on-failure`. Record total, passed, and the name of every failing test with a one-line cause.
- [ ] **Step 3:** Commit the baseline record.

### Task 1: Rebrand and go offline

**Files:**
- Modify: `core/config/GuitarFXConfig.h` (brand strings, ids, URL, e-mail, startup preset title set to `""` until Task 5)
- Modify: `core/ui/ts/apiConfig.ts`, `core/ui/ts/toneSharingPanel/api.ts`, `core/ui/ts/tone3000Api.ts`, `core/ui/ts/practiceTool/tab/youtubeReference.ts`, `core/src/uiclient/ToneSharing.h`, `core/src/uiclient/Tone3000.h`, `core/src/uiclient/Jam.cpp`, `juce/source/PluginEditor.cpp`, `core/ui/index.html`, `core/ui/ui-components/panels/settings-help.html`
- Modify: `README.md` (what Sepia is, that it is a fork of Soundshed Guitar, upstream copyright notice kept)
- Delete: `core/ui/presets/factory/Preset-Pack-Vol.-1.soundshed.presets` (third-party captures we have no right to redistribute)
- Test: `core/tests/OfflineBrandingTests.cpp` (new)

**Interfaces:**
- Produces: `guitarfx::branding::kOnlineServicesEnabled` (`inline constexpr bool`, `false`) in `GuitarFXConfig.h`; the UI reads the same value through the existing settings/state push as `onlineServicesEnabled`.

- [ ] **Step 1: Failing test** `OfflineBrandingTests`:
  - `branding::kProduct == "Sepia"`, `kCompany == "Sepia Audio Project"`, `kDomain == "org.sepia.guitar"`.
  - `branding::kOnlineServicesEnabled == false`.
  - A source scan (test reads the repo tree from `GUITARFX_SOURCE_DIR` compile definition) finds no string `api-guitar.soundshed.com` under `core/src`, `core/ui/ts`, `juce/source`.
  - The resolved user data directory for a headless `PluginController` contains `Sepia` and not `Soundshed`.
- [ ] **Step 2:** Run it; expect failure on every check.
- [ ] **Step 3:** Change the brand macros. Add `kOnlineServicesEnabled`. Where a client builds a URL to the upstream API, return "unavailable" when the flag is false and remove the host literal. Hide the tone-sharing panel entry, the update check, and the proxy search option in the UI when `onlineServicesEnabled` is false. The Tone3000 official API (`https://www.tone3000.com/api/v1`) stays available with the user's own key; its proxy mode is removed.
- [ ] **Step 4:** Delete the bundled preset pack. Adjust tests that assumed it exists only if they fail for that reason, and list each in the commit message.
- [ ] **Step 5:** Full `ctest`; compare with baseline. UI: `cd core/ui && npm run build && npm test`.
- [ ] **Step 6:** Commit and push.

### Task 2: Five lo-fi cabinets

**Files:**
- Modify: `core/src/dsp/effects/SimpleCabVoicing.h` (enum `Cabinet`, `kCabinetLabels`, `kCabinets`, the `cabinet` parameter's max 4.0 → 9.0)
- Modify: `core/tests/SimpleCabEffectTests.cpp`
- Modify if they enumerate cabinets: `core/src/dsp/effects/SimpleCabMatch.h`, `core/ui/data/effect-presentation.json`

**Interfaces:**
- Produces: `simple_cab::Cabinet::{AmRadio=5, Telephone=6, Boombox=7, PracticeAmp=8, VhsCamcorder=9}`; labels `"AM Radio"`, `"Telephone"`, `"Boombox"`, `"Practice Amp"`, `"VHS"`.

Target responses with every control at noon, dynamic mic, relative to each voicing's own level at 1 kHz:

| Voicing | -3 dB low corner | -3 dB high corner | Character |
|---|---|---|---|
| AM Radio | 250–400 Hz | 2.2–3.2 kHz | peak of +3 dB or more between 1 and 2 kHz |
| Telephone | 300–450 Hz | 2.8–3.6 kHz | flat passband within ±3 dB, at least -24 dB at 100 Hz and at 8 kHz |
| Boombox | 90–140 Hz | 5–7 kHz | resonance of +4 dB or more between 120 and 220 Hz |
| Practice Amp | 150–220 Hz | 3.5–5 kHz | low-mid bump of +2 dB or more between 300 and 600 Hz |
| VHS | 80–130 Hz | 4–6 kHz | no resonance above +2 dB anywhere |

- [ ] **Step 1: Failing tests** in `SimpleCabEffectTests.cpp`:
  - `TestLegacyCabinetsUnchanged`: for cabinets 0..4, the magnitude response from the voicing model at 24 log-spaced frequencies 40 Hz–12 kHz equals values captured from the baseline build (hard-coded table, tolerance 1e-9 dB). Capture the table before editing the header.
  - `TestLoFiCabinetCorners`: each row of the table above.
  - `TestCabinetParameterAcceptsNewRange`: `SetParam("cabinet", 9)` then `GetParam` returns 9; 10 clamps to 9.
- [ ] **Step 2:** Run; `TestLegacyCabinetsUnchanged` passes, the other two fail.
- [ ] **Step 3:** Append five rows to `kCabinets` and the enum/labels. If the existing sections cannot reach a row's corners (the low cut is one second-order high-pass), add per-voicing optional extra sections that are bypassed for cabinets 0..4 rather than changing shared constants.
- [ ] **Step 4:** Run `SimpleCabEffectTests`, then full `ctest` against baseline.
- [ ] **Step 5:** Commit and push.

### Task 3: Tape effect

**Files:**
- Create: `core/src/dsp/effects/TapeSaturationEffect.h`
- Create: `core/tests/TapeSaturationEffectTests.cpp`
- Modify: `core/src/dsp/EffectGuids.h`, `core/src/dsp/effects/BuiltinEffects.h`, `core/CMakeLists.txt`, `core/tests/CMakeLists.txt`, `core/ui/ts/effectGuids.ts`, `core/protocol/effect-aliases.json`, `core/ui/data/effect-presentation.json` (regenerate with `node tools/gen-effect-presentation.mjs` if that is how it is produced), `docs/fx-library.md`

**Interfaces:**
- Consumes: `TapeTransport` (`Prepare`, `Reset`, `SetWow`, `SetFlutter`, `Next() -> float` delay offset in samples) and `TapeDropout` (`Prepare`, `Reset`, `SetAmount`, `Next() -> float` gain) from `dsp/effects/TapeTransport.h`.
- Produces: `EffectGuids::kTapeSaturation` (new UUID v4, generated once), alias `"tape_saturation"`, display name `"Tape"`, category `"lofi"` if the UI accepts a new category, otherwise `"modulation"`.
- Parameters (id, default, range): `drive` 0.3 [0,1]; `wow` 0.25 [0,1]; `flutter` 0.25 [0,1]; `hiss` 0.0 [0,1]; `dropout` 0.0 [0,1]; `tone` 0.6 [0,1] (low-pass 2.5 kHz at 0 to 16 kHz at 1, log); `mix` 1.0 [0,1].
- Factory presets (ids): `clean-reel` (default), `cassette`, `worn-cassette`, `vhs`, `saturation-only`.

Signal path per channel: input → modulated delay (only while `wow + flutter > 0`) → 2x oversampled soft saturation with drive-compensated output → tone low-pass → dropout gain → add hiss → mix. The delay's base length is the transport's peak excursion plus 1 ms, and never more than 5 ms. Engaging or releasing the delay crossfades over 20 ms.

- [ ] **Step 1: Failing tests** (`TapeSaturationEffectTests`):
  - registered under its UUID; alias resolves; parameter table matches the list above; every factory preset passes the shared factory-preset checks.
  - `wow=0, flutter=0, drive=0, hiss=0, dropout=0, tone=1`: output equals input within -80 dB and the impulse peak is at sample 0 (no latency).
  - `wow=1`: a 1 kHz sine's instantaneous frequency deviates by at least 0.5 % and at most 2.5 %; the impulse peak arrives no later than 5 ms.
  - `drive=1` on a 220 Hz sine at -12 dBFS: third harmonic at least -30 dB relative to the fundamental; output RMS within 3 dB of input RMS.
  - Aliasing: 5 kHz sine at -6 dBFS, `drive=1`, 48 kHz: every inharmonic component below 20 kHz is under -60 dB relative to the fundamental.
  - `hiss=1`, silent input: output RMS between -70 and -50 dBFS. `hiss=0`, silent input: output is exactly zero.
  - `dropout=1` on a steady sine over 10 s: gain dips occur and the minimum gain is not below 0.4.
  - No echo: with `wow=1`, an impulse's output energy after 10 ms is under -60 dB of its total.
  - NaN, +Inf and 1e30 input blocks: output finite, and the effect recovers to normal output within one second of clean input.
  - Mono path equals the left channel of the stereo path sample for sample.
- [ ] **Step 2:** Run; expect failure to compile, then failure on every check.
- [ ] **Step 3:** Implement `TapeSaturationEffect` and `RegisterTapeSaturationEffect()`; register in `BuiltinEffects.h`; add GUID and alias in the four listed places.
- [ ] **Step 4:** Run the new test, the UI checks (`node tools/check-effect-presentation.mjs`, `node tools/check-protocol.mjs`, `cd core/ui && npm test`), then full `ctest` against baseline. `FastMathNanTests` and `EffectEdgeCaseSweepTests` must cover the new type; add it to their lists if they enumerate effects by hand.
- [ ] **Step 5:** Commit and push.

### Task 4: Bitcrusher

**Files:**
- Create: `core/src/dsp/effects/BitcrusherEffect.h`
- Create: `core/tests/BitcrusherEffectTests.cpp`
- Modify: the same registration files as Task 3.

**Interfaces:**
- Produces: `EffectGuids::kBitcrusher`, alias `"bitcrusher"`, display name `"Bitcrusher"`, same category as Tape.
- Parameters: `bits` 12 [1,16] (continuous); `downsample` 1 [1,50] (hold factor, continuous, log taper); `mix` 1.0 [0,1].
- Factory presets: `subtle-grit` (default: bits 12, downsample 2), `8-bit`, `sampler`, `broken`.

- [ ] **Step 1: Failing tests**:
  - registration, parameter table, factory presets.
  - `bits=16, downsample=1`: output equals input within -90 dB.
  - `bits=4` on a full-scale ramp: output takes exactly 16 distinct levels.
  - `downsample=8`: output changes value once every 8 samples.
  - `mix=0`: output equals input exactly.
  - `bits=1, downsample=50` at 44.1 kHz and 192 kHz on -6 dBFS noise: finite, and peak output not above peak input + 0.5 dB.
  - NaN/Inf input: output finite.
  - Parameter changes mid-block produce no sample-to-sample step larger than the quantiser step plus the input's own step.
- [ ] **Step 2–5:** as in Task 3 (fail, implement, run all checks, commit and push).

### Task 5: Preset pack tooling and reference presets

**Files:**
- Create: `tools/sepia-presets/build_pack.py` — reads `tools/sepia-presets/presets/*.json` (compact form) and writes `core/ui/presets/factory/Sepia-Factory.soundshed.presets`
- Create: `tools/sepia-presets/presets/<group>.json` for the six groups
- Create: `tools/sepia-presets/README.md`
- Create: `core/tests/SepiaFactoryPackTests.cpp`
- Create: `core/tests/testdata/di/clean-chords.wav`, `palm-mute.wav`, `lead.wav` — synthesised by `tools/sepia-presets/make_di.py` (plucked-string model), so no third-party audio is added
- Modify: `core/config/GuitarFXConfig.h` (`GUITARFX_DEFAULT_STARTUP_PRESET_TITLE "Warm Clean Room"`), `core/tests/CMakeLists.txt`

**Interfaces:**
- Compact preset form: `{"name": str, "group": str, "gate": bool, "chain": [{"type": "<legacy alias>", "preset": "<effect factory preset id>"?, "params": {id: number}}]}`. The chain is serial; the tool emits `__input__`, nodes, edges and `__output__` in the archive's PresetV2 shape, with `category` set to the group name and deterministic ids `sepia-<slug>`.
- Archive extension stays `.soundshed.presets`: it is the file format the loader recognises, not branding shown to the player.
- Groups and final counts: `Clean & Indie` 12, `Lo-Fi` 8, `Shoegaze & Ambient` 8, `Blues & Classic Rock` 10, `Grunge & Punk` 10, `Metal` 12.

- [ ] **Step 1: Failing tests** (`SepiaFactoryPackTests`, loading the built archive through the same code path the app uses and rendering each preset through a headless graph, as `PresetLevelMeter` does):
  - the archive loads; it contains no `resources/` entries and no node with a resource reference.
  - every node type resolves in `EffectRegistry`; every parameter id exists on its effect and is within range.
  - names are unique; none contains a denied term (list in the test: artist and song names from the upstream pack, `Roomtone`, `Soundshed`).
  - for each of the three DI files: integrated loudness (`dsp/Loudness.h`) within ±1 dB of the pack target, and true peak below 0 dBFS.
  - presets in `Metal` and those named with "High Gain" measure at least 20 dB of compression on the palm-mute DI (output crest factor versus input); presets in `Clean & Indie` show THD under 5 % on the clean DI.
  - switching from every preset to the next in pack order through the mixer produces no sample step above 0.25 full scale.
  - every preset whose `gate` is true has a noise gate node first in the chain.
- [ ] **Step 2:** Write `build_pack.py` and `make_di.py`. Write two presets per group (12 total). Build the archive.
- [ ] **Step 3:** Run the test; tune parameters until all 12 pass. Print the measured table (loudness per DI, peak, THD or compression) into `tools/sepia-presets/measurements.md`.
- [ ] **Step 4:** Full `ctest` against baseline. Commit and push.

### Task 6: Complete the pack

**Files:** `tools/sepia-presets/presets/*.json`, rebuilt archive, `measurements.md`.

- [ ] **Step 1:** Add the remaining 48 presets, group by group. Each group must differ audibly within itself: no two presets in a group share the same amp voice, gain within 0.1, and cabinet at once.
- [ ] **Step 2:** `SepiaFactoryPackTests` passes for all 60; update `measurements.md`.
- [ ] **Step 3:** Full `ctest` against baseline. Commit and push.

### Task 7: Linux app build and Windows CI

**Files:**
- Modify: `build_linux.sh`, `build_windows.bat`, `.github/workflows/build-windows.yml`, `.github/workflows/build-linux.yml` — product and artefact names; remove steps that need upstream secrets (signing, bundled API keys), leaving unsigned artefacts
- Modify: `juce/CMakeLists.txt` if target names embed the old product name

- [ ] **Step 1:** Build the Linux Standalone and VST3 here (`./build_linux.sh --no-lv2 --no-zip`), launch the Standalone headless under `xvfb-run` for 10 s, confirm it starts, loads the factory pack and exits cleanly. Record what was and was not verified.
- [ ] **Step 2:** Push; run the Windows workflow on the branch (`workflow_dispatch`); fix failures until it produces a Standalone and VST3 artefact.
- [ ] **Step 3:** Write `docs/sepia-testing.md`: what the tester checks on a real Windows machine and interface (offline install, preset list, switching clicks, dropouts, latency and CPU at buffer 64/128/256), and how to report it.
- [ ] **Step 4:** Commit and push.

## Out of scope

Vinyl crackle, new amp models, UI redesign, macOS/AAX/mobile builds, online preset sharing, artist search.
