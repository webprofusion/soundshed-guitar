# Changelog

## Unreleased

### DSP & Plugin Hosting
* Fixed hosted plugins being fed stale audio when the host delivers a block smaller than the declared maximum (e.g. ASIO buffer sizes below 256). The plugin now sees exactly the frames the host provided instead of processing the previous block's tail.
* Hosted plugins are now explicitly resumed after `prepareToPlay`, so plugins that gate processing on `isSuspended()` no longer load silently without affecting the audio.
### Presets & Workflow
* Effects now have a Presets button in their header, opening a small flyout with a single list of factory presets followed by your own saved settings. Save the current settings of any effect under a name and load them onto any other preset.
* Added back/forward buttons beside the preset selector to step through recently loaded presets (up to 10), for quick A/B comparison between tones.

### Reliability
* UI storage files (setlists, automation, preset folders/favourites/ratings) are now written atomically, so a crash or power loss mid-write can no longer truncate them.
### DSP & MIDI
* Keyboard shortcuts mapped to automation slots now work whether or not the MIDI & Automation panel is open. Previously they only fired while that panel was visible, which made them unusable in practice.
* The spacebar is never captured by keyboard mappings and always passes through to the host, so DAW transport keeps working while the plugin window has focus.
* Added MIDI/automation slots for direct scene selection (Scene 1–4), so a footswitch can jump straight to a scene. Works with the plugin window closed.
* Graphic EQ profiles are now purely band layouts: every profile is flat, differing only in how many bands it has and where they sit. Selecting a profile no longer imposes a voicing, and a newly added Graphic EQ starts flat. Save your own curves as effect presets instead.
* Graphic EQ gained a Reset button that returns every band to 0 dB without changing the selected profile.

## 1.5.0 (July 25, 2026)

### Performance, Presets & Settings
* Added a dedicated Setlist performance pads view in the Play tab, with configurable 4, 6, or 8-pad layouts.
* Update setlist management with named banks and per-pad preset assignments for faster live preset switching.
* Standalone app global effects now reliably retain their state, and settings synchronize across multiple plugin instances.

### DSP & MIDI
* Added a Graphic EQ effect with 5- and 10-band Bass, Guitar, and General Purpose profiles.
* Improved MIDI automation for effect bypasses, including reliable Note On/Note Off toggle behavior and effect-type bypass automation.
* Fixed the selected input audio channel being restored incorrectly at startup.

### Library & Tone Sharing
* Resource folder browsing now remains responsive with large folders.
* Tone Sharing now displays download counts and handles expired sign-in sessions more gracefully.

### UI & Workflow
* Reworked the signal-chain interaction with a more compact layout, clearer parallel-routing controls, and node bypass buttons.
* Added collapsible signal-chain and app control areas.
* Improved responsive layouts, including the control bar and footer, and reduced the minimum window size to 640 x 400.
* Added DSP stats to individual effect visualization
* Refined light and classic themes, knob styling, UI scaling, and overall visual consistency.

### Platform Reliability
* macOS fixed app entitlements
* Improved macOS hosted-plugin loading error reporting.
* Improved Linux WebKit view discovery for more reliable standalone startup.

## 1.4.0 (July 03, 2026)

### MIDI & Automation
* Added full MIDI parameter mapping and automation workflows.
* Added direct MIDI setlist bank selection, plus bank up/down behavior fixes.
* UI now updates parameter values immediately when controls change via MIDI.

### DSP & Audio
* Revised NAM calibration support for chained NAMs
* Reviewed NAM calibration handling for stereo vs. mono signal paths and added a post high-pass DC blocker (5 Hz) to match the reference gateway.
* Added low-latency mode for IR Cab and IR Reverb, with low latency now used by default for IR Reverb.
* IR Reverb output balancing now uses L2-normalized gain for more consistent loudness.
* Fixed EQ artifacting when adjusting EQ parameters.
* Improved mono ping-pong delay behavior to preserve a correct mono main path.
* Fixed blend effect behavior and improved live model blend preview.

### Audio Import & Formats
* IR Cab loading, riff import, and demo audio now support AIFF/AIFC and MP3 files in addition to WAV, via a new shared multi-format audio decoder.

### Signal Analyzer
* Added a new Signal Analyzer utility effect for real-time signal level diagnostics.
* Added LUFS loudness measurements to the Signal Analyzer.
* Added bark band perceptual analysis to the Signal Analyzer.
* Signal Analyzer now supports mono/stereo input modes.

### Presets, Library & Resources
* Added a new folder browser with favorites and preview support for local resources.
* Added tagging and filter-by-tag support to the resource and folder browser, plus general UI polish.
* Added an architecture filter and improved navigation for NAM models in the resource browser.
* Added prev/next selectors for stepping through resources without leaving the browser.
* Added a calibration indicator to show when a NAM model includes calibration data.
* Preset import now creates the destination folder when needed.
* Fixed "Save New Preset" flow where a newly created preset could be empty.
* Added library resource delete support.
* Fixed Tone3000 BYOK loading/authorization flow and added favourites support in BYOK mode.
* Improved startup and browsing performance by optimizing resource loading and deferring heavy tone/jam loads at startup.

### Jam, UI & UX
* Jam tab is now enabled with substantial layout and interaction improvements.
* Fixed Jam video playback integration issues including CORS handling, scrolling, touch drag, and WebKit docking behavior.
* Migrated UI icons to SVG assets for cleaner and more consistent cross-platform rendering.
* Expanded and consolidated theme/layout work (including light/classic refinements) and improved general UI consistency.
* Reworked UI assembly/components architecture for a more maintainable and flexible interface foundation.

### Platform & Packaging
* Windows installer now allows choosing a custom install location.

### Stability & Internal Improvements
* Reduced unnecessary UI updates caused by DSP performance stats.
* Fixed live DSP stats updates.

## 1.3.0 (June 16, 2026)

### Neural Amp Modeler (NAM)
* Reworked NAM processing to more closely match the reference implementation. Resampling performance fixes.
* Input leveling from model metadata is now applied for more consistent output levels across model (for first nam in chain)
* Unified and simplified auto-gain calibration.

### IR Cab
* Added **pan** or **L/R split** controls to the IR Cab, enabling true stereo cabinet placement.
* Simplified and corrected IR level normalization

### Signal Chain & Performance
* **Faster preset switching**: presets now load on a background thread, eliminating audio interruptions when changing presets.
* Signal chain is safely paused during preset loading to prevent partial-state processing artifacts.
* New **parallel execution pipeline** for the effects chain with adaptive workload balancing, improving CPU efficiency on multi-core systems.
* Mono/stereo mode is now correctly recalled between sessions.

### EQ
* Low and high shelf filters now expose a **Q / resonance** control for more precise tonal shaping.

### Mixer / Splitter
* Fixed delay option on the Mixer/Splitter node. iF your preset suddenly have a delay it's becuase it wasn't working before, check the mixer node.

### UI / UX
* Overhauled **Tone Sharing and Browser** experience.
* Preset folder rename and delete workflow improvements.
* **Toast notifications**: status messages now appear as floating overlays rather than being embedded in the footer bar.
* Improved inline knob editor keyboard input behavior.
* Light mode styling improvements.
* Add NAM/IR metadata details in resource browser
* Add Favourites toggle for local resources

### Plugin Host
* Prevented a crash that could occur when the plugin attempted to host itself.
* Improved LV2 plugin list handling and support for bundle-hierarchy plugin types.
* Plugin list UI updated with cleaner item selection and keyboard navigation support.
* When running inside a DAW as a plugin, input channel and mono/stereo mode are now correctly delegated to the host.

### Tone 3000 / Sharing
* Improved Tone3000 integration; the library tab (used for advanced/experimental blends) is now **disabled by default** in Settings to avoid confusion.
* Shared preset titles now prefer the original published name.
* Improved resource handling and UI for shared/imported presets.

### Fixes
* Fixed preset pack uninstall.
* Fixed resource naming during import/export publish.
* Preset-level data no longer inadvertently inherits global signal chain settings.
* DSP diagnostic message rate is now debounced to reduce UI thread overhead.

---

## 1.2.0 (June 05, 2026)
* Support for NAM A2 architecture, including option to adjust NAM processing quality (Settings > DSP Performance). 
* Updated Tone 3000 integration, default to A2 nam models
* Mono/Stereo path DSP optimizations
* Combined performance work drops single-thread/core DSP usage from 30% to 5-10% when using mono path with NAM A2 in "slim" (min) quality mode.
* Clap plugin available on all platforms

## 1.1.0 (May 23, 2026)

---

### 🎧 Core DSP & Audio Engine Updates

*   **Full Stereo Signal Path** (`79e039b`): Implemented a full stereo processing signal path, allowing for rich, immersive stereo audio across the entire effects chain.
*   **Upgraded Resampling & Sample Rate Consistency** (`8078a07`, `1022820`, `4a0fa6c`):
    *   Enhanced **Convolution Reverb** (formerly IR Reverb) resampling with windowed sinc interpolation and proper normalization.
    *   Improved resampling pipelines and sample rate consistency across both Convolution Reverb and Neural Amp Modeler (NAM) effects.
*   **Reverb & NAM Optimizations** (`dba4f66`, `43274d0`, `c2cbbd1`):
    *   Optimized reverb DSP for sound quality and lower CPU overhead.
    *   Renamed **IR Reverb** to **Convolution Reverb** for a cleaner and more professional user interface.
    *   Added a mix wet/dry parameter to the Neural Amp Modeler (NAM) effect.
*   **Performance Tuning** (`fe2ff17`, `684c9c5`):
    *   Limited the frame-rate (FPS) of DSP performance metrics updates to reduce UI thread load and overhead.
    *   Enabled the DSP diagnostics signal meter by default to ensure real-time visual feedback is always available.
*   **Input Calibration & Level Management** (`9445b6a`, `85c83a6`, `21be4a1`, `fa0f5b9`, `c9550cc`):
    *   Simplified and streamlined the audio levels and calibration setup across the entire application and signal chain.
    *   Implemented named input level calibrations and target training.
    *   Disabled calibration controls during active training sessions and added a clean deletion mechanism for calibrated profiles.
    *   Removed legacy, redundant calibration code and duplicate UI components.
*   **Hybrid Transpose & Pitch Corrections** (`3c6712e`, `52701f6`, `2b26d6e`, `be89472`):
    *   Added first-phase support for a Hybrid Transpose mechanism.
    *   Resolved an issue where global pitch transpose interfered with preset-specific signal chain transpose parameters.

---

### 🔌 WASM & Hosted Plugin Integrations

*   **WASM Effects Infrastructure** (`e9d3584`, `338202b`, `84891b8`, `8a92aed`):
    *   Implemented the first-draft **WASM effect host**, allowing custom-compiled WASM effects to run directly in the engine.
    *   Added parameter publishing capabilities for WASM effects.
    *   Laid out the API and UI blueprint for generating custom effects.
    *   Added thumbnail/visual support for loaded WASM modules.
*   **Linux LV2 Support** (`dd30e29`, `f43a570`, `5d7b3d0`):
    *   Added native LV2 plugin support on Linux environments.
    *   Fixed multi-plugin instantiation bugs allowing users to run more than one plugin concurrently.
*   **Hosted Plugin State Capture & Diagnostics** (`be5393b`, `af48bbf`, `aea8bf9`, `9dccf4b`):
    *   Implemented a JSON-based debug state capture mechanism for hosted plugins to simplify diagnostic reporting.
    *   Ensured plugin state updates are correctly validated and tracked across platform boundaries.
    *   Made hosted plugin IDs intentional and consistent across platforms (Windows, macOS, Linux).

---

### 🎨 User Interface & Experience (UI/UX)

*   **UI Modernization & Custom Layouts** (`49a571a`, `2c38fad`, `57c577a`, `8ae0491`, `5234a23`):
    *   Polished visual assets and updated modern icons for all effects.
    *   Improved custom effect and visualization layouts, with specific fixes for visual alignment.
    *   Updated primary design aesthetics, control bar, and footer styling.
*   **Interface Zooming** (`3ea39eb`): Implemented native UI zoom controls to support scaling on displays of varying resolutions.
*   **Demo Audio Render Actions** (`3b0b386`, `1022820`):
    *   Added an action context menu to the Demo Audio player.
    *   Allowed direct rendering of demo tracks at a selectable sample rate (kHz) and saved user preferences for future sessions.
*   **Jam & Practice Enhancements** (`67e983b`, `8a9f584`):
    *   Added a **Scales Tab** to the Jam menu to assist with practice and improvisation.
    *   Implemented Jam query caching to make searches instant.
*   **Integration Feature Flags** (`dcda73f`): Introduced runtime application feature flags, enabling users to toggle active external integrations and experimental features on/off.
*   **Preset & Sharing Refinements** (`aa7fa1b`, `5be14df`, `00561b6`):
    *   Added a quick-close button for the Preset view.
    *   Fixed preset z-index stacking to prevent UI overlapping.
    *   Ensured the "Save As" command successfully assigns a unique preset ID.
    *   Improved the handling and parsing of tone sharing web links.
*   **Session Security** (`c9678c4`): Added auto-refresh support for Tone300 sessions to maintain connectivity.

---

### ⚙️ Build System & Platform Support

*   **Windows x86 (32-bit) Support** (`47ffeee`, `f9fbfe2`, `1ee7d96`, `d73754e`):
    *   Added full build and packaging pipeline support for 32-bit Windows systems.
    *   Added comprehensive diagnostic tooling to troubleshoot and validate 32-bit compiler architectures.
*   **Intel IPP DSP Optimizations** (`c92c7ec`, `5cc444b`, `82fe49b`, `65f75fd`, `249f472`):
    *   Integrated and modularized Intel Integrated Performance Primitives (IPP) config inside CMake helpers.
    *   Configured architecture-specific detection for x86 and x64 builds, falling back gracefully to optimized alternates.
*   **macOS & Linux Fixes** (`f009595`, `0cdfd71`, `0e9b583`):
    *   Resolved macOS microphone and input device permission consent bugs.
    *   Compiled native arm64 and x64 executables for Linux environments, standardizing the resulting filenames.
    *   Suppressed excessive and noisy console output in Linux runtime environments.
*   **Codebase Refactoring & Cleanup** (`fd27eee`, `1314cb4`, `52d221c`, `4d77e4b`, `644a190`):
    *   Conducted sweeping TypeScript cleans and architectural refactoring.
    *   Tidied up JUCE plugin wrapper files and unified DSP bypass/passthrough paths.
    *   Added logging adjustments for a "no-debug" clean launch.


## 1.0.3 (March 27, 2026)
- Fixed macOS standalone microphone permissions. In the standalone app, use Settings > Audio Preferences to set inputs.
- Fixed macOS plugin window sizing and remembered standalone window state preferences.
- Temporarily removed the Jam tab on macOS due to a WKWebView YouTube referrer issue.
- Reorganized Settings > DSP performance.
- Added keyboard focus and direct value entry for knobs, plus mouse wheel editing.