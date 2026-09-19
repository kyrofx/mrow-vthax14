# Getting out of the appliance

BiteDJ boots fullscreen and owns the screen. That is correct for a device on
stage, and a problem the first time something needs fixing at a venue with no
keyboard. This is the escape hatch.

**Nothing here modifies the BiteDJ fork.** It is entirely sway configuration,
waybar, and two small daemons.

## How to use it

| | |
|---|---|
| **Three fingers, swipe down** | Debug desktop |
| **Three fingers, swipe up** | Back to BiteDJ |
| `Super+Escape` | Toggle (keyboard) |
| `Super+Return` | Terminal on the desktop |
| `Super+K` | On-screen keyboard |
| `Super+1` / `Super+2` | Go to BiteDJ / desktop |

**Switching does not interrupt playback.** The audio engine runs independently of
whether its window is visible, so you can drop to the desktop mid-set and the
music keeps playing. This was verified: switching back and forth repeatedly
produced zero audio underruns.

## What is on the desktop

Workspace 2 is an ordinary sway desktop. waybar is visible there (on workspace 1
it sits behind fullscreen BiteDJ, deliberately, so nothing covers the DJ UI's own
tab strip).

The bar is left-to-right: workspace switcher (`DJ` / `DESK`), then launchers.

| Button | Does |
|---|---|
| `DJ` / `DESK` | Switch workspace — the fallback if gestures stop working |
| `TERM` | Terminal (also spawns one automatically when you arrive) |
| `KBD` | Toggle the on-screen keyboard |
| `FILES` | `pcmanfm` |
| `CONF` | `raspi-config` in a terminal |
| `PWR` | Restart BiteDJ / reboot / shut down — always confirms first |

Arriving on an empty workspace with nothing but a bar is a dead end on a
touch-only device, so `bitedj-screen` guarantees a terminal is always there.

## Why it is built this way

### lisgd rather than sway's `bindgesture`

sway *has* gesture bindings, and they will not work here. `bindgesture` binds
libinput **gesture** events, which touchpads emit and touchscreens do not —
touchscreens emit raw touch events, and something has to synthesise gestures from
them. That is what `lisgd` does, and it is why it exists.

### Three fingers, not one

`lisgd` reads the touchscreen without grabbing it, so every touch still reaches
BiteDJ underneath. A one- or two-finger swipe would land on a fader or a
waveform at the same time as switching workspaces. Three simultaneous fingers is
not a gesture the DJ UI uses, so it is unambiguous.

The panel reports `ABS_MT_SLOT` max 9, i.e. 10-point multitouch, so three fingers
is well within what the hardware tracks.

### The threshold is lowered

`lisgd -t 100`, against a default of 125. That default assumes a phone; this panel
is 480px tall, so 125px is more than a quarter of the screen and the gesture feels
like it is not registering. Raise it if you get accidental triggers, lower it if
swipes are being missed.

### waybar is not on the overlay layer

It could be made to float above fullscreen BiteDJ by setting `"layer": "overlay"`,
which would make it permanently visible. It is deliberately not: at 800×480 the
bar would cover BiteDJ's own `PLAY / BROWSE / SAMPLER / LEVELS / SETTINGS` tab
strip along the top. The gesture is the access mechanism instead.

If you want it always visible anyway, it is a one-line change in
`config/home/.config/waybar/config.jsonc`.

### The bar is 36px, not 28px

These are touch targets now. ~28px is below what a fingertip reliably hits. It
costs nothing, because on workspace 1 the bar is hidden behind fullscreen BiteDJ.

### Terminals tile, they do not float

The previous config floated the terminal at `900x500` — on an 800×480 panel, which
put its edges off-screen and out of reach on a touch-only device. Desktop windows
now tile. Only real dialogs float.

## The pieces

| File | Role |
|---|---|
| `scripts/bitedj-screen` | `desktop` / `dj` / `toggle`; ensures a terminal exists |
| `scripts/bitedj-gestures` | Starts `lisgd` with the two bindings |
| `scripts/bitedj-osk` | Toggles `wvkbd` |
| `scripts/bitedj-power` | Power menu, always confirms |
| `config/home/.config/sway/config` | Workspaces, keybindings, starts the above |
| `config/home/.config/waybar/*` | Bar and launchers |

`bitedj-gestures` resolves the touchscreen **by name**, not by event number —
`/dev/input/eventN` numbering depends on probe order and is not stable across
boots. Override with `BITEDJ_TOUCH_DEV` if the match fails; running the script
directly prints every candidate device.

## Troubleshooting

### Gestures do nothing

```bash
pgrep -af lisgd                       # running at all?
~/.local/bin/bitedj-gestures          # run in the foreground; it prints why it failed
```

Most likely causes, in order:

1. **`lisgd` is not running.** It is started from the sway config; check
   `~/.local/share/sway.log`.
2. **The wrong device was picked.** Run the script directly — it lists every
   candidate with its name. Set `BITEDJ_TOUCH_DEV=/dev/input/eventN`.
3. **Not in the `input` group.** `lisgd` reads the evdev node directly.
   `id -nG | grep input`. A group change needs a re-login.
4. **The swipe is too short.** Lower `-t` in `bitedj-gestures`.

The workspace buttons on waybar (`DJ` / `DESK`) do the same thing without
gestures, and a USB keyboard always works — `Super+Escape`.

### Testing gestures without touching the screen

Useful over SSH. Replay synthetic touch events onto the device:

```bash
sudo evemu-play /dev/input/event4 < swipe.evemu
```

The event stream is `E: <sec>.<usec> <type-hex> <code-hex> <value>`; a three-finger
swipe sets `ABS_MT_SLOT` 0/1/2, gives each an `ABS_MT_TRACKING_ID`, then walks
`ABS_MT_POSITION_Y` across the screen before releasing with tracking id `-1`.
Both directions were verified this way.

### The on-screen keyboard does not type

`wvkbd` needs `zwp_virtual_keyboard_v1` and layer-shell, both of which sway
provides. If it renders at all, both protocols bound successfully — it exits
immediately otherwise. Check the target window actually has keyboard focus.

### I am locked into BiteDJ with no keyboard

Not possible to fix from the screen if `lisgd` is dead and waybar is hidden. Over
SSH:

```bash
export SWAYSOCK=$(ls /run/user/1000/sway-ipc.*.sock | head -1)
swaymsg workspace number 2
```
