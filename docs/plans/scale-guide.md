# Scale Guide: our own scale and chord diagrams

Status: phase 1 (the engine) built 2026-10-30, in `core/src/music/` (MusicTheory, MusicCatalog,
Fretboard + ChordShapes, ScaleGuideView) with `core/tests/ScaleGuideTests.cpp`. Phase 2 (the web
Scales and single-chord view, `core/ui/ts/scaleGuide/`, answered by `scaleGuide` in
`controller/ScaleGuideMessages.cpp`) replaced the web iframe on 2026-10-30; Nano Q still shows
scalex.soundshed.com until phase 5. Phase 3 (progressions: the Play picker, steps with
arrow keys, the voice-led chord-box strip, and the player's own progressions in the
`scaleGuide.progressions` app setting, built-ins edited as copies) followed the same day, then
phase 4 (LISTEN: `tuner {action: "listen"}` reads the input without turning the tuner on, so a
muting tuner never silences the player; Scales mode follows the played note, Chords mode matches
a chroma vector from `dsp/ChromaAnalyzer.h` and a clean strum steps a progression), then phase 5
(Nano Q: `nanoq/src/ui/jam/ScaleGuidePage.cpp` and `ScaleGuideDraw.cpp` draw the same view model,
with the web UI's settings and LISTEN rules ported to `core/src/uiclient/ScaleGuide.h`; the Scales
web view is gone from Nano Q), and phase 6 (the scalex links removed: `kScalesUrl`, the
`StaysInWebView` and `isScalexUrl` cases). The optional audio preview was not built. `ScaleGuideTests --dump <tuning> <key 0-11> <chord>` prints the shapes found, for tuning
the search's scoring.

## Why

Jam → Scales embeds scalex.soundshed.com: an iframe in the web UI
(`core/ui/ui-components/panels/jam-panel.html`, `core/ui/index.html`) and, in Nano Q, a WebView2
of its own (`nanoq/src/ui/jam/JamPage.cpp`, `uiclient/Jam.h` `kScalesUrl`), which falls back to the
system browser off Windows or without the WebView2 Runtime. It needs a network connection, it
can't see the instrument (no live notes), it carries the site's own analytics and banner, and the
site draws server-side PNGs from a C# codebase we'd otherwise have to keep alive.

We want the scale and chord views drawn by the app itself, the same in the web UI and Nano Q, for
4- to 8-string instruments: guitar 6/7/8, bass 4/5/6, and ukulele (re-entrant).

## What it shows

One neck view, two modes, sharing the key and tuning.

**Scales**
- Key (12 chips, enharmonic pairs shown as `C♯/D♭`), scale (grouped list), tuning.
- Every scale note on the neck from the nut to fret 24, labelled with its note name, interval
  (R ♭3 4 …) or scale degree; the root stands out. No labels is an option too.
- Positions: WHOLE NECK, then the boxes 1..n. A chosen box dims the rest of the neck (rather than
  hiding it), so the player sees where the box sits.
- Footer: a chip per scale tone (name + interval), the step pattern (W H W W H W W), and for
  seven-note scales the diatonic chords (Em F♯dim G Am Bm C D) with their roman numerals.
  Tapping one opens it in Chords mode.

**Chords**
- Root + quality, or a progression in the key.
- The chord's tones across the neck (ALL TONES), then playable shapes 1..n, each with muted (×)
  and open (o) strings, finger numbers and barres.
- A vertical chord box (the 5-fret grid with the starting fret number) beside the neck for the
  selected shape, and as a strip of boxes for a progression. This is what fits a phone column
  and what people print.
- Footer: chord name and quality, formula (R 3 5 ♭7), the next chord in a progression.
- Progressions: built-ins written in roman numerals (I V vi IV, 12-bar blues, ii V I, i ♭VII ♭VI
  V …), plus the player's own (name + up to 16 chords as degree + quality). STEPS pills with
  prev/next/restart; arrow keys step. Stepping keeps the next shape near the last one.

**Both**
- NOTES / INTERVALS / DEGREES / none labels; left-handed (mirrored) view; fret range (0-12,
  0-24, or follow the chosen position); capo (shifts the nut, labels stay concert pitch).
- LISTEN (phase 4): the neck marks each note you hit, coloured by whether it fits the scale; in
  Chords mode, strumming the current chord steps the progression.

Out of scope: backing tracks (Jam already has search, favourites and the floating player; the
guide can link to it), tab/notation, a chord-name finder from tapped notes (a later idea).

## Instruments and tunings

A tuning is the open strings as MIDI notes in **physical order** (low string to high string as
they lie on the neck, not sorted by pitch), a name, and a flat-naming flag. Pitch order is derived
where an algorithm needs it, which is what makes re-entrant tunings work.

| Instrument | Strings | Tunings |
|---|---|---|
| Guitar | 6 | Standard E, Drop D, E♭, D, Drop C, C, Drop B, DADGAD, Open G, Open D, Open E, Open C |
| Guitar | 7 | B standard, Drop A |
| Guitar | 8 | F♯ standard, Drop E |
| Bass | 4 / 5 / 6 | E A D G; B E A D G; B E A D G C; Drop D |
| Ukulele | 4 | Soprano/concert/tenor G C E A (high G, re-entrant), Low G, Baritone D G B E |

Plus a custom tuning: 4-8 strings, a note per string, saved by name. Frets 0-24 (up to 27 for
extended-range necks if asked for).

## Theory engine

C++, in `core/src/music/` next to `dsp/MusicalScale.h` (which stays as the small audio-thread
table the Harmonizer uses). Pure functions on the UI side (never the audio thread), unit-tested
in core/tests.

- **Catalogue.** Scales, modes, chord qualities and progressions as interval formulas
  (`1 2 b3 4 5 b6 b7`, `ii7 V7 Imaj7`), parsed once. Starting set: the major/minor family,
  pentatonics and blues, harmonic/melodic minor and their modes, the common exotic scales
  (Phrygian Dominant, Hungarian Minor, Double Harmonic, Neapolitan, Lydian Dominant, Altered,
  Bebop, Hirajoshi, Kumoi, Whole Tone, both diminished), arpeggios of every chord quality.
  Chord qualities: maj, m, 5, 6, m6, 7, maj7, m7, mMaj7, m7♭5, dim, dim7, aug, aug7, sus2, sus4,
  add9, 9, maj9, m9, 7sus4.
- **Spelling.** Each degree gets the letter `root + degree - 1` and the accidental that makes the
  pitch; for a key with two spellings both are tried and the one with fewer accidentals wins.
  A progression picks its key spelling over every chord tone it contains; chord roots are spelled
  from the key's letters (♭VII in E♭ is D♭).
- **Scale boxes.** Generated, so every tuning gets them:
  - Monotonic tunings: n notes a string (2 for pentatonics, 3 for seven-note scales, optionally
    3 for pentatonics), starting from each degree on the lowest string; a note the next string
    can't reach stays on this one (drop tunings). Blues-type scales use their parent pentatonic's
    box plus the added note within its span.
  - Any tuning, including re-entrant: fret-window positions (a 4-5 fret span per position, one
    per scale degree on the lowest-pitched string). Ukulele uses these.
  - Scales the box rules don't fit (whole tone, diminished, chromatic, arpeggios) get the
    fret-window positions too.
- **Chord shapes.** Searched, not tabled. Per fret window, each string is muted, open (low
  windows only) or a chord tone inside the window. Rules: the lowest-**pitched** sounding note is
  the root (a slash/inversion option relaxes this), no muted string between sounding ones, every
  tone present except a plain 5th in 4+ note chords, at most 4 fingers with a barre as one. Score
  on string count, stretch, fingers, and open strings high up the neck; keep the best per window
  and up to six spread along the neck. Per instrument:
  - Extended-range guitar must be allowed to start on the low strings (a 7/8-string shape that
    only ever uses the top six is a bug, not a choice).
  - Bass gets its own shapes: root-5, root-5-8, root-3-5 and the 7th arpeggio, 2-3 strings.
  - Ukulele: all four strings sound by default; the bass note is whichever string is lowest in
    pitch.
  - Finger numbers come from the same search (barre = finger 1 across the span).
  - Name the five shapes C A G E D on standard 6-string tuning only.
- **Voice leading.** In a progression, choose the shape nearest the previous one (average fret
  and lowest string).
- **Output: one view model**, the only thing a renderer sees:
  `{ strings: [{openMidi, name}], fretFrom, fretTo, capo, leftHanded, dots: [{string, fret, pc,
  name, interval, degree, role: root|tone|added, dim}], marks: [{string, kind: mute|open}],
  barres: [{fret, fromString, toString}], fingers: [{string, finger}], positions: [...],
  footer: {...} }`. Chord boxes are the same model cut to 5 frets.

The web UI asks for a model with a message (`scaleGuideModel` → `scaleGuideModelResult`,
following the existing request/response handlers in `core/ui/ts/messages/`); Nano Q calls the
engine directly. The search is cheap (a few ms worst case for 8 strings, cached per tuning +
root + quality), so a round trip per click is fine.

Why one C++ engine rather than TS and C++ twins (the custom-layout precedent): the shape search
and spelling rules are the part most likely to drift, and a drift here shows as a wrong chord.
Rendering is the only thing written twice. Golden test vectors (key, scale/chord, tuning →
expected dots and shapes) live in core/tests and are what both renderers' tests load too.

## Rendering

- **Web:** an SVG component under `core/ui/ts/scaleGuide/` in the Jam → Scales section,
  replacing the iframe. Horizontal neck (string 1 on top in the usual tab-like orientation, or
  low string at the bottom: one setting), fret spacing a blend of real 12-TET and even spacing so
  fret 24 still holds a label; inlays at 3 5 7 9 15 17 19 21, doubles at 12 and 24. Colours from
  theme tokens (root uses the accent token), so light and dark themes both work. Compact layout
  stacks the chord box under the neck and scrolls the neck horizontally.
- **Nano Q:** a drawn element in `nanoq/src/ui/` (alongside `EqPlot.cpp` / `ResponsePlot`),
  painting the same model with the artist canvas, plus the page controls with the existing
  `DialogChrome.h` widgets and `WithSectionNav`. This removes the Scales WebView from Nano Q, so
  Scales works on macOS/Linux/iOS builds too.
- Accessibility: each dot is a labelled element ("E, root, string 6 fret 0"); the shape list and
  progression steps are focusable (Nano Q: Focusable/KeyRouter).

## State

App settings (shared across standalone, plugin instances and Nano Q, like `jam.*`), never host
state: `scaleGuide.view` (mode, key, scale, chord, tuning, labels, position, handedness, capo),
`scaleGuide.progressions` and `scaleGuide.tunings` (the player's own).

## LISTEN

- Scales: the tuner feed (`tunerUpdate`) gives the note; show its pitch class wherever it falls
  on the neck, debounce (two matching reads before moving, a short release, a fading trail of the
  last few notes). The tuner runs while the guide is open with LISTEN on.
- Chords: a 12-bin chroma of the dry input, a new tap beside the tuner (pitch-class folding of a
  spectrum is cheap and off the audio thread's critical path). Rule: the chord's tones hold a set
  share of the energy, each tone but a plain 5th is present, and no rival quality on the same root
  fits better; held ~200 ms = clean; the next hit needs a fresh strum (a level jump) so repeated
  chords step once each. Tune thresholds on the demo DI riffs (see the PitchTracker work).

## Clean-up once it ships

Remove `kScalesUrl` and the scalex entries in `uiclient/Jam.cpp` `StaysInWebView`,
`juce/source/PluginEditor.cpp` `isScalexUrl`, the iframe in both HTML copies, `.jam-scales-frame`
in `jam.css`, and Nano Q's `mScalesView`. The public scalex.soundshed.com site can stay as it is.

## Porting notes from the Scalex source

`C:\Work\GIT\soundshed\scalex` (MIT, ours) has the scale/mode/chord interval tables, tunings and
a WebAudio scale player worth reusing as data and ideas; draw natively rather than port its
SkiaSharp renderer. Fix on the way in: Double Harmonic Major is stored as the whole-half
diminished set (`Scales.cs:184`); Aug9 uses a ♭2 instead of a 9th (`Chords.cs:152`); Open C is an
octave low (`GuitarTuning.cs:112`). Its chord shapes are a greedy first-fit, its root isn't
highlighted with note colours on, and it has no boxes, barres, fingering or left-handed view, so
those are new work here.

## Engine notes (phase 1)

- Shape score: 3 per sounding string (up to six), minus 0.6 × stretch², 0.3 per finger, 2 per
  open string inside a would-be barre when there are more than four fretted notes (F as
  1 0 3 2 1 1), and 1.5 per open string when the shape reaches fret 5 (C7 as x 3 5 3 5 0).
- Fingers count up from the lowest fretted note (a lone fretted note is finger 1, so ukulele C
  reads 0 0 0 1 where a chart writes 3). Barres: the lowest fret first, then small barres across
  neighbouring strings at one fret (the A-shape's 3 3 3).
- Spelling a lone chord follows the fewest-accidentals rule strictly, so B♭m7♭5 reads A♯m7♭5
  (F♭ counts as an accidental). B♭m7, D♭ and the like come out as charts write them.
- Search time: about 1.3 ms a chord in a Debug build, worst on 8 strings.

## LISTEN notes (phase 4)

- Readings come about every 43 ms. A note shows once it reads the same twice, goes after four
  silent readings, and the last two stay as a fading trail (`NoteFollower` in listen.ts).
- Chord match, per strictness (Easy / Normal / Strict): the chord's tones hold 55 / 65 / 75 % of
  the chroma, each tone bar a plain 5th reads 10 / 15 / 25 %, and no rival chord on the root that
  is not a superset fits better by more than 3 %. Five clean readings in a row (~200 ms) count;
  the next count needs a 4 dB jump in level, so a repeated chord steps once per strum.
- Tested on synthetic chords (TunerEngineTests, scaleGuideListen.test.ts) and live on the demo DI
  riffs, which are single-note riffs: a clean strum of a real chord on a real guitar is not yet
  checked, so the thresholds may want tuning against recordings (see the PitchTracker notes).

## Phases

1. **Engine + golden tests.** Catalogue, spelling, boxes (both kinds), chord search with the
   per-instrument rules, finger/barre assignment, voice leading, the view model. Tests over every
   preset tuning: each scale's dots match its pitch classes; every shape sounds every required
   tone, has the root lowest, ≤ 4 fingers; 7/8-string shapes use the low strings; ukulele shapes
   use all four strings.
2. **Web Scales + Chords (single chord).** SVG neck, chord box, controls, labels, positions,
   left-handed, capo, settings; replaces the iframe behind the existing Scales feature switch.
3. **Progressions.** Built-ins, the editor, steps, voice-led stepping, chord-box strip.
4. **LISTEN.** Scales from the tuner feed; then the chroma tap and chord matching.
5. **Nano Q page.** The drawn neck and box, controls, then LISTEN. Drop the Scales WebView.
6. **Clean-up** above; optional audio preview (play the scale up/down or strum the shape through
   the app's own output).

## Open questions

- Neck orientation default: low string at the bottom (like looking down at the guitar) or at the
  top (like tab)?
- Should progressions and custom tunings sync to the Soundshed account, or stay per machine?
- Audio preview: worth it in phase 6, or leave sound to the instrument?
- Mandolin/banjo: not asked for; the model allows them (4-5 strings, banjo re-entrant) if wanted.
