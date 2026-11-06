# Linux kiosk (Raspberry Pi 4)

Boots a small Linux box straight into Soundshed Guitar, full screen, with no desktop: a
pedal-style device with a touch screen and a USB audio interface. **Draft, not yet run on
hardware.**

It runs either UI, and the `--dist` folder you give it decides which:

| | Soundshed Guitar | Soundshed Nano Q |
| --- | --- | --- |
| UI | The web UI, in WebKitGTK | Native, drawn with Elements and Cairo on the CPU |
| Build | `build_linux.sh`, or a release zip | `build-nanoq.sh` |
| Starts on | cage | A bare X server |
| Needs | GTK and WebKitGTK | Cairo, no GTK or WebKit |
| Goes full screen with | `--kiosk` ([Main.cpp](../../juce/source/Main.cpp)) | `SOUNDSHED_KIOSK=1` ([the clap-wrapper patch](../../nanoq/patches/clap-wrapper-x11-standalone.patch)) |

Everything here is the boot setup around the app. Nano Q is the one to try on a Pi 4 first,
since it should leave more of the CPU for DSP, but it does less on Linux so far: see
[Nano Q on Linux](#nano-q-on-linux).

## What's here

| Path | What it does |
| --- | --- |
| `install.sh` | Turns a running Pi OS Lite / Debian system (or an image chroot) into the kiosk |
| `build-nanoq.sh` | Builds Nano Q in a Debian trixie container, cross-compiling for arm64 |
| `packages.txt`, `packages-web.txt`, `packages-nanoq.txt` | Runtime packages: what both UIs need (cage, Xwayland, Xorg, ALSA), then each UI's own |
| `rootfs/etc/systemd/system/soundshed-kiosk.service` | Opens a logind session on tty7 as user `soundshed` and runs the session script; RT limits; restarts the app if it exits |
| `rootfs/usr/local/lib/soundshed-kiosk/session` | Starts `cage` or a bare Xorg, then the app, full screen |
| `rootfs/usr/local/lib/soundshed-kiosk/xsession` | Xorg client: screen blanking off, then the app |
| `rootfs/etc/default/soundshed-kiosk` | The settings: session type, app path and args, IRQ priority, WebKit knobs |
| `rootfs/usr/local/lib/soundshed-kiosk/tune-audio` + `soundshed-audio-tune.service` | At boot: `performance` governor, Wi-Fi power save off, USB controller IRQ threads to FIFO 90 |
| `rootfs/etc/security/limits.d/95-soundshed-audio.conf` | rtprio/memlock for the `audio` group, for running the app by hand |
| `rootfs/etc/udev/rules.d/90-soundshed-usb-storage.rules` | Mounts a USB stick as it is plugged in, for Nano Q's file picker |
| `build-image.sh` + `pi-gen/stage-soundshed/` | Builds a flashable arm64 image for the Pi 4: pi-gen Lite (pinned tag) plus a stage that runs `install.sh` |

`install.sh` also adds `threadirqs usbcore.autosuspend=-1` to `cmdline.txt`.

## Why these choices

- **Nano Q starts on a bare X server.** With no WebView to embed there's nothing for a
  compositor to help with, and Xorg can hide the pointer. `SOUNDSHED_SESSION=cage` works
  too, under Xwayland.
- **JUCE's Linux UI is X11 only** (`gtk+-x11-3.0`), and the WebView is WebKitGTK in a
  child process, embedded through XEmbed (`GtkPlug`). JUCE forces the GTK backend to x11
  itself. Under cage, all of this runs in Xwayland. If cage shows the WebView's plug as a
  separate full-screen window, or a blank one, set `SOUNDSHED_SESSION=xorg`: a bare X
  server with no window manager, where XEmbed works as it does on the desktop.
- **ALSA directly, no JACK or PipeWire.** The JUCE build has only the ALSA backend, and
  for one app on one interface that's also the shortest path. The app's own Settings
  panel picks the device and remembers it. Nano Q opens ALSA through RtAudio, asking for
  two periods.
- **The kiosk user's home has to be writable.** Settings, presets and the cache are in
  `~/.config/Soundshed Guitar` (`FileSystem::ResolvePlatformRootDirectory`), and WebKit
  writes under `~/.local/share` and `~/.cache`.
- **The unit sets `LimitRTPRIO`/`LimitMEMLOCK` itself.** PAM limits can't be relied on
  for a service, and without them the audio thread's realtime request fails silently.

## Try it on a Pi (no image build)

1. Flash Raspberry Pi OS Lite (64-bit) with Raspberry Pi Imager, with SSH enabled.
2. Get an arm64 build and copy it to the Pi. For Nano Q, run
   `tools/linux-kiosk/build-nanoq.sh`, which leaves `~/nanoq-dist-arm64`. For Soundshed
   Guitar, either run `./build_linux.sh --arch arm64`
   (needs the cross toolchain), which leaves `linux-dist-arm64/`, or unzip
   `SoundshedGuitar-<version>-Linux-arm64.zip` from
   `https://downloads.soundshed.com/downloads/<version>/`. A `linux-arm64-zip` CI artifact
   holds the same zip while it lasts. The zip has `opt/` and `usr/` at its root, so unzip
   it into a folder of its own and point `--dist` there.
3. On the Pi:

   ```bash
   sudo tools/linux-kiosk/install.sh --dist ~/linux-dist-arm64
   sudo reboot
   ```

4. Watch it with `journalctl -u soundshed-kiosk -f` over SSH. tty1 still has a login
   prompt (Ctrl+Alt+F1); the kiosk is on tty7.

To get a shell back on the device: `sudo systemctl disable --now soundshed-kiosk`.

## Build a Pi 4 image

For Nano Q, build the app first, on Linux or WSL2 with Docker. It cross-compiles, so it
needs no Pi:

```bash
tools/linux-kiosk/build-nanoq.sh
```

Then build the image from either UI's distribution folder, also on Linux or WSL2 with Docker. Keep the `--dist` folder and the work dir on a Linux
filesystem (not `/mnt/c`); the script itself can run from either. Unzipping a release zip
needs `unzip` in WSL, and if it drops the executable bit, `chmod +x` the app binary.

```bash
SOUNDSHED_ADMIN_PASS='…' SOUNDSHED_ADMIN_PUBKEY="$(cat ~/.ssh/id_ed25519.pub)" \
  tools/linux-kiosk/build-image.sh --dist ~/nanoq-dist-arm64
```

Leave out `SOUNDSHED_ADMIN_PUBKEY` to log in over SSH with the password instead.

pi-gen's builder image starts from `debian:trixie`. If your local copy is arm64, run
`docker pull --platform linux/amd64 debian:trixie` first. A failed build leaves the
`pigen_work` container behind, so run `docker rm -v pigen_work` before trying again.

The image (`*-soundshed.img.xz`) lands in `~/.cache/soundshed-pi-gen/pi-gen/deploy/`. It's
Pi OS Lite at the pi-gen tag in `PI_GEN_REF` (currently `2026-10-06-raspios-trixie-arm64`),
with an `admin` user for SSH and tty1, and the kiosk enabled. The password only goes into
the work dir's pi-gen `config` (mode 600). It never goes into this repo.

Bump the pin deliberately: `git ls-remote --tags https://github.com/RPi-Distro/pi-gen.git | grep arm64`.

## Flash and first boot

1. In Raspberry Pi Imager choose **Raspberry Pi 4**, then **Use custom** and the
   `.img.xz`, then the SD card (16 GB or larger). Check the size: an external USB drive
   can be listed beside it. Skip any customisation, because the image already has the
   user, SSH and hostname.
2. Connect the screen to HDMI0 (the micro-HDMI port next to USB-C), the USB audio
   interface, and Ethernet for the first boot. Power it from a 5 V / 3 A supply, and fit
   a heatsink or fan.
3. The first boot resizes the root partition, then the kiosk starts. Log in with
   `ssh admin@soundshed.local`.

## First things to check on hardware

These decide whether the Pi 4 is the right board. Check them before the polish below.

1. **The UI on the real screen.** Nano Q: does the bare X server start, and does touch reach
   the UI? If not, try `SOUNDSHED_SESSION=cage`. Soundshed Guitar: does the WebView appear
   inside the window under cage? If not, try `SOUNDSHED_SESSION=xorg`.
2. **Audio.** Choose the USB interface in Settings, Audio & MIDI: ALSA's default device on
   a Pi is probably HDMI or the headphone jack, with no input. Then reboot and check that
   it stays chosen. Nano Q has never opened a real device on Linux.
3. **DSP headroom.** This is the biggest risk on a Pi 4. Run a typical preset (NAM amp +
   IR cab + reverb) at 48 kHz / 64 and read the block stats (see
   [Diagnosing a slow or crackling Pi](#diagnosing-a-slow-or-crackling-pi)), then try 128 and
   256. The fused NAM NEON path is on by default, but nothing has been measured on the Pi 4's
   Cortex-A72. An Intel N100 mini PC with the x64 build is the fallback.
4. **UI cost.** Measure the UI's CPU while playing. Nano Q draws on the CPU, so look at the
   app's main thread with `top -H`. For Soundshed Guitar, read the `/proc/<pid>/stat` of
   the GTK child process, and try the `WEBKIT_*` knobs in `/etc/default/soundshed-kiosk`
   if it draws wrongly or slowly.
5. **RT actually applied.** `sudo soundshed-diag` should show RR 80 on the audio thread, FF 80
   on its helpers (Nano Q) and FF 90 on the xhci IRQ threads.
   `uname -v` shows whether the kernel is `PREEMPT` or `PREEMPT_RT`. The stock kernel
   with `threadirqs` may be enough. A `PREEMPT_RT` build is the next step if it isn't.
6. **Throttling.** `tune-audio` holds the `performance` governor. After a long session,
   `vcgencmd get_throttled` should print `throttled=0x0`. Anything else means the board
   ran short of power or cooling, and that shows up as xruns.

## Diagnosing a slow or crackling Pi

To turn on the block and paint stats, uncomment `SOUNDSHED_NANOQ_AUDIOSTATS` and
`SOUNDSHED_NANOQ_DRAWSTATS` in `/etc/default/soundshed-kiosk`, then restart the kiosk. With audio
playing, run:

```bash
sudo systemctl restart soundshed-kiosk
sudo soundshed-diag
```

The audio lines tell you which kind of problem it is:

| What the line shows | What it means | What to try |
| --- | --- | --- |
| `over` climbing, `load` high | The DSP doesn't fit in a block | A lighter preset or model, or a bigger buffer |
| `late` climbing, `load` low | The audio thread was held off | Check the priorities and `get_throttled` |
| `givenup` climbing | The DSP lock was held, so the block went out silent | Note what was happening; this one is a bug |
| All zero, still crackling | The stream, not the app | `hw_params`: both directions on one device, no second clock |

`SOUNDSHED_ALSA_PERIODS` trades latency for margin: a late block has more time to catch up at 4
than at 2. In the paint lines, `area` is how many whole windows are redrawn per second. Each one
costs a full CPU draw, so a high figure is the thing to bring down.

`SOUNDSHED_DISPLAY_MODE` holds the xorg session to 1920x1080, since a 4K screen means four times
the pixels to draw. Choose the USB interface for both input and output: each card has its own
clock, so an input on one and an output on another drift apart and crackle at any load, and
`soundshed-diag` warns about it.

## Nano Q on the GPU

`build-nanoq.sh --gpu` draws with Skia on the GPU, through EGL, instead of with Cairo on the
CPU. It has its own build tree and goes to `~/nanoq-dist-arm64-gpu`. Pass that folder to
`install.sh --dist` to try it, and the Cairo folder to go back. Nothing else differs.

It has only run on software GL (llvmpipe), never on a Pi's GPU, so which build is better on a
Pi 4 is still open. Compare them with the same preset and the stats above:

- `soundshed-diag` must show `V3D` as the renderer. `llvmpipe` means the GPU isn't reachable, so
  everything is drawn on the CPU, which is slower than Cairo.
- The first use of each kind of drawing compiles a shader, so a page can stutter the first time
  it's opened.
- The GPU shares memory with the CPU, so check the `late` and `over` audio counts as well as the
  UI. Text also looks slightly different from the Cairo build.

## Nano Q on Linux

Tested on 2026-11-05 without a Pi. The arm64 build installed with `install.sh` in an emulated
arm64 Debian trixie container, and drew its first preset at 1280x720 on Xvfb (standing in for
Xorg) and through the `session` script under cage with a headless output. The x64 build also
drew the compact layout at 800x480, and at idle used 1.8% of a core and 68 MB under Xvfb. None
of that says how a Pi 4 performs, and no real X server or audio device has been tried.

On Linux, for now:

- **No web views.** The Jam tab's YouTube player is Windows only.
- **No hosted plugins.** Nano Q doesn't host plugins, on any platform.

### Network

HTTP is libcurl ([HttpCurl.cpp](../../core/src/net/HttpCurl.cpp)), for the Tones page, Tone3000
and the update check. It reports timeouts, cancels and connection failures as the Windows
transport does. Nano Q turns it on with `GUITARFX_UICLIENT_CURL`, so the JUCE Linux build gains
no dependency, and Android stays on `HttpNone.cpp`. HTTPS needs `ca-certificates`, which
`packages-nanoq.txt` installs.

### Files

Linux has no file dialog to call, and a kiosk no desktop to show one on, so Nano Q draws its own:
a full-screen picker made for touch ([FilePicker.cpp](../../nanoq/src/ui/FilePicker.cpp)). It
offers Home, each disk mounted under `/media`, `/run/media` or `/mnt`, and the whole file
system. A stick shows up within a second of being mounted, and if it is pulled out while the
picker is showing it, the picker goes back to Home. Hidden files, and the `._` files macOS leaves
on a stick, aren't listed. The same picker opens, saves and chooses folders; Windows and macOS
keep their native dialogs.

To bring a model or an IR in from a stick: plug it in, open the slot's chooser, press **File...**,
tap the file, then **OK**. A file on a mounted disk is copied into the library
(`resources/content/local` under the settings folder), because elsewhere the library only keeps
a file's path, and the stick can then be taken away. **Add Folder...** on the Folder tab works too:
a tap there plays the file from the stick, and **OK** copies it in. **Export preset...** saves to
the stick.

`90-soundshed-usb-storage.rules` does the mounting, at `/run/media/system/<label>` (spaces in a
label become underscores). It's an automount with a one-second idle timeout, so that a stick can
just be pulled out. FAT and exFAT sticks are given to the kiosk user, who can then write to them;
any other file system keeps its own owners.

Tested on 2026-11-05 at 800x480 under Xvfb, x64, with a tmpfs standing in for the stick at the
same path: models and IRs imported, the model still there with the stick gone and the app
restarted, folders, saving, replacing, a stick arriving and leaving with the picker open, and a
Tone3000 download. Not tested: the udev rule itself. `systemd-mount` accepts its options, but a
container gets no device units, so nothing has been mounted by it, and no real stick has been
tried. If a stick doesn't appear, `journalctl -u 'run-media-*'` on the device is the place to look.

### Typing

Nano Q draws its own on-screen keyboard ([OnScreenKeyboard.h](../../nanoq/src/ui/OnScreenKeyboard.h)),
because a bare X server has no window manager to place a keyboard program beside the app. It comes up across the foot of the
screen when you tap a text field, type a value into a knob, or open a dialog that asks for text
(a name, say), and the page or dialog moves up when the field would otherwise be under it. A
chooser's search box waits for a tap, since the list is what you opened it for.

Enter works as the Enter key and puts the keyboard away. Hide does the same without entering, to
reach the buttons under it, and a tap on the field brings it back.

It follows `SOUNDSHED_KIOSK`, so desktop and DAW use have none. To turn it off on a device with
a real keyboard, set `SOUNDSHED_NANOQ_KEYBOARD=0` in `/etc/default/soundshed-kiosk`. Setting it to
`1` turns it on anywhere, which is handy for trying it on a desktop.

Tested on 2026-11-05 under Xvfb, x64, with a real X pointer and no X keyboard, at 800x480 and
1920x1080 (the panel stops growing at 1120 wide): a preset named and saved, Tone3000 searched, a
file named on Export preset, the sign-in prompt, a knob's value. Not tested: a real touch screen,
or how the keys feel under a finger on the Pi. There are no accented letters, and a negative
value needs a change of layer, as digits and `-` are on different ones.

### Building

The window, timer and ALSA behaviour comes from
[clap-wrapper-x11-standalone.patch](../../nanoq/patches/clap-wrapper-x11-standalone.patch), and
how frames reach the screen from
[elements-x11-present.patch](../../nanoq/patches/elements-x11-present.patch). FetchContent
doesn't reapply a changed patch to an existing tree, so run `build-nanoq.sh --clean` after
editing either. [elements-x11-skia.patch](../../nanoq/patches/elements-x11-skia.patch) does the
same job for the GPU build.

An arm64 build from `build-nanoq.sh` is tuned for the Cortex-A72 (`-mcpu=cortex-a72`). LTO stays
off: on Windows it made NAM slower, and nothing has measured it here.

## Not done yet

- **Power-cut safety.** Root is writable. A real device wants a read-only root (Pi OS
  overlayfs, `raspi-config nonint enable_overlayfs`) with `/home/soundshed` on its own
  writable partition. That also needs checking that settings writes survive a cut.
- **On-screen keyboard for Soundshed Guitar.** Nano Q has its own, but the web UI has none.
  cage and WebKitGTK with a virtual keyboard (e.g. `wvkbd`) are untested.
- **Wi-Fi setup without a desktop.** For now: `WPA_COUNTRY` at build time, then
  `nmcli` over Ethernet/SSH, or cloud-init's `network-config` on the boot partition
  (pi-gen enables cloud-init by default).
- **Updates.** Re-running `install.sh --dist` replaces the app. There's no update path on
  the device.
- **Shutdown.** The Pi 4 has no power button, so for now it's `sudo poweroff` over SSH.
  A footswitch or on-screen control for this isn't wired up.
- **Scrolling by touch.** Nano Q's lists scroll by dragging, and a tap that wanders under 10 px
  is still a tap. Not tried on a real touch screen: it's tested with X pointer events, which is
  what a finger becomes under Xorg.
- **Display scaling** for small touch screens (`GDK_SCALE`, or the app's UI zoom). Nano Q
  has its own zoom, and goes to its compact layout at 980 x 620 or below.
