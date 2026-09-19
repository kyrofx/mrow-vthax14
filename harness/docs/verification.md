# Verification record

Verified locally on macOS on 2026-09-19. No physical Raspberry Pi, audio hardware,
real music library, or Mixxx fork was available for these checks.

## Automated

`python3 -m unittest discover -s harness/tests -v` — 38 tests passed.
`python3 -m unittest discover -s RPI/tests -v` — 10 tests passed (crowd buttons).
`node --check harness/src/app.js` — passed.
`git diff --check` — passed.

Harness coverage includes atomic imports, rich feature preservation and score
effects, unknown-feature handling, score decomposition, actual performance BPM,
half/double-time continuity, learned transitions, session exclusions, feedback
replacement and persistence, event deduplication, database migration, bounded
planning, infeasible constraints, and playlist order. Added for the Mixxx
integration: per-drive sync (lenient about unanalyzed tracks), ejected drives
keeping history but leaving the suggestions, generated features surviving a
sync, and the model client against both the OpenAI and Anthropic protocols —
alias mapping so no path leaves the device, invented or duplicate picks
ignored, refusals, timeouts, connection errors and malformed replies all
falling back to the local ranking, and the response cache.

The HTTP end-to-end test sends a Musicsearch-format import, records/retries a
play, changes good to bad, checks stored state, skips a suggestion, generates a
constrained setlist, exports the exact ordered paths, and reconstructs the service
against the same database to verify persistence. All without recording planned
tracks as actual plays.

Button-daemon coverage: config validation (bad pins, notes, channels, duplicate
pins or notes), the defaults matching the shipped Mixxx mapping, press/release
note bytes, bounce rejection, repeated states, a release with no press, and
unknown pins.

## BiteDJ fork (C++)

Built in the project's Debian trixie arm64 container (`Mixxx/bitedj`,
`docker compose run --rm pi`), which is the same Debian release as the
appliance. `./mixxx-test --gtest_filter="Harness*"` — 10 tests passed: track
ids keyed by drive UUID and relative path (including a re-plug under another
mount point), Camelot conversion, session naming, and — against a fake harness
over real HTTP — plays with their track metadata, a rating made before the play
is acknowledged, ratings collapsing before that acknowledgement, the `[Harness]`
controls, skips, queue-and-retry while the harness is down, starting a new set,
and a disabled bridge sending nothing.

## On screen

The app was run in the container against a real harness process on a seeded
six-track library, on an 800×480 virtual display over VNC (`docker compose up
pi-run`). Verified: the Assist tab appears in the topbar; the panel reports the
assistant online and which ranking answered; ratings are disabled until
something has played ("Nothing played yet"); four suggestions render with BPM,
key and the reason, each with Load 1 / Load 2 / Skip; and tapping Skip removes
that row, posts the skip, and refills from the harness.

Not exercised in the container: loading a suggestion into a deck (no audio
device or real files), the Rekordbox catalog sync (no drive) and the GPIO
buttons (no hardware). The first two were covered on the device instead — see
below.


## On the device (flx4)

Deployed with `RPI/scripts/deploy.sh --no-build` on 2026-09-19. Binary and
resource checksums match the staged tree; `/usr/local` was written through the
staging directory with sudo, as ssh cannot write there.

Verified on the Pi itself:

- **Rekordbox catalog sync**: 2034 tracks off a real USB drive, keyed by its
  filesystem UUID, all marked available. Every one carries a Camelot key and a
  duration, 823 distinct BPMs, artists on all but 73. Genres on only 12 — that
  drive's export simply has few.
- **The harness service** runs and answers `/health`, with its database in
  `~/.mixxx/harness/`.
- **The crowd buttons service** runs and its virtual MIDI port is live on the
  ALSA sequencer (`client 129: 'MROW Crowd Buttons'`). `python3-rtmidi` had to
  be installed; the deploy script now does that when missing.

Still not verified: pressing a physical button (none wired yet), a real cloud
model endpoint (no key configured), loading a suggestion into a deck, and
everything about performance during a set.

## Browser

An isolated Playwright browser and temporary database exercised:

- JSON file upload and displayed library/picks.
- Record play and current song/history update.
- Bad feedback: pressed state and changed recommendation scores.
- Setlist generation and an actual M3U download; inspected downloaded paths
  matched the displayed Peak → Lift → Steady order.
- Harmonic-only enabled on tracks without keys: no eligible picks, explicit
  zero-song shortfall, and disabled export.
- Reload preserved current play and saved feedback.
- A separate API playback event updated the browser automatically and cleared
  the now-stale generated setlist/export button.
- 390px viewport: no horizontal overflow; mobile screenshot inspected.
- No console errors/warnings after the missing favicon request was corrected.

Browser screenshots are local, ignored artifacts under `output/playwright/`.
The temporary data contains fictional tracks only.

## Performance sample

A synthetic 1,000-track local catalog on this Mac took approximately 7 ms for
five recommendations and 238 ms for a ten-song bounded plan. This is a smoke
benchmark with compact metadata, not a Pi latency guarantee or a large embedding
catalog benchmark.

## Remaining real-world validation

Connect the actual Mixxx fork to the documented event API, decide when a track
counts as audience-facing during overlapping decks, and test on the Pi. Listen
to transitions with actual analyzed files and compare suggestions to the DJ's
choices before treating the weights as calibrated. Buttons capture an operator's
assessment; they do not automatically sense crowd reaction.
