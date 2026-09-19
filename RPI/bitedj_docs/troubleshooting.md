# Troubleshooting

Every entry below is a failure that actually happened during bring-up, with the
diagnosis that resolved it. They are ordered roughly by how long they took to find.

## First moves

```bash
tail -50 ~/.local/share/sway.log     # session: sway, waybar, and BiteDJ's stdout
tail -50 ~/.mixxx/mixxx.log          # BiteDJ's own log
```

With a keyboard attached, `Super+Return` opens a terminal over the UI. `Super+Shift+R`
reloads the sway config; `Super+Shift+E` prompts to exit the session.

SSH always works — `~/.profile` only launches sway on VT 1, so an SSH login gets a plain
shell no matter how broken the session is.

---

## Software rendering

**Symptom:** waveforms are slow, CPU is high, audio is glitchy under load — or nothing
obvious at all, because the UI still draws correctly.

**Diagnosis:** look for `llvmpipe` threads *inside* the BiteDJ process.

```bash
ps -L -o tid,cls,rtprio,comm -p $(pgrep -x mixxx) | grep llvmpipe
```

Any output is a failure. Confirm with:

```bash
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 eglinfo -B
```

The tell is the two platforms disagreeing:

```
GBM platform:       OpenGL renderer: V3D 4.2.14.0      <- hardware works
Wayland platform:   OpenGL renderer: llvmpipe          <- clients get software
```

**Cause:** the Pi 4 has two DRM cards — `card0` is `v3d` (render-only) and `card1` is
`vc4-drm` (display-only). wlroots advertises the display card to clients, which cannot
render, so Mesa falls back to `llvmpipe` without an error.

**Why it matters more than it looks:** the rasterizer threads are spawned inside the
BiteDJ process and therefore inherit its `SCHED_FIFO` priority 49. Software rendering
then competes with the audio callback at real-time priority on a 4-core box.

**Fix:** in `~/.profile`, before `exec sway`:

```sh
export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
```

After a reboot the Wayland platform reports `V3D 4.2.14.0` and the `llvmpipe` thread
count is zero.

---

## BiteDJ crash-loops at boot, screen shows a swaynag error

**Symptom:** `bitedj-session` is running but `mixxx` is not, and an error bar reads
"BiteDJ keeps crashing on startup".

**Diagnosis:** `grep -i "platform plugin" ~/.local/share/sway.log`

```
qt.qpa.plugin: Could not find the Qt platform plugin "wayland" in ""
Available platform plugins are: xcb, offscreen, vnc, minimalegl, minimal, ...
```

**Cause:** `qt6-wayland` not installed. Note `wayland` is absent from the "available"
list — that list is the diagnostic.

**Fix:** `sudo apt install qt6-wayland`, then confirm
`/usr/lib/aarch64-linux-gnu/qt6/plugins/platforms/` contains `libqwayland-egl.so`.

The supervisor's give-up-after-5-failures behaviour is what made this visible rather
than presenting a black screen. That is the intended design.

---

## waybar dies ~40 s after login

**Symptom:** waybar runs briefly at boot, then exits. Log:

```
[error] Error calling StartServiceByName for org.freedesktop.portal.Desktop: Timeout was reached
```

**Cause:** *not* a missing package — the portals were installed. `systemd --user` and
the D-Bus activation environment inherit nothing from the login shell, so
`XDG_CURRENT_DESKTOP` never reaches `xdg-desktop-portal`. It cannot pick a backend
(`/usr/share/xdg-desktop-portal/sway-portals.conf` is keyed on that variable), so every
portal call blocks until it times out.

**Diagnosis:**

```bash
systemctl --user show-environment | grep -iE 'wayland|desktop|swaysock'
```

Empty output is the bug.

**Fix:** first `exec` in the sway config, before anything that may touch a portal:

```
exec_always --no-startup-id dbus-update-activation-environment --systemd \
    WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE SWAYSOCK
```

Also start waybar with `exec` rather than a `bar { swaybar_command waybar }` block — a
bar block is launched on sway's own schedule and races the line above.

---

## Cannot get out of BiteDJ / gestures do nothing

Three fingers swiped down should give you a desktop. If it does not:

```bash
pgrep -af lisgd                # is the gesture daemon running?
~/.local/bin/bitedj-gestures   # run in foreground; prints why it failed and
                               # lists every candidate input device
id -nG | grep input            # lisgd reads the evdev node directly
```

Fallbacks that always work: the `DJ`/`DESK` buttons on waybar, `Super+Escape` with a
USB keyboard, or over SSH:

```bash
export SWAYSOCK=$(ls /run/user/1000/sway-ipc.*.sock | head -1)
swaymsg workspace number 2
```

Full detail in [desktop-access.md](desktop-access.md).

---

## No audio from the controller

Three independent causes, all of which produce silence with no error naming them. In
order of how often they are the answer:

1. **PipeWire is holding the card.** It claims the device at boot via D-Bus device
   reservation, which PortAudio cannot negotiate with.
2. **BiteDJ is pointed at the wrong card** — check with `sudo fuser -v /dev/snd/pcm*p`
   and compare the card digit against `aplay -l`.
3. **An output is assigned to a channel the device does not have**, which shows up as
   steady continuous underruns rather than as an error.

All three, with fixes and the permanent WirePlumber rule, are in [audio.md](audio.md).

---

## Audio underruns

```
Audio buffer underruns (sound device): 44 in the last 5 s, 1021 total (codes: "6")
```

**Read the shape of the number before touching the buffer size.** A steady, unchanging
rate means BiteDJ asked for a channel layout the device cannot provide — raising the
buffer will not help. Bursty underruns that correlate with load are a genuinely
undersized buffer.

Code `6` is `paOutputUnderflow | paInputOverflow`. See
[audio.md](audio.md#a-channel-assignment-the-device-cannot-satisfy).

---

## Track browser is empty with a drive plugged in

Check in this order:

```bash
ls /media/$USER /run/media/$USER      # did it mount at all?
pgrep -af udiskie                     # is the automounter running?
systemctl is-active udisks2
sudo journalctl -u polkit -b | tail   # authorization refused?
```

BiteDJ scans `/media`, `/run/media`, `/mnt`, `/media/$USER` and `/run/media/$USER`, so
the mount *location* is rarely the problem.

One non-obvious failure: if `USER` is unset in the session, the two per-user roots
collapse to the bare `/media` and the browser looks empty with no error at all.
`bitedj-session` pins `USER` to prevent this.

---

## Benign noise — do not chase these

| Message | Why it is fine |
|---|---|
| `Failed to load default skin styles /skins/default.qss!` | There is no skin named `default`. The fork ships only `BiteDJ`, and it loads. |
| `[wlr] [EGL] eglQueryDeviceStringEXT, error: EGL_BAD_PARAMETER` | Known non-fatal wlroots probe. sway uses the GPU fine — it has zero `llvmpipe` threads. |
| `eglSwapBuffers failed with 0x300d, surface: 0x0` ×9 at startup | Startup-only, does not recur. Count them: if the total stops growing, ignore it. |
| ALSA `Unknown PCM cards.pcm.surround*`, `.iec958`, `.modem` | Standard ALSA config noise. |
| `jack server is not running or cannot be started` | BiteDJ probes JACK and falls back. Harmless, but see below. |

That last one appears even when audio is working correctly — BiteDJ probes JACK, finds
none, and falls back to ALSA. See [audio.md](audio.md).

---

## Build problems

See [build.md](build.md). In short: `AUTOMOC` sitting at 10% for ~25 minutes is normal;
an OOM kill just needs the same command rerun; and `~/build-bitedj.sh` already
distinguishes an OOM from a real compile error so it will not loop on the latter.

---

## Recovering a box that will not show a UI

The session is configured so you are never locked out:

0. Three fingers swiped down gets you a desktop and a terminal on the box itself —
   try that before reaching for SSH. See [desktop-access.md](desktop-access.md).
1. `ssh flx4` — always works, session config is VT-1-only.
2. `systemctl --user show-environment` and `tail ~/.local/share/sway.log` to see how far
   the chain got.
3. If sway itself will not start, `sudo systemctl enable lightdm && sudo systemctl
   set-default graphical.target` restores a conventional desktop to work from.
4. `/boot/firmware/cmdline.txt.bak-prebitedj` is the pre-change kernel command line if
   `preempt=full` ever needs reverting.
