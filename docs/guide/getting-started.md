# Getting Started

This guide walks you through installing Soundshed Guitar, setting up your audio device, and loading your first preset so you can play in a few minutes.

---

## System requirements

- **Windows 10/11 (32-bit x86 or 64-bit)** — standalone app or VST3 plugin
- **macOS 11+** — standalone app, VST3, or AU plugin
- An audio interface with a guitar input (recommended) or a USB guitar cable
- Headphones or monitors

The Windows x86 build excludes generic WASM effects because Wasmtime does not provide a Windows x86 C API bundle.

---

## Installation

1. Download the latest installer from the Soundshed website.
2. Run the installer and follow the on-screen steps.
3. Launch **Soundshed Guitar** from your applications folder or Start Menu.

If you are using the VST3 plugin, open your DAW, rescan plugins, and add Soundshed Guitar to a new instrument or audio effect track.

---

## Setting up your audio device

When you launch the app for the first time, open **Settings** (gear icon, top right):

1. Under **Audio Device**, choose your audio interface from the dropdown.
2. Set the **input channel** to the socket your guitar is plugged into.
3. Set the **output channels** to your monitors or headphones.
4. Choose a **buffer size** — lower values (64–128 samples) give less delay but use more CPU. Start at 128 and lower it if your system can handle it.
5. Click **Apply**.

You should see the input level meter react when you play. If the meter stays silent, check your interface gain knob and cable.

---

## Loading your first preset

1. Click the **Preset Browser** tab (folder icon in the navigation bar).
2. Browse the **Factory** folder — pick any preset that sounds interesting or matches the style you play.
3. Click the preset name to load it. The signal chain updates immediately.
4. Play your guitar — you should hear the full tone through your monitors or headphones.

---

## Adjusting the input level

If the tone sounds too compressed or harsh, your input signal may be too hot:

- Open **Settings → Input Trim** and reduce it by a few dB until the tone opens up.
- As a rough guide, your input level meter should peak in the green/amber zone while playing hard — not constantly red.

---

## Next steps

- [Learn about the signal chain](signal-chain.md) to understand how effects are ordered
- [Use the Signal Chain Editor](signal-chain-editor.md) to build your own tone from scratch
- [Browse community presets and models](community.md) to expand your library
