# Nano Q on GPUI: performance spike

2026-11-06. The port and its tools are in [nanoq/gpui-spike](../../nanoq/gpui-spike/README.md).

## Result

On this machine GPUI spends about a third of Nano Q's UI-thread time on continuous redraws (knob
drag, list scroll), at a steadier frame rate, and uses more GPU where it draws paths. At rest
Nano Q is cheaper. Latency is level on a drag.

I would not move Nano Q to GPUI on these numbers. The Windows renderer cannot go in a host's window,
so plugin editors are ruled out without patching GPUI's platform layer, and Nano Q is our only
native UI. Nano Q also spends most of its UI-thread time outside its own draw timing, and that time
has never been looked at. Finding it is a far smaller job than a port, and could close much of the
gap.

**The data is partial.** The full run was invalidated partway through (see Data quality) and then
stopped early. The table uses the one round and the pilots that held up, so read the numbers as
indicative, up to a few points either way. The ratios on knob drag and list scroll repeated each
time they were measured. Launch time was not measured.

## Setup

- Windows 11, AMD Radeon 780M, 1704x1114 physical window (960x600 layout at scale 1.75).
- Nano Q: Release build, Elements on Direct2D, with the real engine and a throwaway profile.
- Port: `gpui-kit` 0.9.0 (Zed's GPUI snapshot), `--features fast` for Longbridge's `gpui-fast`.
  Chain, Effect and Setlist pages, a preset list, the top bar, rail, footer and Controls strip,
  with the same icons, font and theme tokens, and the same primitive count per knob.
- The port has no engine. It replays a session Nano Q recorded (`SOUNDSHED_NANOQ_MSGLOG`) at the
  recorded times and parses the JSON on the UI thread, as Nano Q does. A knob drag builds the same
  `updateSignalPathNodeParam` request per change. So compare UI-thread figures. Nano Q's process
  figures include the engine.
- `gpui-component` widgets were not tried; the port uses only GPUI itself.

## Measurements

UI-thread CPU is one core's time spread across all cores, as Task Manager shows it. GPU is 3D engine
use by the process. Where two figures are given they are the pilot(s) and round 1. Each is a single
4 or 10 s window.

| Scenario | Nano Q UI % | GPUI UI % | Nano Q GPU % | GPUI GPU % | Redraws/s (Nano Q, GPUI) |
|---|---|---|---|---|---|
| Chain page, idle | 0.0 to 0.15 | 0.2 to 1.9 | 0 | 0 to 0.2 | 0, 0 |
| Chain page, playing | 0.16 | 0.94 | 0 | 0.02 | 3, 0 |
| Controls meters, playing | 6.6 to 8.7 | 2.7 | 0.8 | 2.9 | 10, 10 |
| Knob drag | 24.5 to 30.4 | 9.0 to 10.8 | 5.8 to 6.4 | 9.2 to 10.3 | 60, 54 to 58 |
| Card hover sweep | 1.7 to 2.3 | 3.1 to 3.6 | 0.2 | 1.9 to 2.1 | 2 to 3, 8 to 10 |
| Preset list wheel | 40.5 to 45.8 | 11.7 to 14.0 | 7.0 to 7.5 | 3.6 to 3.9 | 49 to 54, 60 |

| Latency, input to pixels | Nano Q p50 / p95 | GPUI p50 / p95 |
|---|---|---|
| Hover, real pointer (one round) | 49.1 / 52.8 ms | 33.0 / 35.2 ms |
| Held knob drag (one round) | 18.2 / 35.2 ms | 18.6 / 35.2 ms |

Latency includes reading the screen back, so compare the two columns and don't quote the numbers
alone. Hover in Nano Q carries about 16 ms by design (see below), which matches the gap. The hover
figures come from one round each, taken before the 1 ms rest was added to the read loop.

| Footprint, Chain page | Nano Q (with engine) | GPUI |
|---|---|---|
| Private memory | 128 to 133 MB | 104 to 109 MB |
| Working set | 146 MB | 111 MB |
| GPU dedicated / shared | 23 to 27 / 33 MB | 16 to 21 / 71 MB |
| Threads | 43 | 29 |
| Executable | 26 MB | 13.9 MB (15.6 MB with `gpui-fast`) |

Memory isn't a like-for-like comparison: Nano Q's holds an engine and models, the port's holds
neither. Even so, the UI layer alone is not small on GPUI: about 100 MB with no engine.

`gpui-fast` compiled with no code changes and showed no clear difference from stock. View caching
(`--no-cache`) showed none either, apart from one list-wheel window (17.3 vs 11.7 %), not enough to
conclude from.

Build cost: first release build 4m37s (2m51s with `gpui-fast`), incremental 37 to 46 s.

## What the numbers mean

- **GPUI redraws the whole window every frame.** Its Windows renderer has no damage rects: it draws
  the scene and presents in full. Nano Q repaints only what changed, which is why it's cheaper at
  rest and in the meters' GPU figure, and why the GPU column flips on a knob.
- **Paths are GPUI's expensive primitive.** They go through a 4x MSAA intermediate texture and a
  resolve. A knob is 24 arc slices, so a knob drag costs more GPU here than a list scroll that
  redraws the whole list.
- **GPUI's frame pacing is its vsync thread.** `DwmFlush` drives `RedrawWindow` at display rate even
  when idle, though no frame is drawn if nothing is dirty. That is the idle overhead above.
- **Nano Q's time is mostly not drawing.** With meters playing the UI thread uses 6.6 to 8.7 % while
  `DrawProbe` counts about 4 ms/s (0.4 % of one core). In a knob drag `DrawProbe` accounts for about
  half. The rest is outside its window: Direct2D `EndDraw` and present, message handling, or JSON and
  state work. It has not been attributed, and it is the best lead this spike produced.
- **Hover differs by design.** Elements learns the pointer position from `WM_MOUSEHOVER`
  (`TrackMouseEvent`, 16 ms), so it adds that delay and doesn't update while the pointer keeps moving.
  GPUI reacts to each `WM_MOUSEMOVE`. That's why it redraws more on a sweep and answers sooner.

## Blockers beyond performance

Read from the `gpui-pre-windows` and `gpui-fast-windows` sources.

- **No embedding in a host's window.** Neither has `WS_CHILD` or `SetParent`, so windows are
  top-level. `hwndParent` is dialog ownership only. `Application::run_embedded` needs a platform whose
  `run` returns at once, and the Windows one blocks in `GetMessageW`. A CLAP or VST3 editor needs a
  patched or custom platform, and GPUI would have to share the host's message loop.
- **Platforms.** Windows, macOS, Linux (wgpu on X11 and Wayland), web and iOS exist. There is no
  Android crate, which is fine while Android stays on WebView. The Pi kiosk is untested: no GPUI
  build has been run on it, and its GPU is already the constraint there.
- **Missing drawing.** There is no radial gradient, so the knob dome and card wash are linear stand-ins
  in the port.
- **A moving target.** `gpui` on crates.io is stale; `gpui-pre` and `gpui-kit` snapshot Zed's tree
  and move with it, so a port would carry API churn with no stability promise.
- **Port size and language.** All of the Elements UI would be rewritten in Rust and joined to the C++
  engine across an FFI boundary. Only the UiLink JSON mailbox would carry over.

## Data quality

The harness needed several fixes, and they're worth knowing before rerunning it:

- Posted moves with the real pointer outside the window cause `WM_MOUSELEAVE`, so every move looks
  like a hover change. `Bench.Park` puts the real pointer inside the view first.
- Posted moves can't drive Elements' hover at all, so the hover scenarios use `SetCursorPos`.
- A tight `BitBlt` loop starved GPUI's frames (a third of the drag samples missed, none for Nano Q).
  The read loops now rest 1 ms.
- The Noise Floor bar doesn't follow telemetry. Live meters are the Controls strip, hence
  `meters-play`.

The full matrix reached 94 of 96 rows. From partway through round 1 until near the end of round 3,
`SetCursorPos` and screen capture stopped having any effect while posted input still worked: card
hover drew 0 frames, latency missed 200 of 200, the DWM counter read 0 and Nano Q fell to 30 paints/s.
The likely cause is the display sleeping: this machine's timeout on AC power is 5 minutes, and
`SetCursorPos` doesn't count as activity. That wasn't confirmed directly, and a locked session would
look the same. It recovered in round 3's last few rows, so rounds 2 and 3 are left out of the table,
except where the figures matched round 1. The raw rows are in
[ab-2026-11-06-partial.jsonl](../../nanoq/gpui-spike/results/ab-2026-11-06-partial.jsonl), and the
last pilot is beside them.

## To finish the numbers

Keep the screen awake and unlocked (the 5-minute display timeout has to be lengthened first) and
leave the mouse alone for about 35 minutes:

```powershell
pwsh -File nanoq/gpui-spike/tools/ab.ps1 -Runs 3 -Seconds 10
python nanoq/gpui-spike/tools/summarise.py C:\t\ab.jsonl
pwsh -File nanoq/gpui-spike/tools/coldstart.ps1
```

A valid run shows `dwm3dPct` above 0 on `knob-drag` and about 60 Nano Q paints/s. `coldstart.ps1` has
not been run yet.
