# MROW DJ harness

A local DJ assistant for Raspberry Pi 4 and 5, also runnable on macOS.
Requires Python 3.9+ and no third-party Python packages. Google Cloud Gemini is optional;
local ranking and planning keep working without a key or network.

## Agent workflow in Mixxx / BiteDJ

1. Start the rebuilt Mixxx fork on the Pi and open **Assist**. The bundled agent
   starts automatically, using private process pipes rather than a web server.
2. Open **Models**, paste a Google Cloud Gemini key and choose a next-song model and a
   setlist model. **Load models** reads the current catalog; model IDs can also
   be typed directly. **Apply** starts using the choices immediately.
3. Import/scan a Rekordbox USB export in Mixxx, or add analyzed local songs to
   Mixxx's collection. The bridge syncs both, including BPM, key and genre.
   Missing files and tracks without BPM are excluded. Ejecting a drive removes
   it from consideration while preserving its feedback history.
4. Use **Setlist**, choose a song count, and tap **Follow crowd**, **Build**,
   **Hold**, or **Ease down**. These create a rolling plan of upcoming songs.
   **Good / Mid / Bad** rate the audible play and update that plan automatically.
5. Review the reasons and use **Load 1 / Load 2** yourself. A playing deck cannot
   be replaced. **Export M3U** saves a playlist; **New set** starts fresh session
   exclusions while retaining learned history.

The native panel is the appliance interface. No browser or separately launched
service is required. The Python sources in this directory are embedded in the
Mixxx executable at build time. The optional standalone web tool below is for
development; it has a separate process and separate runtime credentials.

Runtime keys live only in the harness process: never in SQLite, Mixxx settings,
browser storage, or API replies. Restarting requires reentry unless a private
install-time config was provisioned. **Disconnect** clears the active key for
this run; a provisioned file reloads at the next restart. Applying settings
does not itself verify a credential; advice status reports model success or
the local fallback and error. Requests can incur Google Cloud Gemini charges. Agent
controls in the optional HTTP developer tool accept localhost clients only.

## Optional build/deploy provisioning

Prepare the key with a hidden prompt on the build workstation, outside the repo:

```sh
python3 RPI/scripts/agent-config.py
RPI/scripts/deploy.sh --agent-config "$HOME/.config/mrow-build/agent.json"
```

The first command prompts for a Google Cloud Gemini key, separate next-song/setlist
model IDs (`gemini-2.5-flash` is the default), and an optional ElevenLabs key.
Both key prompts hide input. The second builds and deploys Mixxx,
then streams the JSON through SSH to `~/.config/mrow/agent.json` on the Pi.
No secret enters compiler arguments, binary resources, container layers, shell
history or deployment staging. Without `--agent-config`, existing credentials
are untouched. `--dry-run` validates the file but never transmits it.

The file has mode `600`, its directory `700`, and belongs to the appliance user.
Mixxx loads it automatically when its worker starts. Group/world-readable files,
symlinks, other owners and invalid fields are rejected; local scoring still works
and **Models** explains the error. JSON fields are `api_key`, `next_model`,
`plan_model`, and optional `elevenlabs_api_key`. Existing three-field configs
remain valid. Both providers load at worker startup; invalid supplied fields reject
the entire file before either provider is configured. See the
[build guide](../RPI/bitedj_docs/build.md#inject-google-cloud-gemini-and-elevenlabs-keys)
for replacement and verification steps. On a Pi built locally, prepare it directly with:

```sh
python3 RPI/scripts/agent-config.py --output "$HOME/.config/mrow/agent.json"
```

The prompt refuses to overwrite an existing file. Runtime changes in **Models**
do not modify the provisioned file. To permanently disable provisioned cloud
access, remove that file on the Pi, then disconnect in Mixxx or restart it.

This is permission-protected provisioning, not encryption at rest. The appliance
user, root, or someone reading an unencrypted SD card can recover the key. Use a
dedicated spending-limited Google Cloud Gemini key and revoke it if the device is lost.
Compiling a secret into a binary would not protect it. Provisioning opts into
cloud advice as library/play/feedback context changes; model use can incur costs.

## How the agent chooses

The local scorer retrieves a feasible path and alternatives at every step, up
to 120 candidates. The model sees aliases, music metadata, actual performance
BPM, recent ratings, aggregate genre feedback and the previous plan. The next
model ranks immediate alternatives; the setlist model orders a coherent sequence
and explains the choices. Once a plan is active, Up next shows its upcoming order.

Every model choice is checked against the available library and every planned
transition against the tempo/key policy. Invented IDs and duplicates are ignored;
missing or incompatible choices are filled locally. A model path that dead-ends
earlier than the local plan is rejected. Unanalyzed musical features stay unknown.
This does not infer beat/phrase alignment or automatically hear the crowd.

Plans persist per session. Plays, skips, ratings, library edits, drive availability
and model changes invalidate the plan. A response that arrives after the live
context changes is discarded and replaced by a fresh local result. Unchanged
polls reuse results, and concurrent refreshes coalesce; **Refresh advice** / native
**Refresh** explicitly retries the model, including after a network failure.
Export rejects stale plans. Runtime Google Cloud Gemini calls time out after 25 seconds,
with the existing local scorer as fallback.

## Pi operation and troubleshooting

- **No songs suggested:** confirm the Rekordbox export is mounted and visible in
  Mixxx, or the songs are in Mixxx's local collection. Files must exist and have
  analyzed BPM. Allow up to 15 seconds for library sync. A plan may be shorter
  than requested when songs are played/skipped or transition constraints rule
  them out; **New set** resets session exclusions, not learned history.
- **Local advice instead of model advice:** local mode is expected without a
  key or network. Open **Models** to check configuration, then **Refresh** to
  retry. Applying a key alone does not prove authentication or model availability.
- **Agent fails to start:** check `python3 --version` on the Pi (3.9+ required).
  Rebuild from the full MROW checkout so the Python sources are embedded. Do not
  start the old HTTP service to repair a bundled-worker failure.
- **Provisioned configuration rejected:** run
  `python3 RPI/scripts/agent-config.py --validate "$HOME/.config/mrow/agent.json"`
  as the appliance user from a source checkout. Check ownership and mode `600`;
  the file must not be a symlink. Never paste its contents into issue reports.
- **Settings revert on restart:** runtime changes intentionally stay in memory.
  Re-provision the private file to persist a new key/model selection. An install
  does not restart Mixxx unless `--restart` is requested; restarting stops audio.

History, feedback and saved plans live under the Mixxx settings directory in
`harness/harness.sqlite3`. Back up that directory with Mixxx closed. Credentials
are separate in `~/.config/mrow/agent.json`; protect or omit them from backups.
The agent never auto-plays or replaces a playing deck. Cloud processing sends
song metadata and crowd assessments, not audio files or local filesystem paths.

## Standalone developer tool (not required on the Pi)

From the repository root:

```sh
python3 harness/src/harness.py
```

Open <http://127.0.0.1:8765> on the same device. State persists in
`~/.mixxx/harness/harness.sqlite3` — on the appliance, beside BiteDJ's own
settings. Use `--database /path/to/file.sqlite3` (or `MROW_HARNESS_DB`) to
select another location. Stop with Ctrl+C. If Musicsearch is already running
on port 8765, use `--port 8766` and open that port instead.

Do not run this alongside Mixxx against the same database. The appliance instead
owns an embedded worker with state in its settings directory, under `harness/`.
The installer retires the old `mrow-harness` service without deleting its history.
The default server accepts local connections only. `--host 0.0.0.0` allows devices
on a trusted LAN; this prototype has no authentication, so anyone who can reach
it can record plays and feedback.

## Try it

1. Expand **Library & playback history** and import `harness/examples/library.json`.
   These are fictional tracks with placeholder paths, for testing only.
2. Use **Record play** on a song. This records history; audio stays in Mixxx.
3. Press **Good**, **Mid**, or **Bad** to record your assessment of the current
   song's crowd response. Changing the rating replaces that play's assessment.
4. Use **Skip suggestion** to exclude an unplayed suggestion from this session.
5. Generate a setlist and export M3U. Real audio paths must be accessible on the
   machine running Mixxx.
6. Change the session name for another performance. Historical learning stays;
   played/skipped exclusions reset for the new session.

## Library compatibility

Import a JSON array, or a Musicsearch-style object with a `tracks` array.
Tracks require a stable string `id`, `title`, numeric `bpm` (20–300), and `path`
or `identifiers.local`. `artist` and `genre` are optional; the first Musicsearch
`genres` entry maps to the display `genre`; all genres remain available for scoring. Reimporting an ID updates metadata without
erasing history. Imports are atomic: invalid records reject the entire import.

Musicsearch records with unknown BPM or without local audio paths need those
fields supplied before import. The harness also preserves duration, Camelot key,
energy, familiarity, vocalness, all genres, model-tagged embeddings, and section
timing/vocalness. It does not read the Musicsearch
database, call providers, scan audio, estimate BPM, or verify file availability.

## Recommendations

The offline foundation is an explainable ranking algorithm.
It combines performance/historical tempo, Camelot compatibility, energy direction,
model-compatible timbre vectors, genre overlap, intro/outro fit, annotated vocal
overlap, historical transition probabilities, smoothed/recency-weighted feedback,
familiarity, and recent artist variety. The musical feature scoring is adapted
from Musicsearch's local implementation and has no runtime dependency on that
checkout. Scores are ranking values, not probabilities of crowd enjoyment.

Each result exposes components, weighted contributions that sum to the score,
feature coverage, and unknown inputs. Missing features get a neutral contribution;
they are never reported as measured compatibility. Exact weights and formulas are
documented in [scoring.md](docs/scoring.md).

Setlists use bounded beam search with the same scorer at every edge. They exclude songs already
played or skipped in the session and never repeat tracks. They can be shorter
than requested when the library or transition constraints cannot support the
requested length within the search budget. Generating/exporting never records
plays. With no history, initial ordering is deterministic and not personalized.

The UI and API apply maximum BPM change (default 12), optional harmonic-only
matching, optional half/double-time interpretation, an explicit target BPM, and
auto/steady/up/down direction. Half/double-time tempo stays consistent across
planned transitions. Harmonic-only rejects unknown keys; compatibility assumes
key lock during tempo changes. These are song-count plans, not duration targets.
No beat/phrase alignment or audio quality is inferred from missing annotations.

Feedback is one operator assessment per play, not automatic crowd sensing or
multi-voter polling. Repeated presses replace the assessment. Suggestion skips
exclude a track for that session but do not imply a global dislike. The web panel
polls every five seconds for external API events and refreshes the shared plan.

## Legacy developer-server environment configuration

Optional. Without one the harness ranks on its own, which is also what happens
whenever the model cannot be reached — expected on a WiFi-only box that moves
between venues. The model never introduces a track: it reorders the candidates
the ranking already allowed and explains its picks, under a timeout.

This applies only to the standalone developer server, not the embedded Mixxx
worker. Supply settings in the developer server's environment.
Google Cloud Gemini and OpenAI- or Anthropic-compatible endpoints work:

```sh
MROW_MODEL_PROVIDER=anthropic     # or: openai
MROW_MODEL=claude-opus-5
MROW_MODEL_API_KEY=...
# Optional:
MROW_MODEL_BASE_URL=https://api.anthropic.com   # openai default: https://api.openai.com/v1
MROW_MODEL_TIMEOUT=10             # seconds, 1-60
MROW_MODEL_CANDIDATES=20          # how many ranked songs the model reorders
MROW_MODEL_EFFORT=low             # Anthropic only; empty to omit
```

Only song metadata is sent — titles, artists, genres, BPM, key, the crowd's
reactions. File paths, drive names and track ids stay on the device.
`/api/recommend` reports which ranking answered (`source`) and why the model
did not (`model_error`); `POST /api/status` reports the configuration.

Use a **Vertex AI express-mode API key** from Google Cloud. AI Studio keys and
standard Vertex project/service-account credentials are not supported by this
API-key flow. Replace any previously provisioned OpenRouter key and model IDs;
existing private configuration files are not migrated automatically.
The model picker supplies bundled suggestions and accepts other Gemini model IDs.
For the developer server, set `MROW_MODEL_PROVIDER=gemini`,
`MROW_MODEL=gemini-2.5-flash`, and `MROW_MODEL_API_KEY` to the Cloud key.
The default endpoint is `https://aiplatform.googleapis.com/v1`.

## Mixxx

BiteDJ drives the harness directly (`src/harness/` in the fork): it reports
plays, syncs each drive's catalog, and shows suggestions with crowd buttons on
its Assist tab. M3U export and the web page remain for other setups. See
[the integration contract](docs/mixxx-integration.md).

## Verify

```sh
python3 -m unittest discover -s harness/tests -v
```

Automated tests exercise scoring, constraints, persistence, migrations, event
deduplication, drive sync and eject, generated features, the model client
against both protocols (including its fallbacks), and an HTTP import → play →
feedback → setlist → export flow. Pi performance, touchscreen interaction, the
GPIO buttons and a real cloud endpoint still require device tests.

## Generate original songs with ElevenLabs

Open **Assist → Generate song**, enter an ElevenLabs API key, choose a duration
(3–600 seconds), direction and whether to require instrumental audio, then press
**Generate & download**. At least one played song must have a Good/Mid/Bad rating.
The optional developer page provides the same controls under **Generate original
music**. No Google Cloud Gemini key is required for this feature.

The prompt aggregates the latest 100 rated plays across sets, including history
from disconnected drives: genre counts for each rating and average performance
BPM for liked songs. Changing a play's rating replaces its contribution; skipping
an unplayed suggestion contributes nothing. Titles, artists, paths and audio are
not sent. This is a musical preference summary, not a claim to understand why the
crowd reacted. Direction steers a new original composition.

The worker calls [ElevenLabs Music compose](https://elevenlabs.io/docs/api-reference/music/compose)
with `music_v1` and MP3 output. Each button press requests one paid generation.
The active key stays in process memory, independently of Google Cloud Gemini settings.
A provisioned `elevenlabs_api_key` loads from the private config at startup;
otherwise it must be reentered after restart. Disconnect removes it for future
jobs in this run; provisioned keys reload after restart, and a generation already
started continues. Settings do not verify credentials until generation.

Generation runs in the background while plays and ratings remain available.
Only one generation may run at a time. Closing/reopening the panel or polling
status does not trigger another request. The captured feedback snapshot is kept
with the job; later feedback affects the next generation. Provider errors and
timeouts are shown without automatic retries, since a failed download may still
have consumed credits. Check ElevenLabs usage before trying again.

MP3s are automatically saved to `generated/` beside `harness.sqlite3`, on the
machine running the harness (not the browser's device). **Open downloads** opens
that folder in the native interface. Files appear as complete only after the
bounded download finishes; empty or non-MP3 responses are rejected. Job history
and saved paths persist through restarts. Interrupted jobs are marked failed.
Import the MP3 into Mixxx and analyze it to obtain BPM/key before it enters the
existing library recommendation flow. Generation never invents measured features,
loads a deck, or starts playback.

The private-worker and localhost developer HTTP commands are:

- `POST /api/agent/music/settings`: `{api_key}` or `{disconnect: true}`; empty
  object reads configuration without returning the key.
- `POST /api/agent/music/generate`: `{session, duration_seconds, direction,
  instrumental}`; direction is `follow crowd`, `build`, `hold`, or `ease down`.
  Returns a job ID immediately.
- `POST /api/agent/music/view`: returns configuration and the latest 20 jobs with
  `generating`, `complete`, or `failed` state, plus local paths/errors.

Tests mock the provider: `python3 -m unittest discover -s harness/tests -v`.
Live API billing/audio quality and native touchscreen interaction require a
configured account and device testing.
