# Changelog

## 1.7.3 (Unreleased)
* **macOS**: choosing a template from **New Preset** now creates the preset. Before, the menu closed as you clicked and nothing happened.
* **macOS**: the Standalone's Plugin Host can now load third-party VST3, AU and LV2 plugins. macOS rejected every plugin signed by another developer, so scans came back empty. If a plugin is still blocked, the error now says that macOS may be the cause.
* **macOS**: naming a Multi-Rig mix, saving a Custom Effect and editing a library item's details now ask for the text in the app's own dialog. The Mac app shows no browser prompt, so these did nothing before.
* **macOS**: soundshed:// links from the browser, which open a Tone Sharing tone in the app, now reach the Standalone app. The link was never registered with macOS.
* Installing a Tone Sharing pack no longer fails when some of its Tone3000 models have been removed or made private. The presets are installed with those slots empty, and a dialog lists the models it couldn't get.
* On Windows, the factory presets' models and IRs now show their names when your Windows user folder has a long path. Before, the files were installed but left out of the library, so the presets' amps and cabs showed a 64-character file code instead of the model's name.
* Factory models keep their details (A1/A2, calibration) across a relaunch, instead of losing them until the file was read again.
* The Standalone keeps the input mode you choose in Nano (Mono In 2, for example) when you load another preset.
* The built-in amp no longer goes silent after a bad (non-finite) sample from an earlier effect or hosted plugin until its node is rebuilt.
* Jam's backing-track titles show apostrophes and ampersands properly instead of codes such as `&#39;`, in favourites saved before too.
* On a touch screen, opening the FX Library no longer brings up the on-screen keyboard.

## 1.7.1 (Oct 23,2026)
* Minor fixes: Window chrome style, plugin hosting performance fixes, fix Jam search tab

## 1.7.0 (Oct 22,2026)

### MIDI
* The app can now tell your MIDI device when you pick a bank or a setlist slot in the app, so a controller or a second unit following along shows the same selection. Turn on **Send a program change to the MIDI device** in the MIDI & Automation panel's Mappings tab and choose the channel. Picking a slot sends its setlist's bank number as Bank Select, then a Program Change for the slot (slot 1 is program 0); picking a setlist sends its Bank Select alone. Choose CC0 + CC32, CC0 only or CC32 only to suit your device, or no Bank Select at all. Changes made from the MIDI device itself or from your DAW are not sent back to it.

### New Effects
* **Noise Reduction**: takes the hum, buzz and hiss out of a guitar or bass pickup without gating it. Put it first in the chain and pick your pickup type. It learns your hum and buzz in the pauses between notes and cancels it all the time, under the playing too, following the mains frequency as it drifts and working out for itself whether you are on 50 or 60 Hz. The hiss stage closes the top end only as a note fades into hiss, so the body of every note and its decay are left alone. No latency. Set Listen to Removed Noise to hear exactly what it takes out.

### Noise Gate
* **Learn** sets the noise gate's threshold for you. Mute the strings and press Learn: the gate listens to your rig's noise for two seconds and sets Threshold just above it, high enough that hiss can't open the gate or hold it open after a note. It's at the top of the global gate's settings (the chevron beside the GATE knob) and in the Noise Gate effect's panel, under a meter that shows the noise it heard against the threshold. If you played while it listened it says so and leaves the threshold alone. In an effect's panel, Undo puts the old threshold back.

### Signal Chain
* An effect's DSP status readout (the meter button on its panel) can now show the loudness of the effect's output in LUFS: momentary (the last 400 ms), short-term (the last 3 seconds) and integrated (since you opened the readout), measured as broadcast and streaming loudness meters measure it. Measuring costs your audio nothing you could notice: it runs only while a loudness figure is on screen, for that one effect, and away from the audio thread.
* Choose what the DSP status readout shows. It starts with peak, RMS, processing time and latency; each figure has a small × to remove it, and the + after them adds any of the others, loudness included. Your choice is remembered.
* The output meter has a readout of its own: the chevron under its mute button opens the output's figures and the whole chain's DSP load, its load peak (the heaviest moment in the last 10 seconds, which is what causes crackles and dropouts) and its latency. You choose its figures the same way, and can add the output's loudness.
* You can now remove a split when one of its branches has effects. Click the mixer's × (or select the mixer and press Delete) and that branch's effects move back into the main signal path, in the same order. If more than one branch has effects, it still asks you to clear all but one first.
* The IR Cab has a new **Trim Leading Silence** switch on its Advanced tab, off by default. Many cab IRs start with a moment of silence, the time the sound took to reach the mic, and you hear it as latency when you play: up to 3.5 ms in the IRs we checked. Turn the switch on and the cab starts where the IR's sound does. With two IRs loaded, both are trimmed by the same amount, so their alignment, and any IR B offset you set, stays as it was. With Quality below Full, the length of IR it keeps is then all sound. It is off so your presets keep the timing they were made with.
* The Harmonizer has a **Mix** knob. At full (the default) the guitar and the harmony play in equal parts, as before; turn it down to bring the harmony further under the guitar, down to the guitar alone at zero. The guitar's own level stays put.
* An empty model or IR slot now tells you what to do with it. Instead of "No model selected" it reads **Choose a model** (or **Choose an IR**) with a Browse button, and says you can drop a `.nam` or `.wav` file on it. A loaded slot shows a small arrow, so it's clear that clicking the name opens the library. While you drag a model or IR file over the window, every slot that will take it lights up.
* In the layout designer you can now set the colour of a control's value readout as well as its label (**Value Color** in the control's Style settings). With nothing selected, **Control Text** sets a label and value colour for every control in the layout, the default controls of a backdrop layout included; a control's own colour still wins, and its **Default** button puts it back to the layout's.

### Presets
* The app always starts with a preset loaded. If the preset you last played has been deleted or can't be loaded, it picks the default preset, or another one from your library, or a new preset, and you no longer see an empty "No Preset" signal chain. Previewing a tone from Tone Sharing and then closing the app now brings back the preset you were playing before the preview.

### Tone Sharing
* Right-click a folder in the preset chooser and pick **Create Pack…** to start a Tone Sharing pack from the folder's presets, its subfolders' included. The pack takes the folder's name, and each preset shows whether it is already on Tone Sharing. Presets that aren't yet are uploaded as drafts when you save, and only go to the moderators when you publish the pack. Presets you installed from someone else's pack are left out.
* Imported Tone3000 models now remember their licence and what the capture covers (amp only, full rig, pedal), and shared presets name models from any source, not only Tone3000.
* Previewing a tone now sounds like the tone. A tone built on Tone3000 models or IRs you didn't already have played without them in a preview, so it often sounded clean until you installed it. The preview now fetches what it needs and plays it, its blends included, and still adds nothing to your library: only installing the tone does that. If it can't get a model (without a Tone3000 key, for example), it tells you which ones it left out.

### Practice Tool
* Saved projects can now keep links: the tab for the song, a lesson video, the lyrics. Pick a project and paste an address into the new **Links** section under the loops, with a title if you like. Links are kept with the project as soon as you add them, and clicking one opens it in your web browser.

### Settings
* A new **Full Level on Mono Outputs** switch in Settings → General → Advanced DSP Level Targets, off by default. A preset at the default centre pan comes out 3 dB below its input, which keeps its loudness even as it is panned across a stereo output. A mono output, such as a mono DAW track or a single interface output, has nothing to pan across, and with the switch on a preset comes through there at its input level. Stereo outputs are unchanged. In a DAW it applies to plugin instances you add after turning it on, and projects you have already saved keep the level they were made with.
* Input Calibration is now called **Input Levelling**. It works as before, and the profiles you have trained are kept: open it from the chevron under the input meter.
* A new **Mouse wheel adjusts controls** switch in Settings → General → UI. Turn it off and the scroll wheel stops changing knobs, sliders and the metronome tempo as the pointer passes over them, and just scrolls the page.
* On a large display the footer's Setlists, Tuner, MIDI and Rec buttons are labelled, not just icons. A smaller window, or a higher UI zoom, keeps the icons alone.

### Custom Effects
* A Custom Effect is a small program, so for now shared files don't carry one. Preset archives, Tone Sharing items and packs install only NAM models and IRs. A Custom Effect in a shared preset arrives in place but empty, passing your sound through until you choose a module of your own. Until sharing modules is supported, share the `.wasm` file itself.
* Custom Effects run only while **Custom Effects** is on in Settings → Features → Power Features. Before, that switch only hid them, and a preset or DAW project that used one still ran it. A DAW project can't turn the switch on either.
* A Custom Effect that hangs or goes wrong can no longer freeze your audio, the app or your DAW. Each one has a time limit, a memory limit and a stack limit; one that goes past them is stopped and passes your sound through dry. Modules on a network share are not opened.
* A Custom Effect's category and picture are now always shown as plain text and an image stored inside the module. Before, the category could add elements of its own to the Add Effect menu, and the picture could be a web address the app fetched each time it showed it.
* Custom Effects run on Wasmtime 48.0.6, a long-term-support release that will keep getting security fixes, up from 43.0.1. Each build checks the runtime download against its known checksum.

### Performance
* Amps and cabs are much lighter on your CPU than in 1.6.0. With a mono guitar at 48 kHz and a 64-sample buffer on Windows, a NAM amp uses about a third of the CPU it did, the IR Cab about 30% and the Heavy American about a sixth. A preset with a NAM amp and an IR Cab uses about 40% of what it did.
* The IR Cab and IR Reverb use about half the CPU they did, on computers and Android phones alike, from short cab IRs to 10-second reverbs, and sound the same. Their heaviest buffers are lighter still, so they are less likely to cause a crackle at small buffer sizes.
* Soundshed Guitar now stops processing when nothing is being played, such as on a muted DAW track, an empty one, or with your guitar's volume rolled off, and uses next to no CPU until you play again. It waits for delay repeats and reverb tails to die away first, and the first note you play comes through in full.
* A new **Use Multiple CPU Cores** switch in Settings → Performance. It is off by default in the plugin: your DAW already spreads its tracks across your cores, and the extra threads competed with it, which could cause crackles and dropouts. It stays on in the standalone app. The standalone app and the plugin keep separate settings, and the plugin's applies to every instance.
* When multiple cores are in use, the extra threads now get the same high priority as the audio thread. Before, a busy computer, or a DAW minimised on macOS, could hold them up, which used more CPU and could cause crackles.
* A plugin instance whose window has never been opened no longer measures its levels for meters nobody can see.
* On a stereo track, Soundshed Guitar now notices when both sides carry the same mono guitar, as they do when a guitar input is recorded or monitored on a stereo track, and runs each amp model and cab once instead of twice. That about halves its CPU on such tracks, and the sound is unchanged. It takes a second of playing to notice. A real stereo source is never treated this way, and if a track starts carrying stereo, the right side comes back within a fifth of a second.
* NAM amps run on a new, faster engine. On Windows, Linux and Intel Macs they use about a third of the CPU they did, standard and A2 models alike, and on Apple silicon Macs about half, though A2 models at the lowest NAM quality (Settings → DSP Performance) save only about 15% there. Windows on ARM and Android get the same engine. They sound the same. Going back to a model the app has already loaded, as when you switch between presets, is quicker too: the model is ready to play in under half a millisecond instead of 15 to 25. The new engine is the **Fast NAM Engine** switch in Settings → DSP Performance, on by default. Turn it off to compare it with the standard NAM engine, or if a model ever sounds different on it. The presets you are playing switch over straight away, and in a DAW the switch applies to every instance.
* On Android phones with fast and slow cores, the audio now runs on the fast ones. Android often left it on the slow cores, where everything costs several times as much: on a Galaxy S21 a preset with a NAM amp and an IR reverb went from about 77% DSP load to about 25%, and its peaks, which cause crackles, from nearly twice the time available to about 60% of it. Phones that support Android's performance hints are given them instead, and choose for themselves.
* NAM amps cost about a third less to resample when your DAW or interface runs at a different sample rate from the capture, such as 44.1 kHz with a 48 kHz capture, and they add almost 1 ms less latency there (27 samples instead of 66 at 44.1 kHz).
* On Linux and on Intel Macs, NAM amps now use the fused multiply-add instructions every AVX2 processor has, which makes them a little faster.
* The **Heavy American** amp uses about a sixth of the CPU it did in 1.6.0, and a seventh at high gain, and sounds the same.

### Fixes
* The Room, Chamber, Advanced, Spring and Ambient reverbs no longer go silent until you reload the preset when an effect before them glitches and sends one invalid sample. They now ignore it and carry on. Nothing else about how they sound has changed.
* The noise gate shuts promptly again when you stop playing. Since 1.6.0 it stayed fully open for about a quarter of a second after the last note, before its Hold and Release even started. In front of a high-gain amp that was a burst of hiss every time you stopped, most noticeable at the end of a demo clip. It now starts closing about 40 ms after the playing stops, plus your Hold time. Low notes still hold it open, down to a seven-string's low B.
* Hiss on its own no longer holds the noise gate open. The gate now listens mostly to the range a guitar plays in, so to it the input's hiss is about 5 dB quieter while your playing is unchanged, and it closes when you stop even with the threshold set close to the noise floor.
* On Android the app now saves your rig each time it goes to the background, so nothing is lost when the system later closes it.
* NAM captures whose file says they are an amp with a pedal and a cab are now filed as full rigs rather than amps, and captures of studio gear as preamps. Models already in your library move on the next start.
* The IR Cab and IR Reverb no longer add any latency at all. In 1.6.0 they added at least 64 samples, and in most DAWs 256 to 2048 (5 to 43 ms at 48 kHz): a short IR added as much as the largest block your DAW said it might send, even when the DAW actually sent far smaller ones, and you heard all of it while playing live. That also fixes two things it caused: the IR Cab's Mix below 100% no longer sounds hollow and phasey, and your DAW no longer places the IR Reverb's dry signal slightly early. At small buffer sizes they now use less CPU, and at large ones a negligible amount more. Presets with an IR Cab sound the same, just sooner. An IR Reverb set below 100% Mix can sound slightly different, mostly in the lowest octave: its wet signal used to trail the dry one by an amount that changed with your buffer size, and now the two always line up.
* The IR Cab and IR Reverb no longer have a Low Latency switch: with no latency to save, it only saved a sliver of CPU on short IRs and cost up to several times more on long reverbs. A preset or DAW project that turned it off now plays with no latency too, and otherwise sounds the same, just 5 to 43 ms sooner; your DAW adjusts its delay compensation as it does for any latency change. An IR Reverb set below 100% Mix that had it off can sound slightly different in the lowest octave, as its dry and wet signals now line up.
* On Windows, closing a project or removing the last Soundshed Guitar from a track now stops the plugin's helper thread straight away. Before, it kept running until the DAW unloaded the plugin, and stopping it at that point could hang the DAW.
* The tuner now reads a bass's low strings: the low E of a four-string, the low B of a five- or six-string, the low F# of a seven-string and an extended-range bass tuned down to low C#, down to a semitone under that. It showed nothing below about 45 Hz, or at best the note an octave up now and then.
* Bounces and exports no longer start with a gap. Since 1.6.0, when your DAW started a render, Soundshed Guitar switched its NAM amps to a higher-quality mode, and the switch held up its audio long enough to leave silence at the start of the file, about half a second in FL Studio. Renders now use the quality you play at, so a bounce sounds exactly like playback; to render at a higher quality, raise it in Settings → DSP Performance before you bounce. And if the plugin is busy during a render, say because you opened its window, the render now waits for it instead of getting silence.
* Soundshed Guitar now waits until your DAW has finished loading its settings before telling the DAW its latency has changed. Telling it in the middle of the load, as it did before, can upset a DAW that answers by restarting the plugin there, and may be behind FL Studio's "plugin crashed while loading its settings".
* On Windows, NAM models and IRs whose full path is 260 characters or longer now load. Before, the app reported them as missing ("Model file not found"), which a deep folder of captures can reach.
* Model and IR files installed from older preset packs or shared tones no longer carry their 64-character file code twice in their names, which pushed some of them past that limit. Files already installed that way are renamed on the next start.
* Deleting Soundshed Guitar from a track's inserts and inserting it again no longer crashes your DAW. Since 1.6.0, once every Soundshed Guitar in the DAW had gone, removed or closed with its project, opening the window of the next one crashed DAWs that keep the plugin loaded, such as Studio One.
* An IR Cab with an IR in the IR B slot and none in IR A now plays IR B. Before, it let your guitar through with no cab at all. One IR on its own now plays in full whichever slot it is in, whatever IR Blend and L/R Split are set to, as an IR A on its own always has, with its own slot's Level, Pan, Invert and mic settings. A preset saved with only IR B now has its cab. Loading or clearing a cab's second IR also fades smoothly now, where with IR Blend above 0 the level used to jump.
* A2 captures no longer show as A1 in the library, and calibrated captures no longer show as uncalibrated. An A2 file holds several smaller models, each with details of its own, and trainers write a file's contents in different orders, so the library often read one of the inner models' details, or stopped reading before the file's own: about a quarter of A2 captures showed as A1, and some captures with input and output levels showed none and had no Calibrated chip. They always played correctly. Models already in your library are read again once on the next start.
* On Windows, models, IRs and riffs in folders whose names have letters outside plain English, such as "Müzik" or "Şarkılar", now work throughout. Picking a model or IR from such a folder in the resource browser's Folders tab used to leave the effect without it, a model or IR you added to the library from such a folder showed as missing after you restarted (and presets using it could not load it), and the takes in a riff library kept in such a folder would not open.
* Changing the IR Cab's **Quality** now changes what you hear straight away. Before, a cab with an IR loaded kept playing at its old Quality until you loaded another IR, changed Normalize or Trim Leading Silence, or your audio settings changed, so the setting seemed to do nothing. It now crossfades to the new Quality in a few milliseconds, whether you change it on the panel, from MIDI or with DAW automation.
* Changing the IR Cab's IR while you play no longer makes a tick. The crossfade to the new IR started with the old one silent for a moment, so the sound dropped out for about a millisecond and stayed a little quiet for about ten. The old IR now plays on through the fade exactly as it was.
* The IR Cab's **Auto Gain Comp** now keeps two blended IRs at the level of either one alone. It used to treat every pair as if the two IRs had nothing in common. Two mics on one speaker have a lot in common, so blending them came out louder than either, by up to 3 dB with IR Blend in the middle. It now measures how alike the two IRs are, at whatever IR B Offset you set, and also gets the level right with Normalize off. A preset that blends two similar IRs with Auto Gain Comp on now plays quieter, by up to 3 dB.
* A stereo IR at a Quality below Full no longer has one side cut off short. Each side used to be shortened separately, and then both were cut to the shorter length, so a side with a longer tail stopped abruptly.
* Switching the IR Cab off and back on no longer brings back Speaker Drive's compression from before. It used to hold down the first second of your playing after a loud passage.
* **Speaker Drive** on the IR Cab and Simple Cabinet now compresses the way a pushed speaker does. Playing at a normal level is left nearly alone, and louder playing is squashed more, up to 6 dB. The squash comes on over a fraction of a second and wears off within about a second once you play quieter. In 1.6.0, from about halfway up the knob, it took the full 6 dB off almost anything you played and held it for seconds after a loud passage, so it worked more like a volume cut. A preset that uses Speaker Drive now plays louder at normal levels: up to about 5 dB at full Drive.
* Synth Voice plays the right note high up the neck with Octave +1 or +2. At Octave +2 every note from about C5 (the 8th fret on the high E) up played the same B6, and at Octave +1 from about B5. A Sine or Triangle voice also no longer clicks when you jump more than a fifth.
* Turning the Digital Delay's Time while you play now bends the repeats smoothly instead of crackling, and with Sync on, a tempo change in your DAW no longer makes the repeats crackle while it ramps. Its Ducking now recovers at the same speed at 96 and 192 kHz as at 48 kHz, where it was two and four times faster.
* Switching the Noise Gate from Gate to Swell while a note rings no longer cuts the note to silence: it carries on at its level, and the next note you pick swells in.
* Moving a split's branch level, pan or mute while you play no longer zippers or clicks, and changing a branch's delay no longer crackles. Unmuting a branch with a delay set now plays what you are playing, not a snatch of what you played before the mute.
* The Gain effect, a NAM amp's Input and Output, the Chorus and Flanger's Delay and Depth and the Doubler's Time now glide to a new setting instead of jumping, so moving them while you play, or from an expression pedal or DAW automation, no longer zippers or crackles. Bringing the Octave's Mix up from 0 while you play no longer clicks.
* A NAM amp running at a sample rate other than its model's, such as 44.1 or 96 kHz with a typical 48 kHz model, now reports its true latency. It was short by about 6 samples at 44.1 kHz and by 50 at 192 kHz (more with the linear-phase Quality filters), so a NAM amp's Mix below 100% sounded slightly hollow, a blend of 44.1 and 48 kHz models was out of line, and your DAW's delay compensation fell short by as much.
* A tempo-synced effect inside a custom effect, such as a delay or tremolo set to Sync, now follows your DAW's or the app's tempo. It always ran at 120 bpm.

## 1.6.0 (Oct 8, 2026)

A big update: nine new effects, fifteen classic drive pedals, a Practice Tool for learning songs, undo/redo and A/B for your signal chain, gapless preset switching with ringing tails, and right-click MIDI Learn on nearly every control.

### New Effects
* **Wah**: a real pedal wah for your expression pedal or MIDI controller, with 34 factory voicings covering classic, boutique and artist signature wahs. It can switch itself off when you rock back to the heel. Set Control to Auto Wah and your playing sweeps it instead, through any of the voicings. This replaces the old Auto-Wah effect: one already in your presets becomes a wah on Auto Wah with the same sweep and level.
* **Tape Echo**: warm, wobbly repeats with wow and flutter, up to three playback heads, and the pitch bend of a real tape machine when you change the delay time.
* **Analog Delay**: bucket-brigade repeats that get darker the longer the delay. Push the feedback high and both new delays run away into self-oscillation.
* **3D Spatial**: place your guitar anywhere around you on headphones, or set it moving with seven motion modes, optionally in time with the song.
* **Ring Modulator**: metallic, bell-like and robotic tones. Tracking mode follows the notes you play so the effect stays in tune with you, and an LFO sweep can sync to tempo.
* **Harmonizer**: up to four harmony voices. Pick a key and scale and each voice follows your playing a 3rd, 5th, 6th or octave away, major or minor as the key needs, or set fixed intervals that work on chords too. Each voice has its own level, pan, detune and delay, for twin leads, choirs or a double-tracked part, and the harmony comes in with its own pick instead of a wrong note.
* **Rotary**: a rotating speaker cabinet, horn and drum, heard through two mics in stereo. Switch between Slow and Fast (or Brake) from a footswitch and hear the rotors spin up and slow down at their own pace, the horn in about a second and the heavy drum over several. Drive the cabinet's amp for grit, and set how far apart and how close the mics are.
* **Vibe**: the throbbing, lopsided swirl of a photocell vibe, in Chorus or Vibrato mode. Four unequal phase stages swept by a lamp give it the pulse a plain phaser does not have, and the sweep can sync to tempo.
* **Guitar to MIDI**: play a virtual instrument from your guitar. Put it anywhere before a Plugin Host holding an instrument plugin, and the single notes you play are played on it, as hard as you picked them, with hammer-ons played legato and bends and vibrato followed. It plays one note at a time, so chords mostly don't come through, and a low note picked hard starts about 50 ms after the pick.

### Better Tone
* **Heavy American** amp: much more gain on tap, a new Character control running from vintage fuzz to tight modern high gain, and a steady volume as you turn up the gain or Power Drive. Sag now works with Power Drive to squeeze and roughen the notes you dig into, instead of just turning the volume down. Changing Preamp Stages while a note rings no longer clicks.
* **Overdrive, Distortion and Fuzz** now cover fifteen classic pedals, from the TS-808 and Klon to the RAT, Big Muff and Fuzz Face. They sound smoother at high gain, clean up when you play softly, and no longer jump in volume when you switch them on. Your existing presets stay as loud as they were.
* **Simple Cabinet**: five cabinet types, mic choice and placement, a second mic to blend in, speaker drive and a live response curve, plus 27 factory presets, twenty of them modelled on the most popular cabinets, from a Deluxe 1x12 and AC30 2x12 to Recto, Greenback and G12T-75 4x12s and an 8x10 bass cab. You can export it as an IR, or match it to one of your IRs. Existing presets sound the same.
* **IR Cabinet**: a new Alignment section lines up your two IRs in time, like moving two mics on one speaker. Auto Align finds where IR B matches IR A, flipping it if it lines up upside down, or you can drag, nudge or type an offset to set it off on purpose for a comb-filtered tone, with the combined response drawn as you go. With one IR loaded it shows that IR's response. Existing presets sound the same.
* **NAM**: models recorded at a different sample rate from your session (a 48 kHz model in a 44.1 kHz session, say) sound much cleaner. New quality settings (oversampling) live in Settings → DSP Performance, bounces and exports automatically render at higher quality, and standard models use about 9% less CPU.
* **NAM Blends**: the Blend knob now plays the right model, blends crossfade smoothly between models, they work in Multi-Rig mixes, and saving a blend is heard straight away.
* **Reverbs**: Room, Ambient and Spring sound smoother, Diffusion no longer acts as a volume boost, and Decay has a more usable range. At their default settings they are as wet as before; presets that turned Diffusion up on Advanced or Ambient Reverb come out drier, and ones that turned it down, wetter.
* **Room, Chamber, Advanced and Spring Reverb** no longer ring with a metallic tone when Mix is turned up. The spring's echoes now smear out like a real tank's instead of fluttering.
* **Reverb presets**: Room, Chamber, Advanced, Spring and Ambient Reverb each come with factory presets in their Presets menu, from Small Room and Surf Spring to Concert Hall, Ducked Lead and Infinite Wash. A newly added reverb still starts where it always did.
* **Compressors**: the VCA and Opto Compressors now compress both channels together, so they no longer shift a stereo image after a chorus or ping-pong delay; a new Stereo Link switch (like the noise gate's) turns that off. Turning Makeup or Mix (or choosing a preset) no longer clicks, and a glitch in the incoming audio can no longer leave them silent or stop them compressing until you reload.
* **Noise gate**: closes smoothly instead of clicking and keeps your settings through restarts. The global gate's full settings are now a click away in the control bar.
* **Transpose**: the global Transpose knob is now far tighter to play through. Picks come through about 5 ms late instead of 80, low bass notes stay in tune, and chords sound cleaner. Turning it to or away from 0 no longer clicks or plays a snatch of old audio. The Transpose effect gets the same engine as a new Low Latency option; ones already in your presets keep the engine they had.
* **Pitch Shift**: new Snap to Semitone switch, and a pedal range so heel to toe covers exactly the interval you want (0 to +7 semitones, say). It also responds a little faster and no longer plays a snatch of old audio when switched back on. Its Low Latency engine, the one for an expression pedal, now keeps chords clean and low notes in tune.
* **Graphic EQ**: starts flat, has a Reset button, and its band layouts now sit in the Presets dropdown alongside your own curves.
* **Synth Voice**: follows your playing more closely, reaching new notes up to twice as fast and tracking down to low F# on an eight-string. It also uses far less CPU, so it no longer crackles at small buffers, and its Square wave no longer thumps on every note when Pulse Width is set away from the middle.
* **Auto Arpeggiator**: each step's note now lands right on the beat instead of about 40 ms late, stays in tune on the low strings, and comes through with far less delay. After a chord change it plays the new chord rather than the old one, steps no longer click with the gate fully open, and it uses about a quarter of the CPU. The pitch trigger now fires at the pitch you set, not a semitone or two below it, and the Steps control shows the number you chose.
* **Tremolo** has three new modes: Harmonic, where the lows and highs pulse against each other for the swirl of an early-60s brown amp; Pan, an auto-panner that turns a mono guitar stereo; and Slicer, which chops your playing to one of eight rhythm patterns, in time with the song if you like.
* **Digital Delay** can play in reverse: each slice of your playing comes back backwards, swelling in behind the note. Two new presets, Reverse and Reverse Wash.
* **Ambient Reverb** adds Shimmer, which sends the tail back round an octave (or a fifth) higher each time so it climbs into a halo, and Freeze, which holds the reverb ringing under whatever you play next, ready for a footswitch. Three new shimmer presets.
* **Noise Gate** has a Swell mode: every note fades in, as if you rolled the volume knob up after each pick, for violin-like swells. Two new presets, Volume Swell and Slow Swell.
* **Overdrive** adds a Rangemaster-style treble booster: it pushes the treble into your amp while leaving the bass alone, for the classic cranked-amp lead sound.
* **Chorus and Flanger** now come out in stereo from a mono guitar input. Their two sides were being merged back into one at the output, or at the next amp, drive pedal or noise gate. An amp after them now processes both sides, so a NAM amp there uses about twice the CPU, even with the chorus or flanger switched off. Set the amp's Channels to Mono to save it.
* **Global Doubler** now actually widens a mono guitar. It was coming out the same on both sides, so it coloured the tone instead.
* **Composite effects** with a chorus, flanger or stereo delay inside now come out in stereo too, instead of being merged back into one side by the rest of the chain.
* **Stereo and Dual Mono input**: whether your rig runs in mono or stereo now follows your input instead of being guessed from the sound, and a stereo input keeps each side to itself unless an effect pans or mixes them. In a DAW the track decides; in the standalone app the new input mode dropdown under the IN knob, whose list draws each choice's routing, offers Mono In 1, Mono In 2, both inputs summed, or Stereo. **Dual Mono**, in both, runs a stereo input as two separate rigs through the same preset, for two guitars or both sides of a stereo source. A pan, spread or ping-pong is heard the moment you turn it up, and each effect has a new Channels menu that runs it in mono to save CPU. A NAM amp on a mono guitar now loads one model instead of two, and a mono output gets both sides mixed in instead of just the left.
* Long reverb IRs no longer crackle at small buffer sizes. IR cabs and reverbs now play at the same level at every sample rate, so at 88.2 or 96 kHz they may be a little quieter than before.

### Presets & Live Playing
* **Gapless preset switching**: presets crossfade instead of cutting, and delay and reverb tails keep ringing when you change preset or scene. You choose how long in Settings → General → Preset Switching. Switching is also about three times faster.
* **Undo/redo and A/B** for the signal chain (#43). Ctrl/Cmd+Z undoes, Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y redoes, and A/B flips between two versions of your chain.
* **Effect presets**: every effect has a Presets dropdown with factory settings, listed alphabetically, and your own saved ones.
* **More factory presets**: Digital Delay, Chorus, Flanger, Heavy American, the VCA and Opto Compressors, Noise Gate, Parametric EQ, Synth Voice, Auto Arpeggiator and Pitch Shift now come with their own, from Slapback and Dotted Eighth to CE-2, MXR 117, Tight Djent, Chicken Pickin', Mud Cut, Synth Bass, Octave Bounce and Dive Bomb. The compressor and amp presets are level-matched, so trying them doesn't jump in volume, and the gate's leave your threshold where you set it. A newly added effect still starts where it always did.
* Back/forward buttons beside the preset selector step through your recently loaded presets for quick comparisons.
* **Multi-Rig** is now on by default. Add presets to the mixer from any preset card, set the mix level with its own Master Out knob, and save whole mixes to your library. The output limiter switch has moved to Settings → General → Advanced DSP Level Targets and is now remembered.
* Drag presets onto setlist pads to assign them.
* Setlist pads, footswitches and MIDI switch presets reliably, and choosing the preset you're already editing no longer throws away your unsaved changes.
* Replacing an effect with one from a different category, such as a delay with a reverb, now works.
* The save and publish dialogs share one tag list, now with a `bass` tag (credit: diego).

### MIDI & Automation
* **Right-click MIDI Learn** on almost any control: effect knobs, the IN and OUT knobs, Input and Output gains, setlist pads and bank arrows. Clear Mapping removes it again.
* **MIDI Learn for this preset** lets one expression pedal do a different job in each preset: a wah in one, a volume pedal in another.
* Jump straight to Scene 1–4 from a footswitch or DAW automation.
* Setlist, bank and scene changes from MIDI or DAW automation now work with the plugin window closed.
* **Bank Select**: a MIDI controller that sends Bank Select (CC0 or CC32) then a Program Change now loads that preset from the setlist with that bank number. A Program Change mapped to a setlist preset, scene or bank arrow now works for every program number (programs 0–63 used to do nothing), as does a softly played note, and MIDI Learn on a footswitch that sends Bank Select picks up its Program Change.
* Keyboard shortcuts work without the MIDI panel open, and the spacebar always reaches your DAW's transport.
* MIDI and automation now sweep each effect parameter's full range. If you automated an effect parameter on a custom slot, it now plays back across that full range.
* Wide frequency knobs now turn on a musical scale, so the knob, an expression pedal and automation all sweep them evenly. This covers the Ring Modulator's Frequency, and Low Cut and High Cut on Digital Delay, IR Cab and Advanced Reverb. Automation you've already recorded on those cuts follows the new curve.
* POGO Midi controllers show the loaded preset's name on their display. You can turn this off in MIDI & Automation → Mappings.
* The plugin tells hosts it accepts MIDI, so routing MIDI to it is easier (credit: diego). In Logic it stays an audio effect, so your existing projects still find it, and Logic still can't send it MIDI from a track. Automation values are saved with your DAW project, and MIDI channels read 1–16 everywhere.

### Practice & Jam
* **Practice Tool** in the Jam panel: load a backing track (WAV, AIFF or MP3), slow it down or change its key independently, loop named sections, EQ the track, and save it all as a project (credit: AriKuorikoski, #39).
* **Metronome** rebuilt with odd and compound time signatures, accent patterns, subdivisions including triplets, sampled clicks and tap tempo (Space bar or TAP).
* Riff previews loop seamlessly and can be trimmed while they play.
* Favourite a Jam track straight from the player (credit: belowm, #48).

### Library & Tone3000
* Drag NAM models and IR files from your file manager into the app, or straight onto a NAM or IR Cab effect to load them.
* The resource browser remembers your place for each kind of resource, and double-clicking a result picks it.
* Next/previous now steps through Tone3000 search results, downloading each model only when you reach it. A tone's models are listed in natural order, and its artwork shows on the effect.
* Tone Sharing search now covers every community preset, with a new tag filter.
* Fixed a crash on files with non-ASCII characters in their names, such as an emdash.
* Fixed effects that use both an amp model and a cab IR, such as Supercharged Neural Amp, losing one when you pick the other.
* Removing an installed Tone Sharing pack, or cleaning up unused resources, now removes them for good: they no longer reappear after a restart with their files gone. Models and IRs still used in a preset's later scene, an effect preset, a composite or a custom effect are kept.
* Previewing a shared tone no longer downloads or saves anything, so a model it uses plays only once it is in your library.
* Deleting a model or IR from the resource library no longer deletes one that an effect preset, a composite, a custom effect, the global chain or an unsaved Multi-Rig rig still uses: the delete is refused and says what uses it, as it already did for presets and blends.

### Look & Feel
* **Compact layout** for small windows, chosen automatically. You can override it with Layout Density in Settings. A tall, narrow window (the app snapped to half your screen, say) shows the signal chain and the selected effect together, and on a phone the preset bar, controls, preset library, FX list and setlist pads all fit the screen.
* The signal chain can wrap onto more lines instead of scrolling sideways (the toggle in its top-right corner), and does so automatically on small screens.
* The Global, Parametric and Graphic EQs show a live spectrum of your signal behind the curve.
* A new OUT meter beside the OUT knob, and both meters now show a peak level in dB.
* The control bar reads in signal order: the global EQ button has moved to the right, before DOUBLER and OUT, since it acts on everything your rig puts out.
* Effect artwork fills its panel and appears on the signal chain.
* Choose standard controls or a custom layout for each effect, keyword or preset (Effect Layout Editor power feature).
* Rewritten Help panel, new icons and refreshed knobs.

### Settings
* Standalone app: a new Audio & MIDI tab in Settings replaces the old device dialog. It covers driver, devices, channels, sample rate and buffer size, with a test tone, input meter, latency and dropout count. Your input mute is remembered for each device pair.
* The plugin window reopens at the size you left it, and new instances open at a size that suits your display.
* Settings, presets and your library are stored more safely, and several plugin instances and the standalone app can run side by side without overwriting each other. Existing data carries over automatically.
* NAM quality and window size are saved with each plugin instance in your DAW project, and a plugin no longer overwrites your standalone-only settings.

### Performance
* **Release builds are now true release builds**: the framework underneath (JUCE, now 9.0.3) was compiled with its debug-only assertions and leak detectors switched on in every shipped build, including on the audio thread. They are off now.
* The audio engine does far less work: a simple chain now uses about a third of the CPU it did.
* The interface is quicker to update and does less background work while hidden.
* Presets that host other plugins switch faster: one with three Plugin Hosts now loads in about half a second instead of up to two.
* **Starts faster**: the app's window appears as soon as you open it rather than a second or two later, and on Windows the interface is ready about half a second sooner. The first start after a restart or an update gains more: the library no longer reads every NAM model file on each launch, and the interface's files are read in the background while your audio device opens.

### Requirements
* macOS: Soundshed Guitar now needs macOS 11 (Big Sur) or later, so Catalina (10.15) is no longer supported, and the installer checks for it. On Big Sur, keep Safari up to date, as the interface relies on its latest web engine.

### Fixes
* Opening a DAW project no longer marks it as changed, and the plugin no longer disrupts your DAW's undo and redo.
* Moving a knob from a MIDI controller, an expression pedal or DAW automation no longer marks the preset as unsaved.
* Fixed crashes when some DAWs save or open a project, or read parameters while mappings reload.
* Fixed a crash previewing demo audio on a DAW track with a mono input.
* Fixed rare crashes and audio glitches in Multi-Rig and in chains with parallel branches on multi-core computers.
* Multi-Rig: adding, moving or removing an effect in one rig, or loading a model or IR into it, no longer fades out the other rigs or throws away their unsaved changes, and switching between rigs keeps every knob, bypass and pedal change.
* Hosted plugins keep their settings through chain edits, scene switches, Multi-Rig and restarts (#23), get clean audio at every buffer size, and ones that used to load silently now work.
* CLAP: parameter changes made while playback is stopped now apply straight away.
* Windows: the CLAP plugin window fits on scaled displays (#38), the installer's checkboxes work (#41), and the app no longer leaves a new temporary folder behind on every launch.
* macOS: imported files are still available after a restart.
* Linux: dragging and reordering effects works (#27), and the app explains what to install if WebKit is missing (#21).
* Restoring the app after minimising it no longer reloads the whole interface: what you had open stays as it was, and a Jam track keeps playing.
* Fixed the effect dropdown at non-default zoom levels (#33) and non-ASCII text in the plugin UI (#13).
* The tuner gives steadier readings and shows high notes in the right octave.
* Plus many smaller fixes. The git history has the full list.

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
