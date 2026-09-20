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
  - `src/buttons/` — the five GPIO switches and Qwiic Twist as a virtual
    MIDI port BiteDJ reads; see below.
- `tests/` — automated tests for Raspberry Pi code.

## Building

Two routes, and the right one depends on the change:

| | Cold build | Incremental |
|---|---|---|
| On the Pi (`scripts/build-bitedj.sh`) | 2h20m | **~50s** |
| In Docker on an Apple Silicon Mac | minutes | minutes |

A one-line change is faster to iterate on over ssh than to cross-compile. A first
build, a clean rebuild, or a change to a widely-included header is not. See
[`bitedj_docs/cross-compile.md`](bitedj_docs/cross-compile.md) for the Mac setup and
[`scripts/deploy.sh`](scripts/deploy.sh) for shipping the result to the device.

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
| `bitedj-bt` | manually | Pairs a Bluetooth speaker from a shell; clears the rfkill block that otherwise makes every command fail with `NotReady` |
| `bitedj-bt-ui` | gesture / button | Touch picker for Bluetooth speakers. Draws over fullscreen BiteDJ via layer-shell, so a speaker can be connected mid-set. Confirms before connecting |
| `bitedj-cursor-park` | from sway | Parks the cursor in a corner after each touch, so a tap does not leave an invisible pointer hovering over the UI |
| `bitedj-volume` | waybar / `Super+V` | Touch volume slider for the default sink. waybar's own module can only mute, and a touchscreen has no scroll wheel |
| `bitedj-usb-recover` | boot service | Power-cycles USB ports that came up empty, so the controller is found without replugging it |

## Configuration

| File | Installs to | Purpose |
|---|---|---|
| `etc/security/limits.d/99-bitedj-audio.conf` | same | `rtprio`/`memlock`/`nice` for the `audio` group |
| `etc/systemd/system/cpu-performance.service` | same | Pins all cores to the `performance` governor |
| `etc/systemd/system/getty@tty1.service.d/autologin.conf` | same | Console autologin (written by `raspi-config`) |
| `etc/polkit-1/rules.d/50-bitedj.rules` | same | udisks2 + power-off without an unanswerable prompt |
| `etc/systemd/system/bluetooth-unblock.service` | same | Clears the persisted rfkill soft-block on Bluetooth at boot |
| `etc/systemd/system/bitedj-usb-recover.service` | same | Power-cycles dead USB ports at boot so the controller enumerates |
| `home/.config/wireplumber/.../51-bitedj-bluetooth-no-suspend.conf` | `~/.config/wireplumber/wireplumber.conf.d/` | Stops WirePlumber idle-suspending a Bluetooth sink and dropping the A2DP transport |
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

## Crowd buttons and the built-in DJ agent

Mixxx owns its embedded agent and starts it automatically over private process
pipes. No browser or localhost service is required. Python 3.9+ is the only agent
runtime dependency, installed by `scripts/install-runtime.sh`. The GPIO bridge
remains a separate service:

| Service | What it does |
| --- | --- |
| `mrow-buttons` | Five GPIO switches plus the I2C Twist, sent through one virtual MIDI port. |

`bitedj-session` starts the buttons before BiteDJ. The button service is ordered first
on purpose: BiteDJ enumerates MIDI devices once at startup, so a port that
appears later is not seen until a rescan.

### Wiring

Each button goes between its GPIO pin and ground; the pull-up is enabled in
software, so a press reads low. The defaults (BCM numbering):

| Switch | Action | BCM GPIO | Physical pin | MIDI note |
| --- | --- | --- | --- | --- |
| 1 | Load selected track into deck 1 | 17 | 11 | 0x40 |
| 2 | Load selected track into deck 2 | 27 | 13 | 0x41 |
| 3 | Good audience reaction | 22 | 15 | 0x3C |
| 4 | Med audience reaction (`rate_mid`) | 23 | 16 | 0x3D |
| 5 | Bad audience reaction | 24 | 18 | 0x3E |

All switches share ground. Idle is HIGH / 1; pressed is LOW / 0.
The service samples every 5 ms and requires a stable state for 20 ms for both
press and release. Holding a switch does not repeat. A switch held at service
startup must be released before it can trigger a load or rating.

On the verified appliance, the SparkFun Qwiic Twist shares the display's
**I2C bus 10** (3.3 V logic/power and common ground). A live scan found 0x3F
alongside the display's claimed 0x38/0x45 addresses. The shipped config uses
bus 10. Normal header SDA GPIO2 / SCL GPIO3 wiring uses bus 1 instead; set
`[twist].bus = 1` for that wiring. The daemon fallback with no config also uses
bus 1. No extra encoder GPIOs are needed. The default address is **0x3F**; the address jumper selects 0x3E.
The daemon checks device ID 0x5C before accepting input. It reads the count
without resetting it and handles signed 16-bit wraparound. The count is
little-endian and the current push level is status bit 1 in
[SparkFun's firmware](https://github.com/sparkfun/Qwiic_Twist/blob/master/Firmware/Qwiic_Twist/Qwiic_Twist.ino).
RGB settings are left at their existing values.

Twist behavior matches the unshifted FLX4 browse control in this fork:

- Rotate on Play: waveform zoom; rotate elsewhere: library selection.
- Press on Play: open Browse; press elsewhere: move library focus forward.
- Clockwise sends +1 and counterclockwise -1 per detent. Set `reverse = true`
  in `[twist]` if the direction should be reversed.

An absent or disconnected Twist logs an error and retries every two seconds;
the GPIO switches and virtual MIDI port remain available. Reconnection takes
a fresh count baseline so missed turns are not replayed.

Ground is on physical pins 6, 9, 14, 20, 25, 30, 34 or 39.

### Remapping

Two layers, neither needing a rebuild:

- **Rewiring a button** — `~/.config/mrow/buttons.toml` (pin and note per
  button, GPIO chip, MIDI channel, debounce). Check it without starting the
  service: `python3 ~/.local/share/mrow/buttons/mrow_buttons.py --check`.
- **Changing what a button does** — the Mixxx mapping
  `res/controllers/mrow-crowd-buttons.midi.xml` in the fork. Point a note at
  a Mixxx control. Browse behavior lives in `mrow-controls.js`. A DJ
  controller's pads can bind to the same controls.

### Existing Pi configuration

Install/deploy scripts preserve `~/.config/mrow/buttons.toml`. A Pi with the old
three-button configuration will keep its old wiring until you replace it.
For this five-switch hardware, run from the updated checkout on the Pi:

```sh
cp ~/.config/mrow/buttons.toml ~/.config/mrow/buttons.toml.before-key-bindings
cp RPI/config/home/.config/mrow/buttons.toml ~/.config/mrow/buttons.toml
```

Install the updated daemon **and** both controller files
(`mrow-crowd-buttons.midi.xml` and `mrow-controls.js`) into the active Mixxx
resource directory. A full `RPI/scripts/deploy.sh` includes both; `--only buttons`
updates only the Python bridge and is insufficient for the new mapping.
`install-runtime.sh` installs `python3-smbus2` and `i2c-tools` and adds the user to `i2c` as well as `gpio`. On an existing installation:

```sh
sudo apt-get install python3-smbus2 i2c-tools
sudo usermod -aG i2c,gpio "$USER"
# Only needed for header GPIO2/3 wiring if bus 1 is not enabled:
sudo raspi-config nonint do_i2c 0
```

Log out/reboot after changing groups so the user service inherits membership.
Before starting the updated service, run `sudo i2cdetect -y 10` (or `-y 1`
for header wiring) and check the display address and `3f`. A scan alone cannot distinguish two devices sharing an address:
if the display also uses 0x3F, change the Twist address (e.g. jumper to 0x3E)
and update `[twist].address` before running the bridge. No automatic address
changes or display reconfiguration are performed.

### Checking it

Goal: one action per press, correct browse direction, and uninterrupted display.
After installing the mapping, restart BiteDJ when it is safe to interrupt audio.
Then test switches 1–5 for deck 1 load, deck 2 load, Good, Med, Bad; hold and release
each switch to confirm no repeated action. Turn the Twist both ways in Browse,
press to change focus, and check the Play-screen zoom/open-Browse behavior.

Automated checks (no Pi hardware required):

```sh
python3 -m unittest discover -s RPI/tests -v
node RPI/tests/test_controls.js
```


```sh
systemctl --user status mrow-buttons
journalctl --user -u mrow-buttons -f      # prints the button on each press
aseqdump -l | grep MROW                   # the virtual port exists
```

If BiteDJ does not react, confirm the port name in `buttons.toml` still matches
the mapping's `<name>`: the fork pairs a device with a mapping by name, and
hides MIDI devices that have none.
Use **Assist → Models** to inspect the agent, enter a runtime key or change its
models. Optional [build/deploy provisioning](../harness/README.md#optional-builddeploy-provisioning)
loads the key and models automatically on boot without placing secrets in the binary.

## Deploying from a workstation

Building on the Pi takes over two hours. `scripts/deploy.sh` builds in the
project's Debian trixie arm64 container instead — the same release the
appliance runs — and copies the result over ssh:

```sh
RPI/scripts/deploy.sh                 # build and deploy everything to flx4
RPI/scripts/deploy.sh --dry-run       # print what it would do, change nothing
RPI/scripts/deploy.sh --no-build      # deploy what is already built
RPI/scripts/deploy.sh --only mixxx    # mixxx (includes agent) | buttons | all
RPI/scripts/deploy.sh --agent-config "$HOME/.config/mrow-build/agent.json"
RPI/scripts/deploy.sh --host bitedj   # another device (ssh alias or user@host)
RPI/scripts/deploy.sh --restart       # restart BiteDJ afterwards
```

| What | Where it lands | Needs root |
| --- | --- | --- |
| Binary | `/usr/local/bin/mixxx` (and the `bitedj` symlink) | yes |
| Resources (skin, mappings, effects) | `/usr/local/share/mixxx` | yes |
| Agent code | embedded in `/usr/local/bin/mixxx` | included above |
| Optional agent credentials | `~/.config/mrow/agent.json` (mode 600) | no |
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

Local wiring is never clobbered; credentials change only with `--agent-config`.
The script checks `ldd`, validates button configuration and restarts the button
service. The obsolete `mrow-harness` service is disabled, not deleted, and its
history is preserved. Old `harness.env` files are not read by the embedded agent.
