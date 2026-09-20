# Stems — plan

Play a track as two parts the DJ can drop independently: **vocals** and
**instrumental**. Separation happens ahead of time on a workstation; the
appliance only plays what it is given.

**Status: Phase 0 measured (it passes), the rest not built.** This is the plan
and the reasoning behind it.

## Decisions

| Question | Decision |
| --- | --- |
| How many stems | **Two**: vocals and instrumental. |
| Per-stem effects / EQ | **No.** Effects and EQ stay on the deck as a whole. |
| Where stems live | **On the USB drive**, beside the track, so they travel with the stick like everything else the DJ owns. |
| Separation | **Ahead of time**, off the device. A Pi 4 cannot separate audio in real time, and would not try. |

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

## Phase 1: separation on the workstation

- **Demucs** (`htdemucs`), run on the workstation, keeping only the vocal and
  instrumental split (the four-stem model's other outputs are summed).
- Output beside the track on the drive:

  ```
  Music/Artist - Title.mp3
  Music/Artist - Title.stems/
      vocals.opus
      instrumental.opus
      manifest.json
  ```

- `manifest.json` records the model and version, the sample rate, the frame
  count, and a hash of the source file. The hash is what stops a stale stem
  set being played against a re-encoded or replaced track.
- Alignment is by construction: both stems come from one decode of the source,
  so frame 0 is frame 0. The frame count in the manifest is checked at load.
- **Codec**: Opus at ~128 kbps stereo per stem. Two stems then cost roughly
  the size of the original again, which is the price of the feature.
- The same pass should emit the harness's missing features — `vocalness`,
  `energy` and section boundaries all fall out of a separation run — and POST
  them to `/api/features`. One pipeline, two payoffs.
- A script under `RPI/scripts/` or a small tool in the MROW repo; it never
  runs on the Pi.

## Phase 2: playback

- A `StemReader` sitting behind the deck's `ReadAheadManager`: two
  `CachingReader`s, summed with per-stem gain on each read. ~5 MB and one
  worker thread more per deck, which is nothing on 4 GB.
- Stem discovery at track load: look for `<track>.stems/` on the same drive,
  read the manifest, verify the hash and frame count. Anything missing or
  mismatched means the track plays normally — a library is mostly not
  separated, and that path must stay exactly as it is today.
- Controls per deck: `[ChannelN],stem_vocals_enabled`,
  `stem_instrumental_enabled`, and `[ChannelN],stem_available` for the skin
  and mappings to read.
- Gains cross-fade over a few milliseconds rather than switching abruptly; a
  hard gate on a mute is audible as a click.

## Phase 3: on screen and on the controller — built (against stub controls)

The S button, the swap and the two stem buttons exist and were exercised on an
800x480 display. The controls behind them (`src/mixer/stemcontrols.cpp`) are a
stub: `stem_available` is always 0 and toggling a stem changes no audio, which
is what Phase 2 fills in.

### The S button

A toggle in each deck's `WaveformInfo_Controls` row (`waveform.xml`), to the
right of the key readout and the loop beat count, beside the existing Q
(quantize) and keylock buttons. Same `templates/toggle_button.xml`, same 30x30
size, so it reads as one of that set; an "S" where Q has its letter. Q's glyph
is an embedded raster in `icons/quantize.svg`; the S is better drawn as button
text styled in `style.qss`, which stays sharp at any size and takes the
highlight colour in its pressed state for free.

The button is present on every deck regardless of the track, like Q and
keylock. It is disabled (not hidden) when the loaded track has no stems, so
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
  The manifest hash is the guard.
- **Drive removal mid-play** already has a path (tracks are evicted on eject);
  two readers per deck must not add a second one that behaves differently.
- **Separation quality** varies by material. Vocals over a dense mix leave
  artifacts, and a DJ will hear them on a big system before anyone else does.

## Testing

- Unit: manifest parsing and rejection (bad hash, wrong frame count, missing
  file), stem discovery on a drive, the mix with gains at 0, 1 and mid.
- Device: no xruns with both decks playing stems; seek, loop and scratch keep
  the stems locked; eject mid-play; a track with no stems is unchanged.
