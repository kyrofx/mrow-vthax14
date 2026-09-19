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
  `~/.local/bin/` on the Pi except the two that are run directly.
- `config/` — configuration files, laid out mirroring their real paths on the Pi, so
  `config/etc/...` goes to `/etc/...` and `config/home/...` goes to `~/...`.
- `src/` — Raspberry Pi application and device code.
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

## Scripts

| Script | Runs | Purpose |
|---|---|---|
| `build-bitedj.sh` | directly | Builds BiteDJ with OOM recovery — retries on an OOM kill, drops to `-j1`, and stops on a genuine compile error rather than looping |
| `install-runtime.sh` | directly | Applies everything in `config/`, installs packages, enables units |
| `bitedj-session` | from sway | Supervises BiteDJ: restarts it on crash, gives up after 5 fast failures rather than spinning |
| `waybar-usb` | from waybar | Custom module showing which removable drives are mounted |
| `add-wifi` | manually | Stores a WiFi network **without connecting to it**, for venues you have not visited yet |

## Configuration

| File | Installs to | Purpose |
|---|---|---|
| `etc/security/limits.d/99-bitedj-audio.conf` | same | `rtprio`/`memlock`/`nice` for the `audio` group |
| `etc/systemd/system/cpu-performance.service` | same | Pins all cores to the `performance` governor |
| `etc/systemd/system/getty@tty1.service.d/autologin.conf` | same | Console autologin (written by `raspi-config`) |
| `etc/polkit-1/rules.d/50-bitedj.rules` | same | udisks2 + power-off without an unanswerable prompt |
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
