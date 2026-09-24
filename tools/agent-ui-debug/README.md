# Agent UI Debug: driving the live app over WebView2 remote debugging

The Standalone app's UI is a WebView2 (Chromium) view (see `juce/source/PluginEditor.cpp`).
Browser-automation "computer use" tools only reach a real Chrome/Edge browser
tab — they cannot see or click this native Win32 window. `cdp-tool.mjs` talks
Chrome DevTools Protocol (CDP) directly to that same WebView2 instance, so you
get screenshots and real interaction against the **actual running app** (real
backend, real DSP state, real preset data) instead of a static code read.

See `docs/agent-quickstart.md` → "Live UI testing" for when to reach for this.

## Workflow

1. **Build the Standalone target** (Release is usually what's already built
   under `juce/builds/SoundshedGuitar_artefacts/`; Debug works too):
   ```bash
   cmake --build juce/builds --config Release --target SoundshedGuitar_Standalone --parallel
   ```

2. **Launch with the remote-debugging port enabled.** WebView2 reads this
   standard env var; no code changes needed. Pick an uncommon port:
   ```bash
   WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9333" \
     "juce/builds/SoundshedGuitar_artefacts/Release/Standalone/Soundshed Guitar.exe" &
   ```
   Run this in the background (e.g. the Bash tool's `run_in_background`) —
   it's a GUI app that stays open. A launch with this variable set uses its own
   WebView2 profile folder (`%LOCALAPPDATA%\Soundshed Guitar\WebView2-<hash>`),
   so it runs alongside a normally launched instance: WebView2 refuses to share
   one profile between processes whose browser arguments differ.

3. **Get the page's websocket URL:**
   ```bash
   curl -s http://127.0.0.1:9333/json/list
   # -> "webSocketDebuggerUrl": "ws://127.0.0.1:9333/devtools/page/<id>"
   ```
   There's exactly one page target, titled "Soundshed Guitar", at
   `https://juce.backend/...` (a custom scheme intercepted by JUCE's
   resource provider — don't try to navigate a separate browser tab to it,
   it won't resolve there).

4. **Drive it with `cdp-tool.mjs`** (requires Node 22+ for the built-in
   `WebSocket` global):
   ```bash
   WS="ws://127.0.0.1:9333/devtools/page/<id>"

   # Click something, then screenshot the result:
   node tools/agent-ui-debug/cdp-tool.mjs "$WS" \
     --eval "document.getElementById('preset-chooser-label').click()" \
     --screenshot out.png

   # Query DOM state / read values:
   node tools/agent-ui-debug/cdp-tool.mjs "$WS" \
     --eval "Array.from(document.querySelectorAll('.mixer-preset-tab')).map(t=>t.textContent.trim())"

   # Drag a custom knob widget with REAL Input.dispatchMouseEvent events
   # (JS-dispatched MouseEvents often aren't enough for drag-based widgets;
   # CDP Input events behave like genuine OS input):
   node tools/agent-ui-debug/cdp-tool.mjs "$WS" \
     --drag 511,831,60 \
     --eval "document.querySelector('.node-param-knob').dataset.value" \
     --screenshot after.png
   ```
   Prefer `--eval` with `document.querySelector(...).click()` / direct DOM
   queries over pixel-coordinate clicking — it's precise and doesn't depend
   on the window's current size/scale.

5. **Verify against backend truth, not just the optimistic client UI.**
   Client-side state can look right (a knob visually moves, a "dirty" badge
   appears) while the actual change silently failed to reach the C++ engine.
   If the "Capture Debug State" feature is enabled (Settings → Features →
   "Capture Debug State"), click the footer camera icon
   (`.footer-debug-capture-btn`) or:
   ```bash
   node tools/agent-ui-debug/cdp-tool.mjs "$WS" \
     --eval "document.querySelector('.footer-debug-capture-btn').click()"
   ```
   This writes a full backend+UI snapshot to
   `%APPDATA%\Soundshed Guitar\data\v1\logs\debug-state.json` — inspect
   `backend.presetId`, `backend.preset` (note: nodes live under
   `preset.scenes[i].graph.nodes`, keyed by `backend.activeSceneId`, not a
   flat `preset.graph`), and `backend.mixer.activePresetIds` to confirm the
   engine's actual state, independent of what the UI is showing.

6. **Clean up when done:**
   ```bash
   taskkill //F //IM "Soundshed Guitar.exe"
   ```
   Also delete any throwaway presets/composite-presets you created under
   `%APPDATA%\Soundshed Guitar\` — this is the real user profile, not a
   sandbox, and mixer/session state persists across relaunches.

## Gotchas found the hard way

- A duplicate/leftover build or launch process holding the same output
  files causes `C1041` PDB-lock errors that look like real compile failures.
  Kill stray `cmake.exe`/`MSBuild.exe`/`Soundshed Guitar.exe` processes
  before re-running a build or relaunch.
- Feature flags (e.g. "Multi-Rig Mixer") default off and persist across
  restarts once toggled — check `#feature-toggle-<id>` state before
  assuming a feature is hidden/broken.
- Mixer/session state persists across app restarts too; a fresh launch is
  not necessarily a clean slate.

## Soundshed Guitar Nano (native UI)

Nano has no WebView, so CDP cannot see it. It has its own debug port instead, opened only
when `SOUNDSHED_NANO_DEBUG_PORT` is set, and `native/nano-tool.mjs` drives it:

```powershell
$env:SOUNDSHED_NANO_DEBUG_PORT = "9444"
Start-Process "juce\builds\SoundshedGuitarNano_artefacts\Debug\Standalone\Soundshed Guitar Nano.exe"
node tools/agent-ui-debug/native/nano-tool.mjs 9444 state
node tools/agent-ui-debug/native/nano-tool.mjs 9444 tree --ids
node tools/agent-ui-debug/native/nano-tool.mjs 9444 resize 800 480
node tools/agent-ui-debug/native/nano-tool.mjs 9444 click nav-chain
node tools/agent-ui-debug/native/nano-tool.mjs 9444 longpress node:amp_0
node tools/agent-ui-debug/native/nano-tool.mjs 9444 menu "Move later"
node tools/agent-ui-debug/native/nano-tool.mjs 9444 screenshot C:\t\nano.png
```

The script's header lists every command (`type`, `combo`, `slider`, `theme`, `send`, ...).

- **Ids** are component ids, plus the targets a view paints itself: `node:<id>`,
  `bypass:<id>` and `add:<id>` on the chain, `fx:<effect name>` in the effect picker,
  `preset:<name>` and `slot:<n>` in the preset and setlist lists, and on the Tones page
  `tone:<title>`, `installed:<title>` and `action:<text>` ("Load more"). The Tones page talks to
  the real tone sharing service and installs into the real profile: back the profile up first,
  and remove what a test installs from its Installed tab.
- **Menus:** JUCE closes a popup menu as soon as the app is not in front, which it never is
  while a terminal drives it. `menu` falls back to the copy `NanoContext::showMenu` keeps of
  the last menu, so open it (`click`, `longpress`) and then choose from it.
- **`send`** goes straight to the engine, so the client's copy of a setting does not change
  (the engine does not echo `setSetting`); use the control (`slider`, `combo`) instead.
- It shares the real profile, and only one of the two standalones runs at a time. Back up
  and restore the profile as for the WebView app, and `Stop-Process -Name "Soundshed Guitar Nano"`
  when done.
