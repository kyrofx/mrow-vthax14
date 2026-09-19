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

### Pairing

Use the helper rather than remembering the bluetoothctl order of operations —
it handles the rfkill check, the agent, and setting the default sink:

```bash
bitedj-bt scan              # 20s; put the speaker in pairing mode first
bitedj-bt pair <MAC>        # pair + trust + connect + make default sink
bitedj-bt status
```

`trust` matters: without it the speaker will not reconnect on its own next time.

**If the speaker does not appear in a scan, it is not in pairing mode.** A JBL
that is already connected to a phone will not advertise. Hold the Bluetooth
button until it beeps and flashes, then scan again. A scan that returns dozens
of bare-MAC devices and no speaker is a working Bluetooth stack finding beacons
and trackers — not a broken one.

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

### Why you should not perform on it

A2DP latency is **100-300ms**. You would hear the beat up to a third of a second
after touching the jog wheel, so beatmatching and cueing are impossible. It is
useful for casual playback, for checking a track, or for proving audio flows at
all.

It also costs you the headphone cue. Routing to a PipeWire sink means giving up
`hw:3,0` and its 4-channel split (Master 1-2, Headphones 3-4) for a stereo sink.

Mixxx can drive two devices at once — Master to `pipewire`, Headphones to
`hw:3,0` — but they are separate clock domains (the Bluetooth clock and the
controller's USB clock), which drift apart and produce xruns over a set. For
performance, use the controller's master out into a wired speaker.

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
