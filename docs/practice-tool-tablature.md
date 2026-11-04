# Practice Tool: tablature and sync

The Practice Tool shows a score — Guitar Pro 3–8, MusicXML (plain or `.mxl`),
Capella or alphaTex — under the waveform, with a cursor that follows the
reference: the backing track the tool is playing, or a YouTube video. The user
lines the two up by marking where bars start ("sync points"); the cursor then
follows the reference through tempo changes, rubato and repeats.

Rendering is [alphaTab](https://alphatab.net) (MPL-2.0, `@coderline/alphatab`,
pinned in `core/ui/package.json`). Its rendered "alphaTab.net" attribution must
stay visible: nothing may hide, cover or crop it without the maintainers'
permission (https://alphatab.net/docs/attribution).

## Where the code is

| Piece | File |
| ----- | ---- |
| Section markup | `core/ui/ui-components/panels/jam-panel.html` (`#practice-tool-tab-section`) |
| Styles | `core/ui/css/practice-tool-tab.css` |
| Facade | `core/ui/ts/practiceTool/tab.ts` |
| DOM: toolbar, sync bar, drop zone, T key | `ts/practiceTool/tab/panel.ts` |
| Open score and reference, and what changing either does | `ts/practiceTool/tab/session.ts` |
| alphaTab instance, external-media handler, cursor, sync flags | `ts/practiceTool/tab/tabView.ts` |
| Open binding, per-reference autosave | `ts/practiceTool/tab/tabState.ts` |
| Sync point rules (pure, tested) | `ts/practiceTool/tab/syncPoints.ts` |
| Backing track as reference | `ts/practiceTool/tab/reference.ts`, over `ts/practiceTool/playhead.ts` |
| YouTube video as reference | `ts/practiceTool/tab/youtubeReference.ts`, `youtubeUrl.ts` |
| Loading alphaTab on demand | `ts/practiceTool/tab/alphaTabLoader.ts` |
| Score files in the document store | `core/src/controller/PracticeTabMessages.{h,cpp}` |
| YouTube relay page | private repo, `api/src/routes/corsproxy.ts` (`/v1/embed/youtube-sync`) |
| Tests | `core/ui/tests/practiceToolTab.test.ts`, `core/tests/PracticeTabMessagesTests.cpp`, private repo `api/test/youtube-relay.test.ts` |

## How it fits together

**Loading alphaTab.** It is ~1.2 MB, and most sessions never open a tab, so it is
not on the page. `scripts/copy-vendor.js` copies its browser build, its licence
and the Bravura web fonts to `dist/vendor/alphatab/`; the first score opened
injects the script (a classic script defining the `alphaTab` global — the UI has
no bundler to resolve the package's ES module build). The editor's launch
prefetch (`juce/source/editor/UiResourcePrefetch.cpp`) skips `dist/vendor/` for
the same reason. No soundfont ships: alphaTab never makes sound here.

**Score files.** The engine keeps them, never parses them. A browsed or dropped
file goes to the document store as type `practice-tab`, id = its content hash
(the same file opened twice is one row), and comes back to the UI as
`practiceToolTabLoaded {id, fileName, data}`. The UI keeps only the id, so a
project recalls its tab wherever the original file went. Files over 16 MB are
refused. Messages are in `docs/user-interface.md`.

**The cursor.** alphaTab runs in `PlayerMode.EnabledExternalMedia`. The tab view
calls `updatePosition(referenceTimeMs)` every animation frame while the
reference plays (skipping the work while the panel is hidden) and once on each
pause, seek or load, and mirrors play/pause into alphaTab's own state, which
animates its beat cursor. alphaTab maps reference time onto the score through
the sync points. Its playback rate and volume stay at 1: Speed is the backing
track's own fader (the reference reports media time, which Speed doesn't
change), and a video has YouTube's controls.

**Clicks on the score.** alphaTab asks the reference to seek when a beat is
clicked, through the external-media handler. It also pauses and rewinds its
output by itself when the score ends, which would stop a backing track that
runs on past the last bar. So the handler passes play, pause and seek on only
within 1.5 s of a press on the score; anything else it asks for is ignored.

**Sync points.** One per bar *pass* (`barIndex` + `barOccurence`, since repeats
play bars more than once), at the bar's start, in the reference's milliseconds:
alphaTab's `FlatSyncPoint`, applied with `score.applyFlatSyncPoints` and
`api.updateSyncPoints()`. alphaTab interpolates between points and carries the
last tempo past the final one. With none, the score plays at its own tempo from
0:00. `syncPoints.ts` keeps the list usable: setting a bar replaces any point on
that pass and removes the ones that would make time run backwards, so a
mis-tap fixes itself on the next try.

**Editing sync.** "Edit Sync" turns a click on a bar into choosing it (outlined)
rather than seeking; it starts on bar 1 while nothing is synced, and on the bar
under the cursor after that. "Bar N starts here", or T, sets the chosen bar to the
reference's time now and moves on a bar in playback order: tap along while it
plays, or pause on the exact downbeat for precision. A key press is taken at its
event time, not when it is handled. "Shift all" moves every point by 10 ms (a
consistent tapping lag). Synced bars carry an amber flag.

**What remembers what.** Every reference remembers the tab last synced to it
(app setting `practiceTool.tabs`, keyed `track:<path>` or `yt:<videoId>`, at
most 300), like the per-file loop store: reopening a track brings its tab back.
Loading a track with nothing remembered closes a tab that was synced to the
previous reference, and keeps one that was not synced yet. A project saves the
binding (`tab`) and, when the reference is a video, `youtube`; a project may
follow only a video, with no track. `practiceTool.tabStaves` remembers Tab /
Notation + Tab / Notation.

**YouTube.** The video cannot be embedded straight into the app: on macOS and
Linux the WebView's origin is a custom scheme YouTube refuses to play under,
which is why the Jam panel embeds through the API's `/v1/embed/youtube`. That
page cannot report time, so the tab view uses `/v1/embed/youtube-sync`, which
runs YouTube's IFrame Player API on the API's origin and relays a small
postMessage protocol (documented in `youtubeReference.ts`). YouTube's script
never runs in the app's own page, which holds the native bridge; the app
accepts messages only from that iframe and that origin. The relay reports four
times a second while playing (twice while paused, only when the time moved);
the app dead-reckons in between. A seek before the video has started is held
until it plays, since YouTube's `seekTo` would start it.

## alphaTab timing traps

Found while driving the live app; the unit tests could not have caught them.

- `scoreLoaded` comes before the score's timing exists: `api.tickCache` (the
  playback order) is built in `loadMidiForScore`, which fires `midiLoaded` and
  *then* hands the player the sync points the score had at that moment. Sync
  points are checked and pushed from a microtask queued in `midiLoaded`, after
  that call has returned; pushed any earlier they are dropped or overwritten.
- With the render worker, `boundsLookup` (what the flags are placed by) arrives
  with `postRenderFinished`, not `renderFinished`.
- `pause()` leaves alphaTab's time at the start of the beat it stopped in, so the
  reference's position is pushed after play/pause is mirrored, not before.
- A score of 10 bars or fewer (and at most two tracks) renders without the
  attribution; that is alphaTab's rule, not ours.
- While the window is minimised the page is hidden: no animation frames, so the
  cursor does not move (by design) and screenshots time out. Live tests need the
  window restored.

## Not done

- Nano Q (the native UI) has no tab view; this is web UI only.
- No synthesized playback of the score: with no reference, the tab is a static
  display with a movable cursor.
- alphaTab's own loop and range selection is off; loops stay the Practice Tool's
  (backing track only). A YouTube reference has no loops.
- Stored score files are never deleted: a file no project or reference points
  at stays in the document store.
- Speed for a video is YouTube's own setting; the faders act on the backing
  track only.
