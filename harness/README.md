# MROW DJ harness

A local DJ assistant intended for Raspberry Pi 4 and 5. Requires Python 3.9+;
no third-party Python packages, cloud account, or model download.

## Run

From the repository root:

```sh
python3 harness/src/harness.py
```

Open <http://127.0.0.1:8765> on the same device. State persists in
`~/.mixxx/harness/harness.sqlite3` — on the appliance, beside BiteDJ's own
settings. Use `--database /path/to/file.sqlite3` (or `MROW_HARNESS_DB`) to
select another location. Stop with Ctrl+C. If Musicsearch is already running
on port 8765, use `--port 8766` and open that port instead.

On the appliance this runs as a systemd user service beside BiteDJ; see
[RPI/README.md](../RPI/README.md). The web page stays available there as a
debug view — the DJ uses the Assist tab in BiteDJ.

On the Pi, use Raspberry Pi OS with Python 3.9+ and open the page in its browser.
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

The initial agent is an explainable ranking algorithm, not a language model.
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
exclude a track for that session but do not imply a global dislike. The panel
polls every five seconds for external API events and invalidates stale setlists.

## The cloud model

Optional. Without one the harness ranks on its own, which is also what happens
whenever the model cannot be reached — expected on a WiFi-only box that moves
between venues. The model never introduces a track: it reorders the candidates
the ranking already allowed and explains its picks, under a timeout.

Configure it in `~/.config/mrow/harness.env` (mode 600, never committed), or in
the environment. Any OpenAI- or Anthropic-compatible endpoint works:

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
