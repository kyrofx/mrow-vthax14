# Stem playback

Generate stems on a workstation with `RPI/scripts/prepare-library.sh`, then
bring the prepared USB drive to the Pi. The appliance only plays stems:
there is no separation worker, request queue, or generation action in its UI.

The Play page's **S** button is greyed out when the loaded track has no valid
stems. With stems available, it opens **Vocals** and **Instrumental** controls.
Reload a track after adding its stem files to make them available.

## Prepare a library on your Mac or Linux computer

Plug the music USB drive into the computer and run:

```sh
./RPI/scripts/prepare-library.sh "/Volumes/MUSIC/Contents"
```

Use your actual music folder path (Linux example: `/media/you/MUSIC/Contents`).
The launcher creates a private environment under `~/.cache/bitedj-stems/venv`,
installs pinned Demucs/PyTorch dependencies on first use, and downloads the
model when first needed. Install ffmpeg and Python 3.10–3.13 beforehand; on
macOS: `brew install ffmpeg python@3.13`. The launcher keeps the Mac awake
while processing.

It recursively processes one track at a time, prints Demucs progress and
per-track elapsed time, skips current stems, and continues past individual
failures. Hidden files, generated stems and partial outputs are excluded.
A timestamped JSON report in `~/.cache/bitedj-stems/` records completed and
failed paths. Ctrl-C stops the run; the same command resumes by skipping
finished tracks. An interrupted track restarts from the beginning.

```sh
# Preview without installing anything or writing files:
./RPI/scripts/prepare-library.sh "/Volumes/MUSIC/Contents" --dry-run
# Explicit NVIDIA GPU, or CPU:
./RPI/scripts/prepare-library.sh "/Volumes/MUSIC/Contents" --device cuda
./RPI/scripts/prepare-library.sh "/Volumes/MUSIC/Contents" --device cpu
```

Automatic device selection uses CUDA when available, otherwise CPU. Apple
MPS can be explicitly requested with `--device mps`, but is not the default
because Demucs operations may fall back to CPU or be unsupported. Use CPU
if that backend fails. GPU speed and memory needs depend on the computer;
no workstation performance benchmark is claimed here.

Original audio files are unchanged. Keep each `<filename>.stems` folder
beside its original when copying a prepared library to the USB drive.
Reconnect the drive to the Pi and load/reload the track to use the stems.
This launcher prepares files locally; it does not fetch a library over SSH.

## Decisions

| Question | Decision |
| --- | --- |
| How many stems | **Two**: vocals and instrumental. |
| Per-stem effects / EQ | **No.** Effects and EQ stay on the deck as a whole. |
| Where stems live | **On the USB drive**, beside the track, so they travel with the stick like everything else the DJ owns. |
| Separation | **Ahead of time**, on a workstation only. |

"No per-stem effects" is what makes this affordable — see below.

## What the engine does today

- `mixxx::kEngineChannelCount` (`src/engine/engine.h`) is a compile-time
  constant, and the reader's cache chunks are sized from it
  (`CachingReaderChunk::kChannels`). The engine is stereo end to end.
- Each deck owns one `CachingReader`, which feeds one `ReadAheadManager`,
  which feeds one scaler — the timestretcher (SoundTouch on this box, chosen
  because RubberBand's phase vocoder is too expensive here).
- A `CachingReader` costs ~5 MB of cache (80 chunks) and one worker thread,
  which the appliance runs at `SCHED_FIFO` 60.

## Where to mix the stems

This is the whole design decision, and it is about CPU.

**After the scaler** — one scaler per stem. This is what a general stems
implementation does, and it is what buys per-stem effects: each stem is
independently time-stretched, so it can be processed separately. On this box
that means **2× the keylock cost per deck**, and keylock is the most expensive
thing in the audio path.

**Inside `ReadAheadManager`, after the cache and before the scaler** — the two
cached readers are summed with per-stem gains as samples are pulled, so there
is still **one scaler per deck**. Only decoding doubles, and Opus decode is far
cheaper than time-stretching.

**Take the second.** The decision above gives it away for free: without
per-stem effects there is nothing the separate scalers would have bought. The
costs of this choice, stated plainly:

- A stem mute lands after the scaler's look-ahead — tens of milliseconds. Not
  audible as latency, but it is not sample-exact either.
- Stems share the deck's EQ, filter and effects. That is the decision above.
- Both stems stay locked to one playback position by construction, through the
  same `ReadAheadManager`, so seeking, looping, scratching and sync cannot
  drift them apart. This is a real advantage, not just a saving.

`ReadAheadManager` already takes its reader by pointer and is virtual
throughout, so this is a contained change rather than an engine rewrite.

## Phase 0: measured, and it passes

Run on the device (`flx4`, Pi 4, performance governor, box otherwise idle) on
2026-09-19, decoding a 137 s track single-threaded:

| Stream | Decode speed | One stream costs |
| --- | --- | --- |
| Original MP3 | 165x realtime | 0.61% of one core |
| **Opus 128 kbps stem** | **112x realtime** | **0.89% of one core** |
| FLAC stem | 160x realtime | 0.62% of one core |

A stems track decodes two Opus streams where it used to decode one MP3:
**+0.9% of one core per deck**, under 2% of one core with both decks on
stems. Mixxx idles at 6.6% on four cores. Summing the two streams is ~192k
float additions per second and does not register.

Opus also settles the codec: 2.8 MB against FLAC's 21 MB for the same track,
for within a third of the decode cost. Two stems then cost about the size of
the original again.

Caveats worth keeping in mind:

- This measures ffmpeg's decoders. Mixxx decodes Opus through libopusfile and
  MP3 through libmad, so the absolute numbers will differ — but not by the
  order of magnitude that would change the conclusion.
- Decoding happens in `CachingReaderWorker` threads, which this appliance runs
  at `SCHED_FIFO` 60 — *above* the engine's main thread at 49. Doubling the
  work at that priority is the theoretical risk; at ~1% of a core it is noise,
  but the real test is an xrun count during a set, not a benchmark.
- Nothing here exercises the audio callback. What is measured is the cost that
  would land in the reader threads, which is where it lands by design.

## Phase 1: separation on the workstation — built

`RPI/scripts/separate-stems.py`. Walks a drive or a list of files, skips
tracks that already have current stems. The workstation launcher uses this code.

- **Demucs** (`htdemucs`, `--two-stems=vocals`), keeping only the vocal and
  instrumental split.
- Each track is separated into a `.partial` directory and renamed into place
  only once everything is written. A half-finished stems directory beside a
  track is worse than none, because the device would try to play it.
- Output beside the track on the drive:

  ```
  Music/Artist - Title.mp3
  Music/Artist - Title.mp3.stems/
      vocals.opus
      instrumental.opus
      manifest.json
  ```

- `manifest.json` records the model and version, the sample rate, the frame
  count, and the size of the source file. The size check stops a stale stem
  set being played against a re-encoded or replaced track.
- Alignment is by construction: both stems come from one decode of the source,
  so frame 0 is frame 0. The frame count in the manifest is checked at load.
- **Codec**: Opus at ~128 kbps stereo per stem. Two stems then cost roughly
  the size of the original again, which is the price of the feature.
- The same pass should emit the harness's missing features — `vocalness`,
  `energy` and section boundaries all fall out of a separation run — and POST
  them to `/api/features`. One pipeline, two payoffs.
- Used by the workstation library preparation launcher.

## Phase 2: playback — built

`src/engine/cachingreader/stemcachingreader.cpp`, a `CachingReader` subclass
the deck uses in place of the plain one (decks only: a sampler has no stem
controls, and an extra reader per sampler would be sixteen threads for
nothing).

- It owns a second `CachingReader` for the instrumental, created the first
  time that deck loads a stems track — a deck that never sees one never pays
  the 5 MB and the worker thread.
- `read()` sums the two, ramping each gain over ~5 ms so a toggle is not heard
  as a click. Because this sits behind `ReadAheadManager`, the sum happens
  before the scaler: one timestretcher, and the stems cannot drift apart.
- **The deck's track stays the track.** The audio comes from the stem files,
  but `CachingReader` now reports track loads through a virtual hook that the
  stem reader overrides, so the deck keeps the real track's beatgrid, cues,
  key and waveform. Getting this wrong is silent: the deck plays, and the
  cues are simply gone.
- Stem discovery at load (`src/track/stemset.cpp`): the manifest must be a
  version this build understands, and must name a source file of exactly the
  size on disk. That last check is what stops a re-encoded track playing the
  stems of what used to be there. Anything missing or mismatched means the
  track plays normally, and why is logged.
- Controls per deck, owned by the reader so they exist before the skin or any
  mapping binds to them: `[ChannelN],stem_vocals_enabled`,
  `stem_instrumental_enabled`, `stem_available`.

## Phase 3: on screen and on the controller — built

The S button, the swap and the two stem buttons exist and were exercised on an
800x480 display. They were first built against stub controls; Phase 2 replaced
those with the real ones, which the reader owns — toggling a stem now changes
what the deck reads.

### The S button

A toggle in each deck's `WaveformInfo_Controls` row (`waveform.xml`), to the
right of the key readout and the loop beat count, beside the existing Q
(quantize) and keylock buttons. Same `templates/toggle_button.xml`, same 30x30
size, so it reads as one of that set; an "S" where Q has its letter. Q's glyph
is an embedded raster in `icons/quantize.svg`; the S is better drawn as button
text styled in `style.qss`, which stays sharp at any size and takes the
highlight colour in its pressed state for free.

The button is present on every deck regardless of the track, like Q and
keylock. It is disabled (not hidden) when the loaded track has no valid stems, so
the row never reflows — the space it occupies is the same either way.

### What it swaps

Tapping S replaces that deck's small overview waveform — the high-speed
scrubbing strip at the bottom of the deck strip, left or right depending on
which deck — with that track's stem buttons. Tapping it again brings the
overview back.

The overview strip and the stem panel occupy the same 50f in `deck.xml`, each
binding its own `visible` to `[Skin],stem_panel_<channel>` — inverted for the
overview — which the S button writes. Nothing above moves when it switches,
and each deck has its own panel.

A `WidgetStack` would be the natural shape, and is what the cue drawer uses.
It does not work here: `LegacySkinParser` reads the `currentpage` attribute
raw (`legacyskinparser.cpp`, `parseWidgetStack`), so it cannot carry the
channel variable, and both decks would share one panel. Paired `visible`
bindings are the per-deck equivalent.

### The stem buttons

Two buttons across the strip: **Vocals** and **Instrumental**. Each toggles
its stem and lights while that stem is playing. Two stems is the decision, not
a step towards four — the control names leave room for more without the skin
being designed around it.

Trading the overview for them costs the DJ their scrub strip while the panel
is open, which is why it is a toggle and not a permanent row — and why S sits
next to Q rather than somewhere that takes more space.

### On the controller

The same `[ChannelN],stem_*` controls bound to pads, as the crowd buttons are
bound to `[Harness]`. Nothing in the skin is a precondition for that.

## Risks

- **CPU** is the whole question. Phase 0 answers it or kills the feature.
- **Storage**: roughly double per separated track on the stick.
- **Stale stems**: a re-encoded track with an old stem directory beside it.
  The manifest source-size check is the guard; same-size replacements are not detected.
- **Drive removal mid-play** already has a path (tracks are evicted on eject);
  two readers per deck must not add a second one that behaves differently.
- **Separation quality** varies by material. Vocals over a dense mix leave
  artifacts, and a DJ will hear them on a big system before anyone else does.

## Testing

Automated: 15 Python tests for the separation pass (layout, staleness, the
atomic swap, the demucs and ffmpeg commands as data) and 13 GTest cases for
the device half — what a trustworthy stem set is, and the mixing and ramping
of gains.

End to end in the container: a track with stems beside it loads them (two
reader workers open the two files) while the deck reports the original track;
a track whose stems were made stale falls back to plain playback and says
why.

## What is left

- **Nobody has heard it.** A real separated track, played on the device,
  through speakers. Everything above says the right files are opened and the
  right numbers come out; none of it says it sounds right.
- Real Demucs output has been verified: both Opus files and the manifest
  are generated successfully by the workstation preparation command.
- **No xrun measurement while actually playing stems.** Phase 0 measured
  decode cost in isolation; the honest test is a set.
- S is disabled whenever `stem_available` is false.
