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
`StaysInWebView` and `isScalexUrl` cases). The optional audio preview was not built. Arpeggios, the third mode, followed on 2026-11-06 in
`core/src/music/Arpeggios.h` on both UIs (see Arpeggio notes). `ScaleGuideTests --dump <tuning> <key 0-11> <chord>` prints the shapes found, for tuning
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

One neck view, three modes, sharing the key and tuning.

**Scales**
- Key (12 chips, enharmonic pairs shown as `C♯/D♭`), scale (grouped list), tuning.
- Every scale note on the neck from the nut to fret 24, labelled with its note name, interval
  (R ♭3 4 …) or scale degree; the root stands out. No labels is an option too.
- Positions: WHOLE NECK, then the boxes 1..n. A chosen box dims the rest of the neck (rather than
  hiding it), so the player sees where the box sits.
- Footer: a chip per scale tone (name + interval), the step pattern in semitones (2 1 2 2 1 2 2), and for
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

**Arpeggios**
- Root + quality, then the runs of that chord across the neck: one per chord tone it can start on,
  in each octave that fits, sorted up the neck and named (Root position, From the ♭3). Position
  picks a run; ALL shows every chord tone.
- Style: Sweep (one pick stroke across the strings, a hammer-on and pull-off where a string has
  two notes), 2 per string (a note pair on each string, the hand shifting up the neck), or Box
  (every chord tone once in one hand position). Strings, counted from the highest, set how wide a
  sweep or 2 per string runs; a box ignores it.
- The chosen run lies on the neck as a path through its notes, labelled Order or Fingers if asked,
  with the rest dimmed. Under the neck, a tab card: up the strings and back, pick strokes above,
  intervals below.
- Or a progression in the key, the same list as Chords (built-ins and the player's own; the choice
  is shared between the two tabs). The neck shows the chord at the current step and the tab card
  writes the whole progression out, a bar for each chord, each chord in the shape nearest the one
  before. A bar is a button that steps to its chord. Slides, in the card's heading, makes each
  chord take a shape that can be slid into from the last note of the chord before, and marks the
  slide (`sl.`). Play sounds the whole progression, the tab lighting each note and the neck
  stepping to each chord as it comes.

**All three**
- A chosen position can be heard: Play sends it through the current preset, and a dot on the neck
  plays that one note.
- NOTES / INTERVALS / DEGREES / none labels; left-handed (mirrored) view; fret range (0-12,
  0-24, or follow the chosen position); capo (shifts the nut, labels stay concert pitch).
- LISTEN (phase 4): the neck marks each note you hit, coloured by whether it fits the scale; in
  Chords mode, strumming the current chord steps the progression.

Out of scope: backing tracks (Jam already has search, favourites and the floating player; the
guide can link to it), tab/notation beyond the arpeggio runs, a chord-name finder from tapped notes (a later idea).

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

- **Neck size:** one drawing unit is one CSS pixel once the space is 1000 px or wider, so dots and
  labels stay UI-sized and follow the app's zoom while the frets spread to fill the width. Below
  that the whole drawing scales down. Nano Q draws the same way.
- **Web:** an SVG component under `core/ui/ts/scaleGuide/` in the Jam → Scales section,
  replacing the iframe. Horizontal neck (string 1 on top in the usual tab-like orientation, or
  low string at the bottom: one setting), fret spacing a blend of real 12-TET and even spacing so
  fret 24 still holds a label; inlays at 3 5 7 9 15 17 19 21, doubles at 12 and 24. Colours from
  theme tokens (root uses the accent token), so light and dark themes both work. Compact layout
  stacks the chord box under the neck and scrolls the neck horizontally.
- **Web layout:** three slim rows above the neck, so the neck starts as high as it can. The tabs,
  with Play and the instrument button at their end (LISTEN has no switch: it is on whenever the
  guide shows); the button reads what is set
  ("Standard E · Capo 2 · Left-handed") and opens Tuning, Capo and Left-handed, which are set once
  and then left alone. Then what is showing: Key or Root with the scale, chord or progression,
  and on that row an arpeggio's Style and Strings and a progression's steps. Then, on the neck,
  Position or Shape at the left and what the dots say at the right. The progression editor is a
  card of its own that shows only while editing. Play's volume is a short slider beside it, and
  "Listen marks the whole neck" is in the instrument menu.
- **Nano Q layout:** the same rows, drawn with its own widgets. The instrument button opens a
  three-column menu (tunings, capo, then Left-handed and "Listen marks the whole neck"), Play's
  volume is a picker of fixed steps (100 to 10%) rather than a slider, and an arpeggio's Style and
  Strings and a progression's steps each take a row of their own. It has no progression editor,
  as before.
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
  last few notes). The tuner runs whenever the guide is on screen: LISTEN is always on there,
  with no switch and no "Listening" indicator, and stops when the guide is left.
- Chords: a 12-bin chroma of the dry input, a new tap beside the tuner (pitch-class folding of a
  spectrum is cheap and off the audio thread's critical path). Rule: with overtones removed, the
  chord's tones hold a set share of the energy and each tone but a plain 5th is present; held
  ~130 ms = clean; the next hit needs a fresh strum (a level jump) so repeated chords step once
  each. Thresholds are measured on labelled real strums: see LISTEN notes below.

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

## Arpeggio notes

`Arpeggios.h` searches; `ScaleGuideView` turns the result into `view.arpeggio`, so Nano Q and the
web do no music theory.

- A sweep or 2 per string is a dynamic programme over the rising chord pitches, assigning each to
  the next string inside a fret window. An extra note on a string costs 1 on the top string, 3 on
  the first and 4.5 inside, which is what puts a sweep's hammer-on at the ends: Am reads
  `12 15/14/14/13/12 17`, fingers 1 4 3 3 2 1 4.
- Fingering is a second one: a move wants a finger gap equal to its fret gap, rolls stay on one
  finger, and a shift while changing string costs half.
- Sweeps run root to root. Chords past the octave drop the octave root's rung (R 3 5 7 9) and may
  end on the 9th. Where no octave ending fits (DADGAD's top strings), any ending that sounded
  every tone will do.
- 2 per string needs at least a seventh chord across five strings: a triad would climb 17 frets
  or more, so `ArpeggioStringsWithRuns` offers only the counts with runs and the view plays the
  closest. The saved count stays, for the next style or chord.
- Re-entrant tunings have no sweep or 2 per string; the view says so and shows the chord tones.
  Boxes work anywhere.
- `runs` carry `notes` lowest first and `steps` for the way up and back, the top note once.
- The golden shapes and the rules over every tuning are in `core/tests/ArpeggioTests.cpp`, and
  `ArpeggioTests --dump <tuning> <key> <chord> <style> <strings>` prints the runs found.
- A progression (`ArpeggioProgression.h`) searches each distinct chord once, in the style and on
  the most strings it has runs for, up to the count asked. One chord is the anchor: `position`
  picks its run and `anchorStep` says which chord that is (the one showing when the player last
  picked a position). The rest follow round the progression from it, each taking the run with the
  least shift from the one before: twice the change in starting fret, four times the change in
  starting string, plus the change in highest fret. Stepping changes `step` only, so the tab
  never moves under the player. `view.position` is the showing chord's run; the anchor's comes
  back as `progression.anchorStep` and `anchorPosition`, and that is what the UI keeps.
- With `arpeggioSlides` a run that starts on the last run's starting string at another fret (both
  fretted) is preferred over a nearer one, and its first step's stroke is `slide`. Every run goes
  up and back, so it ends where it began and the slide is along that one string. Where no such
  run exists (an open string, a box that starts on another string) the chord is picked as usual.
- `view.progressionRun` is the whole piece: `notes` numbered through it, `steps`, and `bars` (one
  per chord: `from`, `count`, the run's name and frets). A chord with no run keeps an empty bar.
  Play uses it whenever it is there, whatever the position, and each played note carries its
  `step` of the tab and its `chord`, which is how the UI follows.
- Played, each chord starts on a bar line of the metronome's meter (`MetronomeService::BarSeconds`):
  a run of 13 eighths in 4/4 takes two bars, its last note held for the rest of the second. A
  `play` with `bar` sounds that one chord's bar alone, from its first note, which is what a click
  on a bar of the tab does.
- A slide is one stroke: the renderer does not strike the note slid to, it lets the note before it
  last through both and moves its read through the string's samples faster or slower (the same
  moving read the vibrato uses), eased over 60 ms plus 12 ms a fret, at most 140 ms, centred on
  the beat. A note marked `slide` with no note before it on its string is struck like a hammer-on.
- The web draws the tab as one diagram that runs on to new lines at the card's width
  (`runTab.ts`, `TabFlow`): a bar line after each chord, a heavier one at the end, the chord's
  name over its first note and the string names at the start of each line. A bar is kept on one
  line where a line can hold it and split where it cannot. It is laid out again when the width
  changes.
- `ArpeggioTests --progression <tuning> <key> <progression> <style> <slides 0|1>` prints a
  progression bar by bar.
- Nano Q has the same (`ScaleGuidePage.cpp`, `ProgressionTabView` in `ScaleGuideDraw.cpp`): the
  progression picker in Arpeggios, the flowing tab with bar lines and slides, Slides in the card's
  heading, and a bar that steps to its chord and plays. `uiclient::scaleguide::ViewRequest` sends
  the progression, `anchorStep` and `arpeggioSlides`. As a progression plays, the neck steps
  through its chords (see Play notes).

## Play notes

- A chosen scale position gets a `run` in the view model like an arpeggio's (`ScalePositionRun`):
  its notes rising and falling, alternate-picked, a pitch two strings share played once. That is
  what the tab card and Play read, so all three stay the same for both modes. A chord plays its
  shape strummed once. All has nothing to play, so Play is disabled there.
- Other ways in: a scale's summary (badge, name and notes) plays `ScaleExampleNotes`, one octave
  from the lowest root in the chosen position's run to the next root and back, taken from the first
  position when All is showing. A chord box, or a progression's, strums its own frets
  (`StrummedChord`, action `playChord`), so the chord heard is the box clicked even before the
  view catches up. The tab card has a Play of its own, the same as the one beside the tabs.
- `music/ScalePlayback.h` renders the notes on the message thread to a mono signal, eighth notes at
  the metronome's tempo (held to 60-200 BPM), peaking at 0.35 like a DI track.
  `DemoPreviewService::StartPreviewFromSamples` then mixes it into the input, so it goes through the
  preset as a demo clip does; the audio thread only copies.
- In time with the click: while the metronome is clicking, Play and a scale's example wait for its
  next bar line. `StartPreviewFromSamples` takes the metronome's `TransportClock`, reads where in
  the bar the next block starts while it holds the DSP lock, and has the audio thread skip that
  many frames before the clip's first, so the start is to the sample. The wait goes back to the
  UI as `delayMs` in `scaleGuidePlayback`, and the following waits with it. A clicked note and a
  strummed chord box do not wait. With the click off, or in a DAW (where the host owns the tempo
  and the clock is not running), Play starts at once: it does not follow the host's transport.
  What was sounding stops at once under a start that waits, without the usual fade.
- The voice is a Karplus-Strong string: a noise burst low-passed and combed by the pick position, a
  loop that loses the most top a pass can, an all-pass for the fraction of a sample, a comb for the
  pickup, then the pickup and cable as a two-pole and a one-pole low-pass. It is within 4 cents from
  E2 to E6 at 44.1, 48 and 96 kHz. Hammer-ons and pull-offs start softer and rounder. A render never
  varies, so a test can pin it.
- A note left held (a run's last, a chord's strings, a clicked note) rings for 3.5 s, a chord 4.5 s,
  on the string's own decay (5 to 13 s to -60 dB, the loop gain making up what its filter takes
  from the fundamental), and fades over its last 1.2 s rather than being stopped. From 0.55 s in a
  vibrato grows over 1.3 s to a bend of about 16 cents, sharp only as a rocked finger is, near 5 Hz
  with its rate and depth wandering towards a new random mark every 0.2 s, seeded per note. A
  chord's fretted strings get a third of that each, on their own wander; an open string gets none.
  It is a moving Hermite read of the string's samples, outside the loop, so the loop's tuning and
  losses are untouched. `ScalePlaybackTests --wav <file> held` writes these to listen to.
- Its first voicing was harsh: -5 dB of its power above 2.8 kHz, where the demo DI take has -34 dB
  and the two riff recordings -45 dB. It is now -37 dB, and its octave bands from 63 Hz to 5.6 kHz
  sit between the DI's and the riffs'. `ScalePlaybackTests --tone` prints the bands side by side,
  `--wav <file>` writes the sample it measures, and the test holds the voice between the DI's top
  plus 1 dB and 11 dB under it.
- Reports use the preview id `scale-guide`, kept out of the demo clip's notifications on the web. A
  clicked note is not announced at all, and fades over whatever it interrupts, since a hard cut
  through a high-gain preset is a loud click.
- Both UIs light each note from `scaleGuidePlayback`'s timings, on the neck where it is fretted and
  in the tab, and show a progression's next chord 80 ms before its first note. The web sets timers
  (`playback.ts`). Nano Q keeps the notes and the time they came in the client state
  (`DemoState::scaleGuide`), and each tick asks `uiclient::scaleguide::PlaybackAt` what sounds now
  (`Shell::SyncScaleGuidePlayback`): a changed note repaints the neck and the tab, a changed chord
  rebuilds the page. Neither scrolls the tab to the bar playing.

## LISTEN notes (phase 4)

- Readings come about every 43 ms. A note shows once it reads the same twice, goes after four
  silent readings, and the last two stay as a fading trail (`NoteFollower` in listen.ts).
- Chord match, per strictness (Easy / Normal / Strict): the chord's tones hold 60 / 70 / 80 % of
  the chroma and each tone bar a plain 5th reads 3 / 5 / 7 %. Three clean readings in a row
  (~130 ms) count; the next count needs a 3 dB jump in level after a lockout of eight readings
  (~340 ms), and the level history restarts at each count so a ringing strum can't re-arm on the
  quiet before it.
- The chroma leaves out peaks that are a strong lower peak's 3rd, 5th, 6th or 7th harmonic. They
  land on notes nobody fretted, and with them counted a well played chord's own tones typically
  held under 80 %.
- Measured on GuitarSet (six players, pickup signal, annotated chords; 3,889 well played strums),
  fitting on three players and checking on the other three. On the check players, correct chords
  pass 92 / 86 / 75 %; asked for the parallel major or minor instead, the chord is accepted 3.8 /
  1.2 / 0.6 % of the time, and for the relative major or minor 10.6 / 6.9 / 3.7 %. The relative
  pair is the hardest: the two chords share two of their three notes.
- Not measured: LISTEN's single-note following, electric guitars through a DI, and a player
  following a progression from start to finish in the app.

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
7. **Arpeggios.** The search, the third mode on both UIs, the run path and tab card.

## Open questions

- Neck orientation default: low string at the bottom (like looking down at the guitar) or at the
  top (like tab)?
- Should progressions and custom tunings sync to the Soundshed account, or stay per machine?
- Audio preview: worth it in phase 6, or leave sound to the instrument?
- Mandolin/banjo: not asked for; the model allows them (4-5 strings, banjo re-entrant) if wanted.
