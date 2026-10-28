# Linux kiosk (Raspberry Pi 5 first)

Boots a small Linux box straight into Soundshed Guitar, full screen, with no desktop: a
pedal-style device with a touch screen and a USB audio interface. **Draft, not yet run on
hardware.**

The app already does the kiosk part itself: `--kiosk` sizes the window to the display
with no title bar ([Main.cpp](../../juce/source/Main.cpp), the `--kiosk` flag). Everything
here is the boot setup around it.

## What's here

| Path | What it does |
| --- | --- |
| `install.sh` | Turns a running Pi OS Lite / Debian system (or an image chroot) into the kiosk |
| `packages.txt` | Runtime packages: cage, Xwayland, Xorg fallback, GTK, WebKitGTK, ALSA |
| `rootfs/etc/systemd/system/soundshed-kiosk.service` | Opens a logind session on tty7 as user `soundshed` and runs the session script; RT limits; restarts the app if it exits |
| `rootfs/usr/local/lib/soundshed-kiosk/session` | Starts `cage` (default) or a bare Xorg, then `soundshed-guitar --kiosk` |
| `rootfs/usr/local/lib/soundshed-kiosk/xsession` | Xorg client: screen blanking off, then the app |
| `rootfs/etc/default/soundshed-kiosk` | The settings: session type, app path and args, IRQ priority, WebKit knobs |
| `rootfs/usr/local/lib/soundshed-kiosk/tune-audio` + `soundshed-audio-tune.service` | At boot: `performance` governor, Wi-Fi power save off, USB controller IRQ threads to FIFO 90 |
| `rootfs/etc/security/limits.d/95-soundshed-audio.conf` | rtprio/memlock for the `audio` group, for running the app by hand |
| `build-image.sh` + `pi-gen/stage-soundshed/` | Builds a flashable Pi 5 image: pi-gen Lite (pinned tag) plus a stage that runs `install.sh` |

`install.sh` also adds `threadirqs usbcore.autosuspend=-1` to `cmdline.txt`.

## Why these choices

- **JUCE's Linux UI is X11 only** (`gtk+-x11-3.0`), and the WebView is WebKitGTK in a
  child process, embedded through XEmbed (`GtkPlug`). JUCE forces the GTK backend to x11
  itself. Under cage, all of this runs in Xwayland. If cage shows the WebView's plug as a
  separate full-screen window, or a blank one, set `SOUNDSHED_SESSION=xorg`: a bare X
  server with no window manager, where XEmbed works as it does on the desktop.
- **ALSA directly, no JACK or PipeWire.** The JUCE build has only the ALSA backend, and
  for one app on one interface that's also the shortest path. The app's own Settings
  panel picks the device and remembers it.
- **The kiosk user's home has to be writable.** Settings, presets and the cache are in
  `~/.config/Soundshed Guitar` (`FileSystem::ResolvePlatformRootDirectory`), and WebKit
  writes under `~/.local/share` and `~/.cache`.
- **The unit sets `LimitRTPRIO`/`LimitMEMLOCK` itself.** PAM limits can't be relied on
  for a service, and without them the audio thread's realtime request fails silently.

## Try it on a Pi (no image build)

1. Flash Raspberry Pi OS Lite (64-bit) with Raspberry Pi Imager, with SSH enabled.
2. Build arm64: `./build_linux.sh --arch arm64` (needs the cross toolchain) or take the
   `linux-arm64` CI artifact, and copy `linux-dist-arm64/` to the Pi.
3. On the Pi:

   ```bash
   sudo tools/linux-kiosk/install.sh --dist ~/linux-dist-arm64
   sudo reboot
   ```

4. Watch it with `journalctl -u soundshed-kiosk -f` over SSH. tty1 still has a login
   prompt (Ctrl+Alt+F1); the kiosk is on tty7.

To get a shell back on the device: `sudo systemctl disable --now soundshed-kiosk`.

## Build a Pi 5 image

On Linux or WSL2 with Docker, from a Linux filesystem (not `/mnt/c`):

```bash
SOUNDSHED_ADMIN_PASS='…' SOUNDSHED_ADMIN_PUBKEY="$(cat ~/.ssh/id_ed25519.pub)" \
  tools/linux-kiosk/build-image.sh --dist linux-dist-arm64
```

The image (`*-soundshed.img.xz`) lands in `~/.cache/soundshed-pi-gen/pi-gen/deploy/`. It's
Pi OS Lite at the pi-gen tag in `PI_GEN_REF` (currently `2026-10-06-raspios-trixie-arm64`),
with an `admin` user for SSH and tty1, and the kiosk enabled. The password only goes into
the work dir's pi-gen `config` (mode 600). It never goes into this repo.

Bump the pin deliberately: `git ls-remote --tags https://github.com/RPi-Distro/pi-gen.git | grep arm64`.

## First things to check on hardware

These decide whether the Pi 5 is the right board. Check them before the polish below.

1. **The WebView under cage.** Does the UI appear inside the window, and does touch reach
   it? If not, try `SOUNDSHED_SESSION=xorg`.
2. **DSP headroom.** Run a typical preset (NAM amp + IR cab + reverb) at 48 kHz / 64 and
   watch for xruns. The fused NAM NEON path is off, and NAM on ARM hasn't been measured
   on this board. An Intel N100 mini PC with the x64 build is the fallback.
3. **WebKitGTK cost.** Measure the UI's CPU while playing (the `/proc/<pid>/stat` of the
   GTK child process). Try the `WEBKIT_*` knobs in `/etc/default/soundshed-kiosk` if it
   draws wrongly or slowly.
4. **RT actually applied.** `ps -eLo pid,cls,rtprio,comm | grep -i -e soundshed -e irq/`
   should show FF/RR on the audio threads and FIFO 90 on the xhci IRQ threads.
   `uname -v` shows whether the kernel is `PREEMPT` or `PREEMPT_RT`. The stock kernel
   with `threadirqs` may be enough. A `PREEMPT_RT` build is the next step if it isn't.

## Not done yet

- **Power-cut safety.** Root is writable. A real device wants a read-only root (Pi OS
  overlayfs, `raspi-config nonint enable_overlayfs`) with `/home/soundshed` on its own
  writable partition. That also needs checking that settings writes survive a cut.
- **On-screen keyboard.** Preset names, Tone3000 search and sign-in need text input. cage
  and WebKitGTK with a virtual keyboard (e.g. `wvkbd`) are untested.
- **Wi-Fi setup without a desktop.** For now: `WPA_COUNTRY` at build time, then
  `nmcli` over Ethernet/SSH, or cloud-init's `network-config` on the boot partition
  (pi-gen enables cloud-init by default).
- **Updates.** Re-running `install.sh --dist` replaces the app. There's no update path on
  the device.
- **Shutdown.** The Pi 5's power button already asks systemd for a clean shutdown. A
  footswitch for this isn't wired up.
- **Display scaling** for small touch screens (`GDK_SCALE`, or the app's UI zoom).
