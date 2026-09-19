# Local patches to the BiteDJ fork

Changes carried on top of `TeamDeckshark/bitedj`. Keep this list short — every
entry is something to re-apply or re-evaluate when merging upstream.

The vendored copy that actually gets built lives at `Mixxx/bitedj/` in this
repository (that is what the Docker cross-compile uses). The Pi's `~/bitedj`
checkout should be kept in step with it.

---

## Show the PipeWire PCM in the audio device picker

**File:** `src/preferences/audiodevicesettings.cpp` (`refreshDeviceList`)
**Why:** to route Booth to a Bluetooth speaker while the controller keeps Master
and Headphones — the Rekordbox "booth out" arrangement.

### The problem

Mixxx core already supports what Rekordbox does. The appliance UI has three
buses (`BusMaster`, `BusBooth`, `BusHeadphones`), each with its own device index
and channel base, and `applyBusConfig()` calls `addOutput()` per bus — so buses
can already live on *different* devices. Nothing in the engine needed changing.

The obstacle was purely in the picker. The fork hides logical ALSA PCMs:

```cpp
// The appliance UI only supports direct ALSA hardware devices. Hide
// logical PCMs such as default, pulse, dmix, and surround aliases;
// SoundDevicePortAudio leaves alsaHwDevice empty for all of them.
if (pDevice->getHostAPI() == MIXXX_PORTAUDIO_ALSA_STRING &&
        pDevice->getDeviceId().alsaHwDevice.isEmpty()) {
    continue;
}
```

That rule is right in general — nobody wants `dmix` or a surround alias in an
appliance picker. But `pipewire` is caught by it too, and on this box PipeWire
is the *only* route to system audio and therefore the only way to reach a
Bluetooth speaker. There is no second piece of audio hardware to be the Booth
device.

### The change

Two parts, both small:

1. **Allow the `pipewire` PCM through the filter**, by name. `SoundDeviceId`
   carries nothing else that distinguishes it from `dmix` — for PCMs that do not
   match the `(hw:X,Y)` pattern, `SoundDevicePortAudio` stores the full PortAudio
   name and leaves `alsaHwDevice` empty. Everything else logical stays hidden.

2. **Clamp its advertised channel count to 2.** This is the part that is easy to
   miss: the PipeWire PCM reports **64** output channels, and `buildOptions()`
   emits one option per stereo pair. Taken at face value that is 32 extra entries
   to tap past on *every* bus, on a touchscreen. It is a stereo sink in practice.

### Re-applying after an upstream merge

The hunk is anchored on the `alsaHwDevice.isEmpty()` filter and on
`const int channels = ...` immediately below it. If upstream reworks
`refreshDeviceList`, re-apply by hand rather than forcing the patch — and
re-check the channel clamp, since that is the half that silently degrades the UI
rather than breaking the build.

If a future upstream ever offers this properly (an "advanced devices" toggle,
say), drop this patch in favour of it.

### Verifying it works

```bash
# 1. The PCM exists at all (needs pipewire-alsa -- see audio.md)
aplay -L | grep -x pipewire

# 2. PortAudio, which is what BiteDJ uses, can see it
#    (a busy device is skipped, so stop BiteDJ first if checking the controller)
/tmp/padevs        # see audio.md for the 15-line enumerator

# 3. SETTINGS > AUDIO: cycle the Booth bus. `pipewire` should appear exactly
#    once, as a single stereo option, not 32 of them.
```

An incremental rebuild of this one file takes **about 50 seconds** on the Pi
(one TU, re-archive `libmixxx-lib.a`, relink with mold). Only the *first* build
is the ~2h20m one.
