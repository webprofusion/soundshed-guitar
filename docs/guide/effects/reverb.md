# Reverb

> Simulates the acoustic character of a physical space — from a small practice room to a huge concert hall.

Reverb adds the sense of space and dimension that makes a sound feel like it exists somewhere real rather than coming directly from a speaker. Soundshed Guitar currently includes five reverb types: choose the one that suits the sound and size of space you want.

---

## Common parameters

Most reverb types share these controls:

| Parameter | Range | What it does |
|-----------|-------|--------------|
| **Decay** | 0–100% (or in seconds, varies per type) | How long the reverb tail rings out before fading to silence. Short Decay = tight, close space; long Decay = huge, cavernous wash |
| **Mix** | 0–100% | Blends the reverb (wet) signal with the dry signal. Keep Mix moderate (10–30%) for most guitar use — too much washes out note definition |
| **Pre-Delay** | 0–220 ms | A short silence between the dry note and the onset of reverb. Adding a few ms of Pre-Delay (10–30 ms) separates the dry attack from the reverb wash, preserving clarity while still sounding spacious. Spring reverb uses an internal tank attack delay instead of exposing this control directly |

---

## Room

> A small to medium natural-sounding room reverb.

The Room reverb adds the tight, bouncy reflections of a real room. It sounds natural and unobtrusive, with a shorter decay and a more immediate sense of space.

### Additional parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **Size** | 0–100% | 50% | The perceived size of the room. Small Size = narrow with fast early reflections; large Size = more open and spacious |
| **Damping** | 0–100% | 50% | How much high-frequency content the reverb absorbs over time. High Damping = warm, dark tail (like a room with soft furnishings); low Damping = bright, reflective tail |

### Tips
- Use Room reverb for live-sounding guitar tones — it adds space without obviously colouring the tone.
- Short Decay (30–40%) and moderate Mix (15–20%) keeps the guitar present in the mix while still sounding like it exists in a real space.

---

## Chamber

> A medium-sized, warm reverb that sounds like a dedicated echo chamber or live room.

Chamber reverb sits between a small room and a larger live room in size: intimate but not tight. The character is warm and organic, inspired by the physical echo chambers studios once used for natural reverb.

### Additional parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **Size** | 0–100% | 38% | The perceived size of the chamber — from a small booth to a large live room |
| **Tone** | 0–100% | 42% | Brightness of the reverb character. Lower values produce a warmer, darker chamber sound |

### Tips
- Chamber works well on blues, classic rock, and jazz tones — the warmth and mid focus complements these styles naturally.

---

## Spring

> Recreates the wobbly, splashy character of a spring reverb tank built into vintage combo amplifiers.

Spring reverb has a very distinct, characterful sound. The mechanical nature of a spring tank creates a slightly sharp, dripping character — particularly noticeable on transients and in the reverb tail. It is the signature sound of surf guitar, vintage country, and rockabilly. The dedicated spring processor models a short, dispersive tank rather than reusing the general room/chamber engine.

### Parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **Tone** | 0–100% | 52% | Adjusts the overall brightness and harshness of the spring character. Lower for a warmer spring; higher for the classic bright, glassy drip |
| **Drive** | 0–100% | 18% | Controls how hard the spring is "driven." Higher Drive increases the splashy, saturated character — higher values can produce the overloaded spring "crash" effect |

### Tips
- Spring is most effective at moderate-to-high Mix levels (30–50%) — the character becomes apparent and musical at these levels.
- The **Drive control is what makes spring reverb sound like spring reverb** — raise it for that distinctive bright, splashy drip rather than keeping it clean.
- Works beautifully with clean amp tones. It is less suitable after heavily distorted signals.

---

## Advanced

> A fully featured reverb with controls for every aspect of the space, modulation, and tone shaping.

The Advanced reverb gives you complete control over the reverb character, including internal delay-line modulation (to prevent static metallic ringing), ducking (to reduce reverb when you are picking), filtering, and more.

### Additional parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **Size** | 0–100% | 40% | The scale of the reverb space. Larger Size increases decay length and the sense of distance |
| **Diffusion** | 0–100% | 74% | Smoothness of the early reflections. High = smooth; low = more distinct early reflections |
| **Low Cut** | 20–1200 Hz | ~80 Hz | Removes low-frequency content from the reverb tail — prevents the reverb from becoming boomy |
| **High Cut** | 1000–20000 Hz | ~8 kHz | Removes high-frequency content from the reverb — warms up a harsh tail |
| **Mod Rate** | 0.02–8 Hz | ~0.5 Hz | Speed of internal LFO modulation. Modulation prevents the unnatural metallic "ringing" of static reverb. Low values (0.1–1 Hz) are subtle and natural |
| **Mod Depth** | 0–1 | ~0.2 | How much pitch variation the modulation introduces. Subtle values (0.1–0.3) add life; high values create obvious pitch wobble |
| **Ducking** | 0–100% | 0% | Reduces reverb level when you are actively playing, and lets it bloom when you stop. Helps keep the dry signal clear in the mix |
| **Drive** | 0–100% | 0% | Adds saturation inside the reverb tail — a subtle warmth at low values, a more coloured character at high values |
| **Tone** | 0–100% | 42% | Overall brightness of the reverb tail |
| **Width** | 0–100% | 73% | Stereo width of the reverb |

### Tips
- The **Ducking** control is especially useful for rhythm playing — the reverb stays out of the way while you pick but blooms naturally during pauses.
- Start with Advanced reverb as a better-controlled version of any of the simpler types — use Low Cut and High Cut to keep the reverb from conflicting with the dry guitar tone.
- **Mod Rate and Mod Depth at subtle values (15–25%)** make a significant improvement to reverb realism without being audible as obvious modulation.

---

## IR Convolution Reverb

> Uses an Impulse Response file to reproduce the exact acoustic character of a real physical space.

The IR Convolution reverb works the same way as an [IR Cabinet](cab-ir.md) — it captures the precise acoustic response of a real space (a studio room, a church, a cave, a famous concert hall) and applies it to your signal. The result is a highly realistic sense of that specific space.

### Additional parameters

| Parameter | Range | Default | What it does |
|-----------|-------|---------|--------------|
| **IR File** | (resource picker) | — | The impulse response file that defines the reverb character. Click the folder icon to browse your library |
| **Mix** | 0–100% | 30% | Blends the reverb (wet) with the dry signal |

### Tips
- Many free and commercial reverb IR libraries are available online and can be imported into your resource folder.
- Unlike the built-in algorithmic reverbs, the IR Convolution reverb precisely captures a real space — but you cannot adjust its decay length or size without a third-party IR editor tool.
- Use Pre-Delay (set in the global chain or via an upstream effect) to separate the dry signal from the reverb onset.
