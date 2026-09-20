# Mixxx integration boundary

The harness owns history, feedback, and personalized ranking. Mixxx owns audio
playback. Musicsearch is the existing source of richer catalog/transition tools;
its local-track JSON exports can be imported here without provider access.

BiteDJ (the fork in `Mixxx/bitedj`) drives this directly through
`src/harness/harnessbridge.cpp`, over private stdin/stdout pipes to its bundled
worker. Mixxx embeds five Python modules as Qt resources, extracts them to a
private temporary directory, launches Python 3.9+ at lowered priority and stops
the worker on exit. The worker has no listening socket. The web page and M3U
export remain for standalone developer setups.

## Command protocol

Private JSON lines: request `{id, path, body}`; reply `{id, status, body}`.
Startup emits `{ready: true}`. IDs allow model requests to finish out of order
without delaying plays/ratings. Status 200 succeeds; 400 rejects invalid input;
500 is retryable. A shared dispatcher serves both this protocol and the optional
developer HTTP tool (POST JSON to the same paths). Session defaults to `default`.

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
| `/api/agent/settings` | Empty to read; `api_key`, `next_model`, `plan_model` to apply; `disconnect: true` to forget | Nonsecret runtime settings; keys never returned |
| `/api/agent/models` | — | Public OpenRouter text-model catalog |
| `/api/agent` | `session`, `action` (`next`, `generate`, `adjust`, `clear`), optional `count`, `options`, `retry` | Tracks, shared plan, rationale, source and fallback error |
| `/api/agent/view` | `session` | Saved plan with stale flag, runtime settings; no model request |
| `/api/agent/export` | `session`, `basis` from the displayed plan | M3U if plan still matches live context; otherwise HTTP 400 |

In HTTP developer mode, agent endpoints require a loopback peer and localhost Host header.
The native panel uses `/api/agent`; older `/api/recommend` and `/api/setlist`
remain compatibility endpoints and do not select the runtime agent models.
`next` refreshes an active rolling plan when its context changes, returning its
first five upcoming songs plus the full `plan`. Without a plan it returns next-song
alternatives. `generate`/`adjust` save a plan. `clear` removes it and returns
next-song alternatives. Equal requests are cached; `retry: true` explicitly
retries a model call. New plays/feedback/settings invalidate cached results.

The transport uses OpenRouter's documented
[chat-completions API](https://openrouter.ai/docs/api/api-reference/chat/send-chat-completion-request)
and [public model catalog](https://openrouter.ai/docs/quickstart).

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

**Library sync** posts the analyzed local Mixxx collection and each mounted drive's Rekordbox catalog to
`/api/library/sync` (`complete: true`), polled every 15 s and on every mount
change; ejecting posts `/api/library/unavailable`, which keeps the history but
stops those tracks being suggested. Catalog content hashes detect metadata edits
even when the track count stays the same. Local files on a mounted drive are
handled by that drive's Rekordbox catalog, avoiding duplicate local/USB IDs.

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

**When the worker is down**, plays, ratings and syncs queue in order and are
retried with backoff; Mixxx restarts the worker automatically. Audio never waits
for it. Runtime-only credentials need reentry; provisioned credentials reload.

**Settings**: `[Harness],enabled` defaults on. The embedded worker is the default
even if an old `url` remains in `mixxx.cfg`. Only explicit `[Harness],external=1`
uses HTTP at `[Harness],url` (developer/test mode, default `http://127.0.0.1:8765`).
Optional owner-only `~/.config/mrow/agent.json` supplies install-time OpenRouter
settings; it is never embedded in the binary. See [provisioning](../README.md#optional-builddeploy-provisioning).
