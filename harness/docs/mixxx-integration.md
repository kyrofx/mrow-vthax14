# Mixxx integration boundary

The harness owns history, feedback, and personalized ranking. Mixxx owns audio
playback. Musicsearch is the existing source of richer catalog/transition tools;
its local-track JSON exports can be imported here without provider access.

Current integration: a separate local web panel, JSON library import, playlist
export, and HTTP endpoints. There is no automatic Mixxx bridge yet.

## Local API

POST JSON with `Content-Type: application/json` to `http://127.0.0.1:8765`.
Success returns HTTP 200. Validation failures return HTTP 400 with an `error`
field. Browser requests must be same-origin. Session defaults to `default`.

| Endpoint | Request fields | Result |
| --- | --- | --- |
| `/api/library` | `tracks`: array or Musicsearch export object | Number imported |
| `/api/state` | `session` | Library and session plays/feedback |
| `/api/play` | `session`, `track_id`, optional actual `bpm` and `event_id` | New or existing `play_id` |
| `/api/feedback` | `session`, `track_id`, `rating`, `play_id` for good/mid/bad | Saved flag |
| `/api/recommend` | `session`, optional `count` (1–100, default 5), `options` | Ranked tracks, component scores, coverage, reasons |
| `/api/setlist` | `session`, optional `count` (1–100, default 10), `options` | Ordered tracks, completion/shortfall notes |
| `/api/export` | `track_ids`: ordered unique IDs (1–100) | M3U `filename` and `content` |

Good/mid/bad ratings must reference the matching recorded play and session.
A suggestion skip requires no play ID. `GET /health` returns an `ok` flag.

Scoring `options` accept `max_bpm_delta` (0–100, default 12), `allow_half_double`
(boolean, default true), `harmonic_only` (boolean, default false), `direction`
(`auto`, `steady`, `up`, `down`; default auto), and `target_bpm` (20–300 or null).
Unknown options are rejected. All options apply identically to recommendations
and every edge of a setlist. Unknown keys fail harmonic-only matching.

Good/mid/bad updates replace the prior rating for the same play; this panel is
an operator assessment interface, not a multi-person voting counter. Skip is
idempotent for that session/track and cannot target an already played song.

## Proposed fork connection

Mixxx documents deck controls including `play`, `file_bpm`, and playback position
in its [official controls reference](https://manual.mixxx.org/2.5/en/chapters/appendix/mixxx_controls).
These controls alone are not an HTTP bridge. Check the actual fork/version
before choosing the mechanism for sending events and embedding controls.

Send stable track IDs and actual BPM when songs become audience-facing
selections. Deck loading, headphone cueing, pause/resume, and position updates
must not create new plays. The policy for overlapping decks still needs defining.
Carry the returned `play_id` into crowd buttons so delayed feedback stays with
the correct song. Send a stable, globally unique `event_id` with each playback
event. Retries with identical track/session/BPM return the original `play_id`;
reuse with conflicting data is rejected. Distinct actual plays need new event
IDs. Omitting `event_id` explicitly records a new play on each request. Deduplication
is transactional and survives restarts.

Suggested screen controls: current song with Good / Mid / Bad; next-song picks
with Skip suggestion; Generate setlist. Suggestion skip does not skip audio.
Deck loading and automatic audio skip require additional fork code.

Preserve stable library IDs and prefer analyzed BPM. This implementation does
not access or modify a Mixxx database. Paths in an exported M3U must exist on
the machine running Mixxx.
