# BiteDJ appliance — documentation

A Raspberry Pi 4 running [BiteDJ](https://github.com/TeamDeckshark/bitedj) as a
fixed-function DJ appliance: it powers on, and BiteDJ is on the screen. No desktop,
no login prompt, no keyboard required.

## Status

Built and running on boot, verified across four reboots on 2026-09-19.

| | |
|---|---|
| Host | `flx4` @ `10.0.0.190` — `ssh flx4` (also `bitedj`, `pi4`) |
| OS | Raspberry Pi OS / Debian 13 "trixie", kernel `6.18.50+rpt-rpi-v8`, aarch64 |
| Hardware | Pi 4, 4 GB RAM, 800×480 DSI touchscreen (`DSI-1`), WiFi only |
| Source | `~/bitedj`, build tree `~/bitedj/build` |
| Binary | `/usr/local/bin/mixxx` (symlink `bitedj`) |
| Upstream | Fork of Mixxx 2.5.6; `BITEDJ_VERSION 1.0` |

## Read these in order

1. **[architecture.md](architecture.md)** — what runs, and what starts what. Read this first;
   the rest assumes you know the boot chain.
2. **[build.md](build.md)** — compiling on the Pi. Which CMake flags, and why each one.
3. **[runtime.md](runtime.md)** — the system configuration that makes it an appliance
   rather than a program: real-time audio, automount, polkit, autologin.
4. **[audio.md](audio.md)** — getting sound out. The known-good device configuration,
   why PipeWire has to be kept off the controller, and Bluetooth.
5. **[desktop-access.md](desktop-access.md)** — the escape hatch. How to get from the
   fullscreen appliance to a real desktop, terminal and on-screen keyboard, with no
   keyboard attached.
6. **[networking.md](networking.md)** — SSH access and staging WiFi networks for venues
   you have not been to yet.
7. **[cross-compile.md](cross-compile.md)** — building in Docker on a Mac instead of
   on the Pi, and deploying the result over ssh.
8. **[fork-patches.md](fork-patches.md)** — the local changes carried on top of the
   BiteDJ fork, and why. Read before merging upstream.
9. **[stems.md](stems.md)** — plan for playing pre-separated vocal/instrumental
   stems, and why they get mixed before the timestretcher rather than after.
10. **[troubleshooting.md](troubleshooting.md)** — every failure hit during bring-up, with
    the actual diagnosis. Start here when something breaks.

## The short version of what bites people

- **The binary is `mixxx`.** The fork kept upstream's CMake target name, so nothing
  on disk is called `bitedj` except a convenience symlink. Config lives in `~/.mixxx`,
  resources in `/usr/local/share/mixxx`, and the Wayland `app_id` is `org.mixxx.Mixxx`.
  Only the version string is fork-branded.
- **Compiling the app is about half the work.** The runtime environment is the other
  half, and none of its failures show up until the box actually boots.
- **Check for `llvmpipe` before blaming the app for being slow.** See
  [troubleshooting.md](troubleshooting.md#software-rendering). This one is invisible
  until you look for it, and it silently costs you the GPU.
- **Silent controller? PipeWire is probably holding the card.** It claims the device at
  boot through a reservation protocol PortAudio cannot negotiate with, so the open fails
  with no useful error. See [audio.md](audio.md#pipewire-contention).
- **Steady, continuous audio underruns mean a bad channel layout, not a small buffer.**
  See [audio.md](audio.md#a-channel-assignment-the-device-cannot-satisfy).
- **You are not locked into the fullscreen app.** Three fingers swiped down gets you a
  desktop, terminal and on-screen keyboard, without interrupting playback. See
  [desktop-access.md](desktop-access.md).
- **The fork is patched.** One change, to show the PipeWire device in the audio
  picker. [fork-patches.md](fork-patches.md) says why and how to re-apply it.
- **Incremental builds take ~50 seconds**, not 2h20m. Only the first build is slow —
  which is worth weighing before reaching for the cross-compile setup in
  [cross-compile.md](cross-compile.md).
