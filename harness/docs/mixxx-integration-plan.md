# Mixxx integration plan

Connects the harness to the BiteDJ fork (`Mixxx/bitedj`) on the Raspberry Pi,
with suggestions refined by a cloud model.

**Status: built.** What was actually implemented, and how to use it, is in
[mixxx-integration.md](mixxx-integration.md); what has and has not been
verified is in [verification.md](verification.md). Still open: the GPIO
hardware, a real cloud endpoint, and everything measured on the device. Supersedes the "Proposed fork
connection" section of [mixxx-integration.md](mixxx-integration.md) once built.

## Decisions

| Question | Decision |
| --- | --- |
| Process model | The harness is a **sidecar** process on the Pi. Mixxx talks to it over localhost HTTP. |
| Model | Any cloud API speaking the **OpenAI** (`/v1/chat/completions`) or **Anthropic** (`/v1/messages`) protocol. |
| Where history and feedback live | **On the Pi, under `~/.mixxx/`.** |
| Deck loading | **Suggest only.** The DJ taps a suggestion to load it; nothing loads automatically. |
| Crowd rating input | **GPIO buttons** on the Pi for Good / Mid / Bad, remappable, alongside the touchscreen and any MIDI control. |
| Energy, vocals, timbre, sections | **Later**, from a separate metadata model. The harness is designed to accept them now. |

```
Mixxx (C++, SCHED_FIFO 49)        Harness (Python, normal priority)       Cloud model
  HarnessBridge ── HTTP 127.0.0.1 ──>  SQLite + heuristic ranker  ── HTTPS ──>  rerank + reasons
  skin panel    <── suggestions ─────  model client (fallback: heuristic)
```

The sidecar keeps SQLite writes, beam search and network calls out of the
real-time process, and a harness crash cannot stop audio.

## Where Mixxx and the fork keep history today

Researched in the fork's source. The device's `~/.mixxx` was not inspected.

| Data | Stock Mixxx | This fork |
| --- | --- | --- |
| Set history (tracklists) | `~/.mixxx/mixxxdb.sqlite`: `Playlists` rows with `hidden = 2` (`PLHT_SET_LOG`), tracks in `PlaylistTracks` (`position`, `pl_datetime_added`) | **Not stored on the Pi.** `SetlogFeature` deletes any local setlog playlists at startup (`purgeLocalSetlogPlaylists`). History is written to the drive the track came from: `<mount>/.bitedj/history.sqlite`, table `history(session, position, location, duration_seconds, played_at)`, keyed by path relative to the mount. Tracks played from local storage get no history, only a play count. |
| Play count / last played | `library.timesplayed`, `library.last_played_at` in `mixxxdb.sqlite` | Same, still updated by `Track::updatePlayCounter()` |
| Star rating | `library.rating` (0–5, per track) | For drive tracks, written to the drive (`<mount>/.bitedj`, `meta_overrides`) via `FsMetaOverrideStore` |
| "Played this session" | — | `PlayedTracks`, in memory only, cleared by `[Library],reset_played_tracks` |

All history recording hangs off `PlayerInfo::currentPlayingTrackChanged`, which
fires when a track becomes the audible one. `SetlogFeature` ignores a track
replayed within the last 6 plays (`kHistoryTrackDuplicateDistance`).

### Can Good / Mid / Bad be stored in those structures?

Not cleanly:

- **`library.rating`** is one value per *track*, not per play, and it is the
  DJ's own star rating. For drive tracks it is also written back to the stick.
  Encoding crowd feedback there would overwrite the DJ's ratings and still lose
  which play the feedback was for.
- **`PlaylistTracks`** has no spare column, and the fork deletes local setlog
  playlists anyway. Adding a column means a schema revision (40) in a file
  shared with upstream migrations.
- **The drive's `history` table** is fork-owned and would be easy to extend,
  but it lives on the stick, not the Pi.

**Plan:** the harness keeps its own database at `~/.mixxx/harness/harness.sqlite3`.
Its existing schema already records one row per play (session, BPM, time,
`event_id`) and one assessment per play. Mixxx's files stay untouched:

- The harness never writes `mixxxdb.sqlite`. Mixxx holds it open and owns its
  schema migrations.
- The fork's drive history is unchanged. The Pi-side harness history is an
  additional, local record.

## Phase 1: The interface between Mixxx and the harness

1. **Track ID:** `<filesystem UUID>:<path relative to mount root>` for drive
   tracks. This is the key the `.bitedj` stores already use, plus the drive's
   identity. Local tracks use `local:<absolute path>`. The harness stores the
   ID. Mixxx sends the current absolute location with each suggestion request,
   so re-plugging under a different mount name still works.
2. **Session:** a new harness session on each `[Library],reset_played_tracks`,
   named like the fork's drive sessions (`YYYY-MM-DD`, `#2`, …).
3. **What counts as a play:** `currentPlayingTrackChanged`, with the same
   6-track replay window `SetlogFeature` uses. Loading, cueing and pausing
   don't count, which settles the overlapping-decks question.
4. **`event_id`:** `<session>:<deck>:<load timestamp ms>`, so a retried request
   deduplicates.

## Phase 2: Harness (Python, stdlib only)

- **Database location:** default `--database ~/.mixxx/harness/harness.sqlite3`.
- **`POST /api/library/sync`:** an incremental upsert per drive, sent by Mixxx.
  Add an `available` column and set it to false on eject rather than deleting
  tracks, so history survives. Skip tracks without an analyzed BPM until they
  have one.
- **`POST /api/play`:** accept the Phase 1 track ID and `event_id`, unchanged
  otherwise.
- **Model client (`model.py`):**
  - `provider = openai | anthropic`, plus `base_url`, `model` and `api_key`,
    read from `~/.config/mrow/harness.env` (mode 600, never committed).
  - The heuristic ranker produces the top N candidates (for example 20). The
    model returns a reordered subset with a one-line reason each, as JSON.
    Validate the response: only known IDs, no duplicates.
  - A hard timeout of about 4 s and no retries on the request path. On
    timeout, network error, bad JSON or a missing key, return the heuristic
    order with `source: "heuristic"`. The Pi is WiFi-only and moves between
    venues, so being offline is expected.
  - Cache by (session state, candidate set) so repeated panel refreshes don't
    call the model again.
  - Send only titles, artists, genres, BPM, key, recent plays and ratings:
    no file paths, no drive labels.
- **Service:** a systemd user unit, bound to `127.0.0.1`, with `Nice=10` and
  `CPUAffinity` set away from the cores Mixxx pins its engine to (check
  `src/util/rtscheduling` for which ones). Started before Mixxx by
  `bitedj-session`.

## Phase 3: Mixxx bridge (C++, new `src/harness/`)

- **`HarnessClient`:** async JSON on the GUI thread via `network/JsonWebTask` or
  `QNetworkAccessManager`, never on the engine thread. It does health checks
  with backoff, and queues play events in memory while the harness is down;
  replays are safe because of `event_id`.
- **Hooks:**
  - `PlayerInfo::currentPlayingTrackChanged` → `/api/play` with the deck's
    actual BPM (`[ChannelN],bpm`). Keep the returned `play_id` for the rating
    buttons, then request fresh suggestions.
  - Drive mount or scan finished (wherever `WUsbList` and the Rekordbox
    feature learn of drives) → `/api/library/sync`. Send title, artist, genre,
    duration, BPM, and key converted to Camelot on the C++ side (from the
    key utilities).
  - Eject → mark that drive's tracks unavailable.
  - `[Library],reset_played_tracks` → new session.
- **Controls (`[Harness]` group):** `rate_good`, `rate_mid`, `rate_bad`,
  `skip_suggestion_N`, `load_suggestion_N_to_deck_M`, `refresh`, `status`
  (0 offline, 1 heuristic only, 2 model). These controls are the single entry
  point for every input: skin buttons, the GPIO buttons (Phase 5), and any
  MIDI control, such as FLX4 pads, all map onto them. Remapping means editing
  a mapping file, not code.
- **Loading (suggest only):** loading happens only on an explicit tap or
  control, via `PlayerManager::slotLoadLocationToPlayer(location, group, false)`.
  Refuse to load into a deck that is playing.
- **Settings:** enabled flag and harness URL in `mixxx.cfg`, shown on the
  in-skin System page.

## Phase 4: UI (800×480 touch)

- **`WHarnessPanel`**, modeled on `WUsbList`, registered in
  `legacyskinparser.cpp`. It contains:
  - the current track with Good / Mid / Bad buttons
  - 3–5 suggestions, each with a reason line and Skip / Load A / Load B
  - a model status indicator
- **Placement:** a new "Assist" tab in `res/skins/BiteDJ/topbar.xml`.
- **Errors:** harness-offline and model-offline states go through
  `WNotificationStrip`, never dialogs.
- **The existing web page** (`harness/src/index.html`) stays as a debug view.

## Phase 5: GPIO crowd buttons

Three momentary buttons on Pi GPIO pins for Good / Mid / Bad. The Pi cannot
send MIDI from GPIO by itself, so a small bridge turns button presses into
MIDI notes on a virtual port. Mixxx then handles them like any controller:

```
GPIO pins ──> mrow-buttons (daemon) ──> virtual MIDI port "MROW Crowd Buttons"
          ──> Mixxx mapping ──> [Harness],rate_good / rate_mid / rate_bad
```

- **Daemon (`RPI/src/buttons/`):** Python using `python3-libgpiod` (edge
  events, internal pull-ups, ~30 ms debounce) and `python3-rtmidi` (creates
  the named virtual port). Both are Debian trixie packages. It runs as a
  systemd user unit with `Restart=always`, started before Mixxx.
- **Two remapping layers:**
  1. **Pin → MIDI note:** in `~/.config/mrow/buttons.toml`, for example
     `good = { pin = 17, note = 60 }`. Rewiring a button means editing this
     file.
  2. **MIDI note → action:** a Mixxx mapping
     `res/controllers/MROW Crowd Buttons.midi.xml`. Remapping or adding
     actions (for example making a fourth button "skip suggestion") means
     editing this file. The FLX4 mapping can bind its pads to the same
     `[Harness]` controls, so both work at once.
- **Fits the fork's device handling:** mark the mapping
  `<hidden>true</hidden>`. `ControllerSettings` then applies it automatically
  and keeps the port out of the Devices picker (`autoApplyHiddenMapping`). The
  port name must match the mapping name, because the fork hides MIDI devices
  that have no matching mapping.
- **Feedback to the DJ:** Mixxx flashes the pressed rating in the Assist panel
  and the notification strip. If the MIDI port is duplex, the mapping can also
  echo to an LED per button (optional GPIO outputs driven by the daemon).
- **No current play:** a press with nothing audible yet is ignored and
  reported in the notification strip. A second press on the same play
  replaces the rating, as the harness already does.
- **Why not read GPIO in Mixxx?** Mixxx runs at real-time priority, and
  hardware polling doesn't belong there; a daemon crash also can't affect
  audio. A kernel `gpio-key` overlay that sends keystrokes would need no code,
  but it only works while the Mixxx window has focus and bypasses the MIDI
  remapping path.

## Phase 6: Deploy and verify

- **Deploy:** rsync `harness/` to the Pi, install the user unit, and create the
  env file with the API key by hand on the device.
- **Tests:**
  - Python: sync/upsert, availability on eject, ID handling, model client
    against a fake local server for both protocols (timeouts, malformed JSON,
    unknown IDs fall back to heuristic).
  - GTest: key-to-Camelot conversion, track ID derivation, `event_id`
    determinism, `HarnessClient` against a fake local server.
  - Button daemon: config parsing, debounce, pin-to-note mapping (with GPIO
    mocked).
- **On the device:**
  - no audio underruns while model calls are in flight
  - WiFi dropped mid-set (status falls to heuristic, plays still recorded)
  - drive ejected while one of its tracks is suggested
  - power pulled mid-set, then reboot (the harness database is intact and the
    session resumes)
  - GPIO buttons: each press records one rating, bounce doesn't double-count,
    and they work while another app or notification has focus

## Later: metadata from a second model

Mixxx supplies BPM, key, duration and genre only, so until this exists the
scorer's energy, vocal, timbre and section components stay neutral. A separate
model will generate them later. To make that a drop-in addition:

- **The harness already accepts these fields** (`energy`, `vocalness`,
  `embedding` with a model name, `sections`) and validates them in
  `scoring.features()`.
- **Keep them separate from Mixxx metadata:** store generated features in
  their own table keyed by track ID, with the generating model and version.
  `/api/library/sync` from Mixxx must never overwrite them.
- **Add `POST /api/features` now:** it upserts features per track ID with a
  `source` field. It stays unused until the metadata model exists.
- **Work out later:**
  - where it runs (the Pi is too slow for audio analysis, so probably
    off-device or in the cloud, cached by file hash)
  - whether it needs audio or only metadata
  - how results reach the Pi (pushed to `/api/features`, or a file on the
    drive)
- **Re-weight when it lands:** once features are real rather than neutral,
  the scoring weights in [scoring.md](scoring.md) need recalibrating against
  the DJ's actual choices.

## Risks

- **Few features until the metadata model exists:** ranking is mostly tempo,
  key, history and crowd feedback, and the model has little else to reason
  with.
- **CPU:** the Pi 4 has little headroom. Measure harness CPU on the device
  during a set before enabling the model by default.
- **Diverging history:** history is now kept in two places, on the drive (the
  fork's tracklists) and on the Pi (the harness's plays and ratings). They
  share the `currentPlayingTrackChanged` trigger and replay window so they
  agree.
