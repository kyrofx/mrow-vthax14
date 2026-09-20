# Audio

Getting sound out of this box needed two things to be true at once: BiteDJ pointed at
the right device with a sane channel layout, and PipeWire *not* holding that device.
Either one alone produces silence, and neither produces a useful error message.

## Known-good configuration

`~/.mixxx/soundconfig.xml`, verified working with a DDJ-FLX4:

```xml
<SoundManagerConfig api="ALSA" deck_count="4" latency="5" samplerate="48000" sync_buffers="2">
 <SoundDevice alsaHwDevice="hw:3,0" name="DDJ-FLX4: USB Audio" portAudioIndex="1">
  <output channel="0" channel_count="2" index="0" type="Master"/>
  <output channel="2" channel_count="2" index="0" type="Headphones"/>
 </SoundDevice>
</SoundManagerConfig>
```

In the UI (**SETTINGS › AUDIO**) that is:

| Setting | Value |
|---|---|
| Sound API | `ALSA` |
| Output device | `DDJ-FLX4 (hw:3,0)` |
| Main / Master | Channels **1–2** |
| Headphones | Channels **3–4** |
| Booth | **None** |
| Sample rate | `48000` |
| Audio buffer | index 5 |

Note the XML is **0-indexed** while the UI is 1-indexed: `channel="0"` is "channels 1–2"
and `channel="2"` is "channels 3–4".

The card number is not stable. `hw:3,0` is whatever `aplay -l` currently reports — check
it rather than assuming, especially if devices are plugged in a different order.

## Why direct ALSA rather than PipeWire

BiteDJ opens the controller as a raw ALSA `hw:` device through PortAudio. That is the
right choice here for two reasons:

- **Latency.** No resampling, no extra buffering, no graph scheduling between the engine
  and the hardware.
- **Channels.** The FLX4 exposes 4 output channels, which is what makes headphone cueing
  possible: Master on 1–2, Headphones on 3–4. A PipeWire sink would present this as a
  single "Analog Surround 4.0" stereo-ish sink and you would lose the split.

The cost is exclusivity: while BiteDJ has the card, nothing else can use it. On an
appliance that is a feature.

## PipeWire contention

**Symptom:** the controller is enumerated correctly (`aplay -l` lists it, `lsusb` shows
it), BiteDJ lists it in the device dropdown, but selecting it produces silence.

**Cause:** PipeWire claims the card at boot and holds it via the D-Bus device-reservation
protocol (`api.dbus.ReserveDevice1 = "Audio3"`). PortAudio does not speak that protocol,
so it cannot ask PipeWire to yield, and the open fails quietly.

**Immediate fix** (reversible, no reboot) — find the device id in `wpctl status`, then:

```bash
wpctl set-profile <device-id> 0      # profile 0 is "off"
```

**Permanent fix** — `~/.config/wireplumber/wireplumber.conf.d/50-bitedj-reserve-controller.conf`
disables the device so PipeWire never creates it:

```
monitor.alsa.rules = [
  {
    matches = [
      { device.name = "alsa_card.usb-AlphaTheta_Corporation_DDJ-FLX4_FBMP034632NN-00" }
      { api.alsa.card.name = "DDJ-FLX4" }
    ]
    actions = { update-props = { device.disabled = true } }
  }
]
```

Match entries are OR'd. The first is this exact unit — `device.name` embeds the USB
serial — and the second catches any DDJ-FLX4, so a replacement controller still works.

Built-in audio, HDMI and Bluetooth are untouched and still go through PipeWire normally.

Verify without rebooting — a WirePlumber restart re-runs the ALSA monitor and re-applies
rules, which is exactly what happens at boot:

```bash
systemctl --user restart wireplumber
wpctl status          # the controller should be absent from Devices
```

This does not disturb a running BiteDJ, because BiteDJ holds the card outside PipeWire.

To undo: delete the file and restart wireplumber.

## The config bugs that caused silence

Recorded because each produced silence with no error naming the cause.

### Wrong device

The default config pointed at `hw:2,0` — the Pi's built-in `bcm2835 Headphones` jack —
not the controller. Confirm what BiteDJ actually has open rather than trusting the UI:

```bash
sudo fuser -v /dev/snd/pcm*p
# /dev/snd/pcmC3D0p:  flx4  1384  F...m  mixxx     <- card 3, correct
```

The digit after `pcmC` is the card number. If it does not match the controller's card in
`aplay -l`, that is the bug.

### A channel assignment the device cannot satisfy

The default config had `Booth` on `channel="4"` while the device was a **2-channel** jack.
Booth at channel 4 means channels 4–5, so BiteDJ asked PortAudio for a 6-channel layout
from a stereo card. The result was not an error — it was this, forever:

```
Audio buffer underruns (sound device): 44 in the last 5 s, 1021 total (codes: "6")
```

Code `6` is `paOutputUnderflow | paInputOverflow`. **Continuous underruns at a steady
rate mean a layout the device cannot satisfy, not a buffer that is too small.** A genuinely
undersized buffer produces bursty, load-correlated underruns instead.

Only assign outputs the device physically has. The FLX4 has 4 channels, so Master and
Headphones fit and Booth must be None.

### Buffer far too small

`latency="3"`. Per the comment at `src/soundio/soundmanagerconfig.h:122`, this is an index
where **1 = 1 ms** — so roughly 2.7 ms. A Pi 4 driving USB audio will not hold that even
with real-time scheduling. Index 5 works. Tune downward only after it is stable, and watch
the underrun counter in `~/.mixxx/mixxx.log` while you do.

## Bluetooth

### It comes up rfkill-blocked from cold

The symptom is misleading. `bluetoothctl` starts fine, registers an agent, and
reports the controller — then:

```
[bluetoothctl]> scan on
SetDiscoveryFilter failed: org.bluez.Error.NotReady
Failed to start discovery: org.bluez.Error.NotReady
```

Nothing in that mentions rfkill. `power on` also fails, silently. The tell is:

```bash
rfkill list bluetooth
#   Soft blocked: yes        <- here
bluetoothctl show | grep PowerState
#   PowerState: off-blocked
```

The hardware is fine — `dmesg` shows the `BCM4345C0` firmware patch loading
normally at boot. The adapter is just administratively disabled.

`systemd-rfkill` persists rfkill state across reboots in
`/var/lib/systemd/rfkill/` and restores it at boot, so once something saves a
block it comes back every time. On this box the offending file was
`platform-soc-amba-fe201000.serial:bluetooth` containing `1`.

Clearing it by hand works until the next shutdown rewrites it from live state,
so `bluetooth-unblock.service` makes it deterministic instead:

```
ExecStart=/usr/sbin/rfkill unblock bluetooth
```

### Pairing from the touchscreen

`bitedj-bt-ui` is the one to use at a venue. It is a GTK picker with 64px rows,
reachable three ways:

- **Three fingers swiped left** — opens it over BiteDJ, mid-set
- The **BT** button on waybar
- `Super+T`

Tap a device, then confirm. The confirmation is deliberate: the rows are
full-width on a screen that also has a DJ deck under it, and a mis-tap that
silently reroutes audio mid-set is a far worse outcome than one extra tap. It is
drawn inline rather than as a dialog, because this window is a layer-shell
surface and a transient dialog parented to one does not reliably stack above it.

Already-paired devices are listed first and reconnect in two taps, which is the
common case at a venue; **Scan for new** is a separate,
deliberate action that takes 12s. Devices that never advertised a name are hidden
— bluez reports those with the MAC as the name, and they are beacons and
trackers, never speakers.

It draws on the **overlay** layer via gtk-layer-shell, which is the only way to
appear above a fullscreen window in sway. The alternative — dropping BiteDJ out
of fullscreen — reflows the waveform widgets mid-set, which is a real cost for a
speaker connection. With layer-shell, BiteDJ is not touched at all: it stays
fullscreen at 800x480 and keeps playing.

If `gir1.2-gtklayershell-0.1` is missing the picker falls back to an ordinary
window, which a sway rule floats — but it will then sit *behind* fullscreen
BiteDJ and look like it failed to launch.

Every bluetoothctl call runs on a worker thread. They block for seconds, and a
frozen UI on a touchscreen is indistinguishable from a crashed one.

### Pairing from a shell

Use the helper rather than remembering the bluetoothctl order of operations —
it handles the rfkill check, the agent, and setting the default sink:

```bash
bitedj-bt scan              # 20s; put the speaker in pairing mode first
bitedj-bt pair <MAC>        # pair + trust + connect + make default sink
bitedj-bt status
```

`trust` matters: without it the speaker will not reconnect on its own next time.

**If the speaker does not appear, check two things, in this order.**

1. **Is it actually in pairing mode?** A speaker already connected to a phone
   will not enter pairing mode at all — turn the phone's Bluetooth off first,
   then hold the speaker's Bluetooth button until it flashes fast. Pairing mode
   also times out after a couple of minutes, so scan promptly.

2. **Can bluez see it with a raw inquiry?**

   ```bash
   sudo hcitool scan --length=12
   ```

   If it shows up there but not in `bluetoothctl devices`, the discovery filter
   is at fault, not the radio — see
   [troubleshooting.md](troubleshooting.md#a-bluetooth-speaker-never-appears-in-a-scan).
   Both tools here set `transport bredr` to avoid it.

A scan that returns dozens of bare-MAC devices and no speaker is a working
Bluetooth stack finding beacons and trackers — not a broken one.

### BiteDJ needs `pipewire-alsa` to reach it at all

This is the part that is easy to miss. BiteDJ talks to PortAudio, PortAudio
talks to ALSA, and a Bluetooth speaker exists only as a **PipeWire** sink. Those
do not meet unless the PipeWire ALSA plugin is installed.

Without `pipewire-alsa`, `/etc/alsa/conf.d/` is empty and `aplay -L` lists only
raw hardware — no `pipewire` PCM anywhere. BiteDJ then has no route into
PipeWire, and a perfectly paired speaker is simply not selectable.

```bash
sudo apt install pipewire-alsa
aplay -L | grep -E '^(pipewire|default)$'     # both should now appear
```

Installing it does not disturb the DJ controller: BiteDJ opens `hw:3,0`
explicitly, not `default`, so the hardware path is untouched.

To use a Bluetooth speaker, set BiteDJ's output device to `pipewire` (or
`default`) in SETTINGS > AUDIO.

### Playing the room over Bluetooth while the deck keeps its own master and cue

This is the Rekordbox arrangement: the controller stays the real output, and a
second output feeds the room. **Mixxx does this natively — no engine changes.**

The appliance UI has three buses, each independently assignable to a device and
channel pair:

| Bus | Device | Channels | Carries |
|---|---|---|---|
| Master | DDJ-FLX4 | 1–2 | The deck's own output |
| Headphones | DDJ-FLX4 | 3–4 | Cue — **beatmatch here** |
| **Booth** | **pipewire** | 1–2 | The same program mix, to the Bluetooth speaker |

Booth is the master mix with its own gain, and the appliance skin already has a
Booth knob on the **LEVELS** tab, so the speaker gets independent volume.

Bluetooth's 100–300ms only lands on the Booth path. Master and Headphones stay on
the controller's own clock, so **cueing and beatmatching are unaffected** — you
monitor on headphones exactly as before, and the room hears the delayed feed.
That delay is inaudible to the room because there is nothing to compare it to.

Set it up in **SETTINGS › AUDIO**: cycle the Booth bus until it reads `pipewire`,
then Apply. Two prerequisites:

1. **`pipewire-alsa` must be installed**, or there is no `pipewire` device at all.
2. **The fork must be patched** to show it in the picker — see
   [fork-patches.md](fork-patches.md). Stock BiteDJ hides logical ALSA PCMs, so
   `pipewire` will not appear.

#### Clock drift

Two devices means two clock domains: the controller's USB clock and PipeWire's.
Mixxx handles this with `sync_buffers`, which the config already sets to `2`
("Default (long delay)"), the most forgiving setting. The device carrying Master
becomes the clock reference and everything else follows it, so the controller
leads and the Bluetooth path absorbs the correction.

If you hear periodic artefacts on the *speaker* over a long set, that is drift
correction; it does not affect the controller's own outputs.

#### The speaker follows PipeWire's default sink

The `pipewire` device is not bound to a particular speaker — it follows whatever
PipeWire's default sink is. `bitedj-bt pair` sets a newly connected speaker as
default, and PipeWire moves existing streams to a new default sink, so Booth
should follow without restarting BiteDJ. Confirm with `wpctl status`: the Booth
stream should appear underneath the Bluetooth sink.

If the speaker drops out, Booth falls back to whatever the default sink becomes
(usually built-in audio). Master and Headphones are untouched either way.

### Do not put Master on Bluetooth

The arrangement above works because Bluetooth only carries Booth. Putting
**Master** on the Bluetooth sink instead is a different thing and does not work:
A2DP latency is 100-300ms, so you would hear the beat up to a third of a second
after touching the jog wheel, and with the controller no longer carrying Master
you lose the 4-channel split and the headphone cue with it.

Use Booth for the room. Keep Master and Headphones on the controller.

## USB ports

Put the controller on a **USB 2.0** port. The Pi 4's USB 3.0 ports sit behind a VIA VL805
controller where USB audio devices are known to misbehave — on this box the FLX4 did not
work there at all.

The ideal split, and what is in use:

| Device | Bus | Why |
|---|---|---|
| DDJ-FLX4 | Bus 001 (USB 2.0) | Isochronous audio, away from bulk transfers |
| USB music drive | Bus 002 (USB 3.0) | Bulk transfers on a separate bus |

If dropouts appear *after* the configuration is correct, suspect power before software —
a bus-powered controller plus a USB drive is a meaningful load on the Pi's shared budget.

## Diagnostics

```bash
aplay -l                              # what ALSA sees; note the card numbers
lsusb                                 # did the controller enumerate at all
sudo fuser -v /dev/snd/pcm*p          # which process holds which card
wpctl status                          # what PipeWire has claimed
cat /proc/asound/card3/stream0        # channel counts, formats, rates
grep underrun ~/.mixxx/mixxx.log | tail
```

Prove the hardware independently of BiteDJ — **turn the volume down first**:

```bash
speaker-test -D hw:3,0 -c 4 -t pink -l 1
```

If that makes noise, the hardware and the PipeWire situation are fine and anything
remaining is BiteDJ's own configuration.
