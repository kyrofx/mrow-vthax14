# Verification record

Verified locally on macOS on 2026-09-19. No physical Raspberry Pi, audio hardware,
real music library, or Mixxx fork was available for these checks.

## Automated

`python3 -m unittest discover -s harness/tests -v` — 22 tests passed.
`node --check harness/src/app.js` — passed.
`git diff --check` — passed.

Coverage includes atomic imports, rich feature preservation and score effects,
unknown-feature handling, score decomposition, actual performance BPM,
half/double-time continuity, learned transitions, session exclusions, feedback
replacement and persistence, event deduplication, database migration, bounded
planning, infeasible constraints, and playlist order.

The HTTP end-to-end test sends a Musicsearch-format import, records/retries a
play, changes good to bad, checks stored state, skips a suggestion, generates a
constrained setlist, exports the exact ordered paths, and reconstructs the service
against the same database to verify persistence. All without recording planned
tracks as actual plays.

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
