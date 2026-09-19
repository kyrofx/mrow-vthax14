# Runtime setup

Compiling the app does not give you an appliance. This is the configuration that turns
a binary into a machine that powers on into a DJ deck. Every item here was needed; none
of it is optional polish.

Paths below are on the Pi. Anything under `/etc` needs root.

## Packages added beyond the build deps

```bash
sudo apt install sway swayidle waybar udiskie qt6-wayland \
                 xdg-desktop-portal xdg-desktop-portal-wlr xdg-desktop-portal-gtk \
                 grim mesa-utils
```

Versions in use: sway 1.10.1, waybar 0.12.0, udiskie 2.5.7, qt6-wayland 6.8.2,
xdg-desktop-portal-wlr 0.7.1, Mesa 26.2.2.

`qt6-wayland` is **not optional** — without it Qt has no `wayland` platform plugin and
BiteDJ aborts on every launch. `swaynag` is part of the `sway` package, not separate.
`grim` and `mesa-utils` are diagnostics (screenshots, `eglinfo`) and can be dropped from
a shipping image.

## Real-time audio

`/etc/security/limits.d/99-bitedj-audio.conf`:

```
@audio   -   rtprio      95
@audio   -   memlock     unlimited
@audio   -   nice        -19
```

The user must be in the `audio` group. `memlock` matters because this box swaps to zram,
and a page fault inside the engine callback is an audible glitch.

Verify on the running process — do not assume:

```bash
chrt -p $(pgrep -x mixxx)          # expect SCHED_FIFO, priority 49
grep -i vmlck /proc/$(pgrep -x mixxx)/status
```

If it says `SCHED_OTHER`, the limits are not being applied and the app has silently
degraded to normal scheduling.

## CPU governor

`/etc/systemd/system/cpu-performance.service`, enabled. `ondemand` ramps too slowly for
an audio callback: the engine wakes, the clock is still at 600 MHz, and the buffer
underruns before the governor reacts.

```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor   # expect: performance
```

## Kernel preemption

`preempt=full` appended to the single line of `/boot/firmware/cmdline.txt`.
Backup at `/boot/firmware/cmdline.txt.bak-prebitedj`.

That file **must stay one line** — a stray newline will make the Pi unbootable. Verify
with `cat /proc/cmdline` after a reboot rather than trusting the edit.

## Graphics — the one that is invisible until you look

`~/.profile` exports, before `exec sway`:

```sh
export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
```

Without it, Wayland clients get software rendering. This is covered in full in
[troubleshooting.md](troubleshooting.md#software-rendering) and
[architecture.md](architecture.md#graphics). Do not remove it.

## Autologin and session

```bash
sudo raspi-config nonint do_boot_behaviour B2   # console autologin
sudo systemctl disable lightdm
```

That sets `multi-user.target` and writes
`/etc/systemd/system/getty@tty1.service.d/autologin.conf`.

The launch itself is at the end of `~/.profile`, guarded so SSH sessions are unaffected:

```sh
if [ -z "$WAYLAND_DISPLAY" ] && [ "$XDG_VTNR" = "1" ]; then
    export XDG_CURRENT_DESKTOP=sway
    export XDG_SESSION_TYPE=wayland
    export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
    exec sway >"$HOME/.local/share/sway.log" 2>&1
fi
```

`~/.profile` is used because neither `~/.bash_profile` nor `~/.bash_login` exists — if
you create either, bash will stop reading `~/.profile` and the session will not start.

## Session supervision

`~/.local/bin/bitedj-session` runs BiteDJ in a restart loop. A crash must not leave a
dead black screen, but a binary that dies instantly must not spin either — so restarts
are rate-limited and it gives up after 5 failures inside 15 s, raising a `swaynag`
error that names the log. That behaviour is what surfaced the missing `qt6-wayland`
immediately instead of presenting a black screen.

It also pins `USER` (see [architecture.md](architecture.md#removable-media)) and sets
`QT_QPA_PLATFORM=wayland`.

## USB automount

`udisks2` is only a backend — something has to call it. `udiskie --automount --notify
--no-tray` runs from the sway config.

BiteDJ scans both `/media/$USER` and `/run/media/$USER`, so the mount location does not
need tuning.

**Confirmed working with a real drive.** A USB stick mounted at `/media/flx4/` and
BiteDJ analyzed tracks from it without intervention.

## Polkit

`/etc/polkit-1/rules.d/50-bitedj.rules` grants, to a **local active session in the
`audio` group** only:

- everything under `org.freedesktop.udisks2.*` — mount, unmount, eject, drive power-off
- `org.freedesktop.login1.power-off` / `reboot` (and the `-multiple-sessions` variants)

The box has no keyboard to answer an auth prompt, so an unanswerable dialog is the same
as the feature not working. A polkit agent still runs in the session
(`/usr/libexec/polkit-mate-authentication-agent-1`) for anything not covered.

Note `/etc/polkit-1/rules.d` is mode `750 root:polkitd`, so a non-root `ls` reports the
file missing. Check with `sudo`.

## Verifying a boot

```bash
for p in sway waybar udiskie mixxx polkit-mate; do
    printf '%-14s %s\n' "$p" "$(pgrep -f $p >/dev/null && echo RUNNING || echo MISSING)"
done
systemctl --user show-environment | grep -iE 'wayland|desktop|swaysock'
chrt -p $(pgrep -x mixxx)
vcgencmd get_throttled                    # expect 0x0
```

Screenshot the panel without a keyboard:

```bash
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 grim /tmp/screen.png
```

## Sudo

`/etc/sudoers.d/010-flx4-nopasswd` grants `flx4` passwordless sudo. Convenient for
remote administration; **reconsider it for anything shipped to a customer.**

## Audio

Configured and working against a DDJ-FLX4. Two pieces live outside BiteDJ's own settings
and belong to the runtime:

- `~/.config/wireplumber/wireplumber.conf.d/50-bitedj-reserve-controller.conf` keeps
  PipeWire off the controller's ALSA card, so BiteDJ can open it exclusively at boot.
  Without it the controller is silent with no useful error.
- The controller must be on a **USB 2.0** port. It does not work on the Pi 4's USB 3.0
  ports.

Full detail, the known-good device configuration and diagnostics are in
[audio.md](audio.md).
