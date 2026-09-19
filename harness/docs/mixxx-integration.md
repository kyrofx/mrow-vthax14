# Mixxx integration boundary

The harness owns history, feedback, and personalized ranking. Mixxx owns audio
playback. Musicsearch is the existing source of richer catalog/transition tools;
its local-track JSON exports can be imported here without provider access.

BiteDJ (the fork in `Mixxx/bitedj`) drives this directly through
`src/harness/harnessbridge.cpp`, over localhost HTTP. The web page and M3U
export remain for setups without the fork.

## Local API

POST JSON with `Content-Type: application/json` to `http://127.0.0.1:8765`.
Success returns HTTP 200. Validation failures return HTTP 400 with an `error`
field. Browser requests must be same-origin. Session defaults to `default`.

| Endpoint | Request fields | Result |
| --- | --- | --- |
| `/api/library` | `tracks`: array or Musicsearch export object | Number imported |
| `/api/state` | `session` | Library and session plays/feedback |
| `/api/play` | `session`, `track_id`, optional actual `bpm`, `event_id`, and `track` + `scope` to record the song at the same time | New or existing `play_id` |
| `/api/feedback` | `session`, `track_id`, `rating`, `play_id` for good/mid/bad | Saved flag |
| `/api/recommend` | `session`, optional `count` (1–100, default 5), `options`, `model` (false to skip the model) | Ranked tracks with component scores, coverage and reasons, plus `source` and any `model_error` |
| `/api/setlist` | `session`, optional `count` (1–100, default 10), `options` | Ordered tracks, completion/shortfall notes |
| `/api/export` | `track_ids`: ordered unique IDs (1–100) | M3U `filename` and `content` |
| `/api/library/sync` | `scope` (a drive), `tracks`, optional `complete` | Counts synced, skipped (with reasons), made unavailable |
| `/api/library/unavailable` | `scope` | Tracks of an ejected drive no longer suggested |
| `/api/features` | `tracks`: `id`, `source`, and generated features | Number stored |
| `/api/status` | — | Available tracks and the model configuration |

A sync is lenient where an import is strict: a track Mixxx has not analyzed yet
is skipped and reported rather than failing the drive. `complete` marks tracks
of that drive missing from the sync unavailable, keeping their history.
`/api/features` holds what a future metadata model generates (energy, vocals,
timbre, sections); a library sync never overwrites those.

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

## How BiteDJ uses it

Implemented in `src/harness/` and `src/widget/wharnesspanel.cpp`; the plan and
its rationale are in [mixxx-integration-plan.md](mixxx-integration-plan.md).

**Track ids** are `<filesystem UUID>:<path relative to the drive>`, or
`local:<absolute path>` off a drive. They survive a re-plug under a different
mount name, and the bridge resolves the current location before loading.

**Sessions** are named like the fork's per-drive history (`2026-09-19`,
`#2`...). `[Library],reset_played_tracks` starts a new one; the name is
remembered, so a restart mid-set resumes the same set.

**Plays** are reported when a track becomes the audible one
(`PlayerInfo::currentPlayingTrackChanged`), with the deck's actual BPM and the
same six-track replay window the History feature uses. Loading, cueing and
pausing record nothing. Each play carries an `event_id` built from session,
deck and load time, so a retry never records a play twice. The play carries its
track, so a file from a drive with no Rekordbox export is still recorded.

**Library sync** posts each mounted drive's Rekordbox catalog to
`/api/library/sync` (`complete: true`), polled every 15 s and on every mount
change; ejecting posts `/api/library/unavailable`, which keeps the history but
stops those tracks being suggested.

**Suggestions** are fetched after every play, rating and sync. Loading is
manual: a suggestion loads only when the DJ taps Load, and never into a playing
deck.

**Controls** — every input maps onto `[Harness]`, so remapping needs no code:

| Control | Does |
| --- | --- |
| `rate_good`, `rate_mid`, `rate_bad` | Rate the current play |
| `skip_suggestion_N` | Drop suggestion N for this set |
| `load_suggestion_N_deck_M` | Load suggestion N into deck M |
| `refresh` | Ask for suggestions again |
| `status` | 0 assistant offline, 1 its own ranking, 2 model-refined |
| `current_rating` | 0 none, 1 bad, 2 mid, 3 good |
| `suggestion_count` | Suggestions currently shown |

The GPIO crowd buttons reach `rate_*` through a virtual MIDI port and the
hidden mapping `res/controllers/mrow-crowd-buttons.midi.xml`; a controller
mapping can bind pads to the same controls.

**When the harness is down**, plays, ratings and syncs queue in order and are
retried with backoff; the panel says so, and nothing about the set is blocked.

**Settings**: `[Harness],enabled` (default on) and `[Harness],url` (default
`http://127.0.0.1:8765`) in `mixxx.cfg`.
