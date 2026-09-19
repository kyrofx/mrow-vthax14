# Raspberry Pi

A Raspberry Pi 4 running [BiteDJ](https://github.com/TeamDeckshark/bitedj) as a
fixed-function DJ appliance: it powers on and BiteDJ is on the screen. No desktop, no
login prompt, no keyboard required.

**Documentation lives in [`bitedj_docs/`](bitedj_docs/) — start with
[`bitedj_docs/README.md`](bitedj_docs/README.md).** Everything in this directory is
covered there in more depth, including why each choice was made.

## Layout

- `bitedj_docs/` — full documentation: architecture, build, runtime, audio,
  networking, troubleshooting.
- `scripts/` — setup, build, and utility scripts. All are deployed to
  `~/.local/bin/` on the Pi except the three that are run directly:
  `build-bitedj.sh` and `install-runtime.sh` on the Pi, and `deploy.sh` from a
  workstation (see below).
- `config/` — configuration files, laid out mirroring their real paths on the Pi, so
  `config/etc/...` goes to `/etc/...` and `config/home/...` goes to `~/...`.
- `src/` — Raspberry Pi application and device code.
  - `src/buttons/` — the GPIO crowd buttons (Good / Mid / Bad) as a virtual
    MIDI port BiteDJ reads; see below.
- `tests/` — automated tests for Raspberry Pi code.

## Getting a box running

```bash
# 1. Build BiteDJ on the Pi. Takes ~2h20m; see bitedj_docs/build.md for the
#    CMake flags and why -j2 rather than -j4.
./scripts/build-bitedj.sh
sudo cmake --install ~/bitedj/build

# 2. Apply the appliance runtime: RT audio, governor, session, automount, polkit.
./scripts/install-runtime.sh

sudo systemctl reboot
```

`install-runtime.sh` is idempotent and never reboots on its own. It deliberately stops
short of two things, because both are choices rather than defaults: the **audio output
device** (see [`bitedj_docs/audio.md`](bitedj_docs/audio.md)) and **passwordless sudo**.

## The fork is patched

One local change on top of `TeamDeckshark/bitedj`, in the vendored copy at
`Mixxx/bitedj/`: the audio device picker hides logical ALSA PCMs, which also hid
`pipewire` — the appliance's only route to a Bluetooth speaker. See
[`bitedj_docs/fork-patches.md`](bitedj_docs/fork-patches.md) before merging upstream.

That enables the Rekordbox-style arrangement: **Master and Headphones on the
controller, Booth to a Bluetooth speaker**, so the room hears the music while you
still beatmatch on the deck's own cue. See
[`bitedj_docs/audio.md`](bitedj_docs/audio.md).

## Getting out of the appliance

BiteDJ boots fullscreen and owns the screen. **Three fingers swiped down** gives you a
desktop with a terminal, on-screen keyboard, file manager and `raspi-config`; three
fingers up returns to BiteDJ. Switching does not interrupt playback.

This needs no changes to the BiteDJ fork — it is sway config, waybar, and two small
daemons. See [`bitedj_docs/desktop-access.md`](bitedj_docs/desktop-access.md).

## Scripts

| Script | Runs | Purpose |
|---|---|---|
| `build-bitedj.sh` | directly | Builds BiteDJ with OOM recovery — retries on an OOM kill, drops to `-j1`, and stops on a genuine compile error rather than looping |
| `install-runtime.sh` | directly | Applies everything in `config/`, installs packages, enables units |
| `bitedj-session` | from sway | Supervises BiteDJ: restarts it on crash, gives up after 5 fast failures rather than spinning |
| `bitedj-screen` | gesture / key | Moves between the appliance and the debug desktop |
| `bitedj-gestures` | from sway | Runs `lisgd` — turns three-finger touchscreen swipes into workspace switches |
| `bitedj-osk` | button / key | Toggles the `wvkbd` on-screen keyboard |
| `bitedj-power` | button | Power menu; always confirms, so a stray tap cannot end a set |
| `waybar-usb` | from waybar | Custom module showing which removable drives are mounted |
| `add-wifi` | manually | Stores a WiFi network **without connecting to it**, for venues you have not visited yet |
| `bitedj-bt` | manually | Pairs a Bluetooth speaker; clears the rfkill block that otherwise makes every command fail with `NotReady` |

## Configuration

| File | Installs to | Purpose |
|---|---|---|
| `etc/security/limits.d/99-bitedj-audio.conf` | same | `rtprio`/`memlock`/`nice` for the `audio` group |
| `etc/systemd/system/cpu-performance.service` | same | Pins all cores to the `performance` governor |
| `etc/systemd/system/getty@tty1.service.d/autologin.conf` | same | Console autologin (written by `raspi-config`) |
| `etc/polkit-1/rules.d/50-bitedj.rules` | same | udisks2 + power-off without an unanswerable prompt |
| `etc/systemd/system/bluetooth-unblock.service` | same | Clears the persisted rfkill soft-block on Bluetooth at boot |
| `etc/sudoers.d/010-bitedj-nopasswd` | **optional** | Convenience only; read the warning in the file |
| `home/.config/sway/config` | `~/.config/sway/config` | The session: what starts, in what order |
| `home/.config/waybar/*` | `~/.config/waybar/` | Status bar |
| `home/.config/wireplumber/.../50-bitedj-reserve-controller.conf` | `~/.config/wireplumber/wireplumber.conf.d/` | Keeps PipeWire off the DJ controller's ALSA card |
| `home/.mixxx/soundconfig.xml` | `~/.mixxx/` | Known-good audio config for a DDJ-FLX4 |
| `home/profile-snippet.sh` | appended to `~/.profile` | Launches sway on VT 1 |

## Things that are host-specific

These are checked in as they ran on the reference box and will need editing elsewhere:

- **The WirePlumber rule names a DDJ-FLX4**, and one of its two match entries includes
  that unit's USB serial. A different controller needs the `api.alsa.card.name` changed.
  If PipeWire keeps the card, the controller is silent with no useful error.
- **`soundconfig.xml` hardcodes `hw:3,0`.** Card numbers are assigned in enumeration
  order — check `aplay -l` rather than assuming.
- **`autologin.conf` names the user.** `install-runtime.sh` substitutes the current user
  for the waybar path, but `raspi-config` regenerates this file for whoever runs it.

## Crowd buttons and the DJ harness

Two user services run beside BiteDJ on the appliance, both installed and
enabled by `scripts/install-runtime.sh`:

| Service | What it does |
| --- | --- |
| `mrow-harness` | The DJ harness (`../harness`): play history, crowd ratings and next-song suggestions, in `~/.mixxx/harness/`. Optionally refined by a cloud model. |
| `mrow-buttons` | Three GPIO buttons sent as MIDI notes on a virtual port BiteDJ maps to `[Harness],rate_good / rate_mid / rate_bad`. |

`bitedj-session` starts both before BiteDJ. The button service is ordered first
on purpose: BiteDJ enumerates MIDI devices once at startup, so a port that
appears later is not seen until a rescan.

### Wiring

Each button goes between its GPIO pin and ground; the pull-up is enabled in
software, so a press reads low. The defaults (BCM numbering):

| Button | GPIO | Physical pin | Note |
| --- | --- | --- | --- |
| Good | 17 | 11 | 0x3C |
| Mid | 27 | 13 | 0x3D |
| Bad | 22 | 15 | 0x3E |

Ground is on physical pins 6, 9, 14, 20, 25, 30, 34 or 39.

### Remapping

Two layers, neither needing a rebuild:

- **Rewiring a button** — `~/.config/mrow/buttons.toml` (pin and note per
  button, GPIO chip, MIDI channel, debounce). Check it without starting the
  service: `python3 ~/.local/share/mrow/buttons/mrow_buttons.py --check`.
- **Changing what a button does** — the Mixxx mapping
  `res/controllers/mrow-crowd-buttons.midi.xml` in the fork. Point a note at
  any `[Harness]` control, or add a fourth button for, say,
  `skip_suggestion_1`. A DJ controller's pads can bind to the same controls.

### Checking it

```sh
systemctl --user status mrow-buttons mrow-harness
journalctl --user -u mrow-buttons -f      # prints the button on each press
aseqdump -l | grep MROW                   # the virtual port exists
curl -s localhost:8765/health             # the harness is answering
```

If BiteDJ does not react, confirm the port name in `buttons.toml` still matches
the mapping's `<name>`: the fork pairs a device with a mapping by name, and
hides MIDI devices that have none.

## Deploying from a workstation

Building on the Pi takes over two hours. `scripts/deploy.sh` builds in the
project's Debian trixie arm64 container instead — the same release the
appliance runs — and copies the result over ssh:

```sh
RPI/scripts/deploy.sh                 # build and deploy everything to flx4
RPI/scripts/deploy.sh --dry-run       # print what it would do, change nothing
RPI/scripts/deploy.sh --no-build      # deploy what is already built
RPI/scripts/deploy.sh --only harness  # mixxx | harness | buttons | all
RPI/scripts/deploy.sh --host bitedj   # another device (ssh alias or user@host)
RPI/scripts/deploy.sh --restart       # restart BiteDJ afterwards
```

| What | Where it lands | Needs root |
| --- | --- | --- |
| Binary | `/usr/local/bin/mixxx` (and the `bitedj` symlink) | yes |
| Resources (skin, mappings, effects) | `/usr/local/share/mixxx` | yes |
| DJ harness | `~/.local/share/mrow/harness` | no |
| Crowd buttons | `~/.local/share/mrow/buttons` | no |
| Services, session script, `buttons.toml` | `~/.config`, `~/.local/bin` | no |

**ssh cannot write to `/usr/local`**, so everything is copied to
`~/.cache/mrow-deploy` on the device first and moved into place there with
`sudo` — which prompts for a password unless the device has passwordless sudo.
The new binary is renamed over the old one, and a rename is atomic, so a
running BiteDJ keeps the copy it started from and nothing is disturbed
mid-set.

`--user` installs the binary and resources under `~/.local` instead, needs no
sudo at all, and points `bitedj-session` at that binary. The app looks for its
resources at `../share/mixxx` relative to itself, so both layouts work.

Deploying does **not** restart BiteDJ: an update should not cut a set short.
The new build starts at the next launch, or restart it with
`ssh flx4 'pkill -x mixxx'` (`bitedj-session` relaunches it), or pass
`--restart`.

Local wiring is never clobbered: `buttons.toml` and `~/.config/mrow/harness.env`
are only created when missing. Afterwards the script prints the installed
version, checks `ldd` for missing libraries, validates the button config, and
restarts the two user services.
