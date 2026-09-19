# MROW DJ harness

A local DJ assistant intended for Raspberry Pi 4 and 5. Requires Python 3.9+;
no third-party Python packages, cloud account, or model download.

## Run

From the repository root:

```sh
python3 harness/src/harness.py
```

Open <http://127.0.0.1:8765> on the same device. State persists in
`harness/data/harness.sqlite3` (ignored by Git). Use `--database /path/to/file.sqlite3`
to select another location. Stop with Ctrl+C. If Musicsearch is already running
on port 8765, use `--port 8766` and open that port instead.

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

## Mixxx

M3U export and a local HTTP API are implemented. Automatic Mixxx events, deck
loading, and embedded skin buttons are not implemented. See
[the integration contract](docs/mixxx-integration.md).

## Verify

```sh
python3 -m unittest discover -s harness/tests -v
```

Automated tests exercise scoring, constraints, persistence, migrations, event
deduplication, and an HTTP import → play → feedback → setlist → export flow. Pi performance,
touchscreen interaction, and Mixxx integration still require device tests.
