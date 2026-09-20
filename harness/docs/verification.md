# Verification record

## Embedded Pi agent and provisioning — 2026-09-19

To repeat the automated checks from the repository root:

```sh
python3 -m unittest discover -s harness/tests -v
python3 -m unittest discover -s RPI/tests -v
git diff --check
```

After configuring the arm64 build with `BUILD_TESTING=ON`, from `Mixxx/bitedj`:

```sh
docker compose run --rm pi cmake --build build --target mixxx mixxx-test --parallel 6
docker compose run --rm pi ./build/mixxx-test '--gtest_filter=Harness*' --gtest_color=no
```

- Rebuilt both `mixxx` and `mixxx-test` in the Debian trixie arm64 container.
- **64 Python agent tests, 10 GPIO tests and 14 native `Harness*` tests passed.**
  New tests cover private JSONL IPC, a real worker subprocess, concurrent feedback
  during slow advice, optional startup credentials, ownership/mode checks,
  symlink rejection, atomic no-overwrite/replacement, secret redaction and local
  fallback after rejected configuration. The native bridge also records real
  play/feedback through its bundled worker while ignoring a stale HTTP URL.
- The native worker test also passed with only the build volume mounted in a
  fresh container: no `/src`, `/harness`, standalone HTTP server or source tree.
- Shell syntax, deployment dry run and `git diff --check` passed. The old
  `mrow-harness` startup unit has been removed from the shipped configuration;
  deployment scripts disable installed copies while retaining history.
- No real key was provisioned and no Pi deployment or hardware/UI/audio test
  was performed for this change. The earlier device records below concern the
  previous HTTP integration, not the embedded worker.

## Agent branch — 2026-09-19

- Python harness: **55 tests passed**. New coverage checks separate next/plan
  models, key lifetime and redaction, private path aliases, model-output
  validation, per-edge constraints, persisted plans, feedback/play/skip/eject
  invalidation, stale in-flight responses, concurrent request coalescing, stale
  export rejection, localhost/origin guards and the HTTP agent workflow.
- GPIO integration: **10 tests passed**.
- Both `mixxx` and `mixxx-test` built in the Debian trixie arm64 Pi container.
  All **12 targeted `Harness*` tests passed**, covering bridge requests, runtime settings and
  setlist actions, queued feedback, local catalog sync, same-count metadata
  edits, missing files and separate library/performance BPM.
- JavaScript syntax and `git diff --check`: passed.
- The local service answered health, settings, empty-library advice and the
  live public OpenRouter catalog (447 text-output models at verification time).
- Paid model responses were exercised through simulated OpenRouter responses;
  no user API key was supplied. Real music transitions, a mounted Rekordbox USB,
  touch interaction and crowd judgment remain user/device tests.
- A synthetic 1,000-track catalog produced a ten-song local agent plan in
  0.279 seconds on this Mac; refreshing unchanged context took 0.007 seconds.
  These timings are a smoke check, not a Pi performance guarantee.

For a manual test, start with an analyzed library and open **Assist → Models**.
Apply a key and two model choices. Expected: model-sourced advice, or a clearly
labeled local fallback. Generate a plan, play its first song, and press **Bad**.
Expected: the played song leaves the upcoming sequence and the new plan uses
the rating. Eject a source drive: its songs must disappear. Try loading into a
playing deck: it must be refused. Check the exported playlist against the
displayed order. The JSON example contains placeholder paths, so it can test
planning but cannot validate audio loading.

## Earlier integration record

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
