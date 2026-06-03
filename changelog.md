# Changelog

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