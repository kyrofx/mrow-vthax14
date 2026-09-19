# Architecture

## What BiteDJ is

A fork of Mixxx 2.5.6 by Deckshark (`github.com/TeamDeckshark/bitedj`, GPLv2), aimed at
an embedded touchscreen appliance with a USB-drive-centric library: no local music
library, no modal dialogs, no keyboard/mouse interaction model, a hard CPU budget.
Windows/macOS/iOS packaging is pruned; Linux + Qt6 only.

### Naming — the thing everyone gets wrong

The fork did **not** rename the CMake target. `project(mixxx VERSION 2.5.6)` and
`add_executable(mixxx ...)` are untouched, so:

| | |
|---|---|
| Installed binary | `/usr/local/bin/mixxx` (we added a `bitedj` symlink) |
| Config / data dir | `~/.mixxx`, `mixxx.cfg`, `mixxx.log` |
| Resource dir | `/usr/local/share/mixxx` |
| Wayland `app_id` | `org.mixxx.Mixxx` |
| Desktop file | `org.mixxx.Mixxx.desktop` |
| Skin | `BiteDJ` — the only one installed |

`BITEDJ_VERSION` (currently `1.0`) is a separate CMake variable from the Mixxx base
version, and moves independently. It is the product version shown in the UI.

Corollary: a startup warning reading `Failed to load default skin styles
/skins/default.qss!` is **benign**. There is no skin named `default`; the fork ships
only `BiteDJ`, and it loads correctly.

## Boot chain

Nothing here is a display manager. The box goes from firmware to DJ UI through a
console autologin, which is fewer moving parts and fails more legibly.

```
firmware
  └─ kernel (preempt=full)
      └─ systemd → multi-user.target        # NOT graphical.target; lightdm is disabled
          ├─ cpu-performance.service        # pins all 4 cores to the performance governor
          ├─ udisks2.service                # the mount backend
          ├─ polkit.service                 # loads /etc/polkit-1/rules.d/50-bitedj.rules
          └─ getty@tty1                     # --autologin flx4
              └─ login shell → ~/.profile
                  └─ exec sway              # only when XDG_VTNR=1
                      ├─ dbus-update-activation-environment   # must be first
                      ├─ waybar
                      ├─ udiskie --automount
                      ├─ polkit-mate-authentication-agent-1
                      ├─ ~/.local/bin/bitedj-gestures        # lisgd: touch escape hatch
                      └─ ~/.local/bin/bitedj-session
                          └─ /usr/local/bin/mixxx --fullScreen
```

### Why a console autologin instead of a display manager

`lightdm` is installed but disabled, and the default target is `multi-user.target`. The
appliance has one user, one session and one application; a greeter is a component that
can only fail. `~/.profile` guards on `XDG_VTNR = 1`, so **SSH logins still get an
ordinary shell** — you are never locked out by the session config.

### Why `dbus-update-activation-environment` must come first

`sway` is exec'd from a login shell, so it inherits `XDG_CURRENT_DESKTOP` and gets
`WAYLAND_DISPLAY` at startup. But `systemd --user` and the D-Bus activation environment
are separate and inherit nothing. `xdg-desktop-portal` picks its backend by reading
`XDG_CURRENT_DESKTOP` (the mapping lives in `/usr/share/xdg-desktop-portal/sway-portals.conf`),
so without this line every portal call blocks for 40 s and the caller gives up. That is
what was killing waybar. Anything that might touch a portal must be ordered after it.

### Why `waybar` is started by `exec`, not a `bar` block

A `bar { swaybar_command waybar }` block is launched by sway on its own schedule, which
races the `exec` list. Starting it with `exec` puts it deterministically after the
activation environment exists.

Note that waybar is currently **invisible in normal use** — BiteDJ is fullscreen on an
800×480 panel and covers it. It is kept running deliberately, to match the reference
pi-gen recipe and to be there if BiteDJ is ever run non-fullscreen. To make it visible
you would need to move it to the `overlay` layer, at a cost of 28 px off a 480 px screen.

## Two workspaces

The session is split so the appliance is not a trap:

| Workspace | Holds | Visible |
|---|---|---|
| 1 | BiteDJ, fullscreen | On boot, and on stage |
| 2 | waybar, terminal, on-screen keyboard, file manager | On demand |

A three-finger swipe moves between them. This matters more than it sounds: the box
has no keyboard, so without it a misconfigured appliance can only be fixed over SSH —
and at a venue there may be no network either.

Crucially, **switching does not interrupt playback**. The audio engine is independent
of whether its window is visible, so the desktop is reachable mid-set.

waybar deliberately stays *behind* fullscreen BiteDJ on workspace 1 rather than
floating above it, because at 800×480 an overlay bar would cover BiteDJ's own tab
strip. See [desktop-access.md](desktop-access.md).

## Components

| Component | Role | Why this one |
|---|---|---|
| `sway` 1.10.1 | Wayland compositor | wlroots-based, scriptable, no desktop baggage |
| `waybar` 0.12.0 | Status bar | Reference recipe parity. Currently occluded — see above |
| `udiskie` 2.5.7 | Automounter | `udisks2` is only a backend; something must call it |
| `udisks2` | Mount/eject backend | What the polkit rules and `udiskie` talk to |
| `polkit` + mate agent | Authorization | Power-off and eject need an agent that can answer |
| `pipewire` + `wireplumber` | Audio for everything *except* the DJ controller | Already the OS default |
| `bitedj-session` | Supervisor | Restarts BiteDJ on crash, with a give-up threshold |
| `lisgd` | Touch gestures | sway's `bindgesture` only sees touchpad gesture events, not touchscreen ones |
| `wvkbd` | On-screen keyboard | The box has no keyboard; layer-shell so it overlays without resizing |

## Removable media

`SystemSettings::removableRoots()` (`src/preferences/systemsettings.cpp`) enumerates:

```
/media   /run/media   /mnt   /media/$USER   /run/media/$USER
```

This is more forgiving than expected — it does not matter whether the automounter uses
the udisks2-native `/run/media/$USER/<label>` or the Debian-style `/media/$USER/<label>`.

It does read `getenv("USER")`, and if that is unset the two per-user roots collapse to
the bare `/media`, which looks like "the browser is empty" with no error. `bitedj-session`
pins `USER` defensively for that reason.

`enumerateUsbMounts()` deliberately does **not** parse `/proc/mounts` — it stats candidate
directories with `QStorageInfo`, because the GUI process can run in a mount namespace
where automounted drives are absent from the mount table but still fully usable.

## Audio device ownership

Audio is deliberately split between two owners, and the split is load-bearing:

| Device | Owner | Why |
|---|---|---|
| DJ controller (`hw:3,0`) | **BiteDJ, exclusively** | Direct ALSA gives low latency and the 4 channels needed for headphone cueing |
| Built-in, HDMI, Bluetooth | PipeWire | Normal desktop audio, nothing latency-critical |

PipeWire is prevented from touching the controller by a WirePlumber rule
(`~/.config/wireplumber/wireplumber.conf.d/50-bitedj-reserve-controller.conf`). Without
it, PipeWire claims the card at boot and holds it through D-Bus device reservation, a
protocol PortAudio cannot negotiate with — so BiteDJ's open fails silently and the
controller never makes a sound. See [audio.md](audio.md).

## Real-time audio

The engine pins CPUs and runs its main thread at real-time priority. Verified live:
`SCHED_FIFO` priority 49 with ~3.2 GB locked via `mlockall`.

This is why the GPU matters more than it looks. When Mesa falls back to `llvmpipe`,
the software rasterizer threads are spawned **inside the BiteDJ process** and therefore
inherit `SCHED_FIFO 49` — software rendering then competes with the audio callback at
real-time priority on a 4-core box. See
[troubleshooting.md](troubleshooting.md#software-rendering).

## Graphics

The Pi 4 exposes two DRM devices, and the split is the source of a subtle trap:

| Node | Driver | Capability |
|---|---|---|
| `card0` | `v3d` | render only |
| `card1` | `vc4-drm` | display only (DSI-1, HDMI-A-1, HDMI-A-2) |
| `renderD128` | `v3d` | render node — **this is the one clients need** |

wlroots defaults to advertising the display card to clients. That card cannot render,
so Mesa silently falls back to `llvmpipe`. `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128`
in `~/.profile` fixes it. With it set, clients get `V3D 4.2.14.0` / OpenGL 3.1.
