# Scoring contract

All processing is local. This is an explainable preference heuristic, not a
trained model or a claim of perfect musical judgment. Use listening tests to
calibrate weights against the actual library and DJ's decisions.

## Shared path

Import validates and stores rich features → a consistent SQLite snapshot builds
session/history statistics → Ranker scores eligible candidates → recommendations
or bounded beam search → UI explanations → server-generated M3U export.
Set planning does not write history. Both callers use the same scoring policy.

## Components

Weights are normalized by their total (1.10). Values are in [0,1]. An unavailable
feature contributes its neutral midpoint (0.5), stays null in `components`, and
reduces reported coverage. Coverage is available weight / total weight; it is not
a statistical confidence interval. `contributions` sum exactly to `score`.

| Component | Weight | Behavior |
| --- | --- | --- |
| Tempo | .26 | exp(-absolute effective BPM gap to target / 8) |
| Harmonic | .09 | Same Camelot key 1; adjacent same-letter or relative A/B .9; distance-two same-letter .45; other .1 |
| Energy | .09 | Closeness to previous energy plus desired energy change |
| Timbre | .06 | Cosine mapped to [0,1]; requires matching model and dimensions, nonzero vectors |
| Genre | .05 | Any normalized shared genre 1; known disjoint genres .25 |
| Intro/outro | .04 | Shorter annotated intro/outro length / 32 seconds, capped at 1 |
| Vocal safety | .03 | 1 minus the lesser of annotated outgoing/incoming vocalness |
| History | .13 | (Observed pair count + 1) / (all outgoing choices from source + 2); unknown without outgoing history |
| Crowd | .15 | Smoothed song feedback (80%) and related-genre feedback (20%), mapped to [0,1] |
| Familiarity | .04 | Imported familiarity, else play count / (play count + 5) |
| Artist variety | .06 | 1 minus occurrences in last three actual/planned tracks / 3 |

Good/mid/bad values are +1/0/-1. Only the latest assessment for each play counts,
including old prototype databases with duplicate rows. Evidence decays with a
half-life of 100 later assessments. A prior of three neutral observations shrinks
sparse evidence. Skip is a session exclusion only. Related-genre transfer is a
weak heuristic, not evidence that all songs in a genre get the same reaction.

History transitions never cross session boundaries and ignore immediate repeats.
Auto tempo trend averages the latest 20 aligned historical deltas, each clamped
to ±4 BPM. A bad assessment on the current song makes auto direction ease down
by at least 2 BPM; this is an explicit recovery heuristic, not a measured crowd
preference for slower songs. Steady/up/down overrides auto with 0/+2/-2 BPM and
0/+.1/-.1 energy targets. An explicit target BPM overrides the tempo target.

## Constraints and planning

Played and skipped songs are excluded for the session. The maximum BPM delta is
checked against the previous performance tempo, not the target. Half/double-time
alignment is optional and propagates its effective tempo through the entire plan.
Harmonic-only rejects unknown/incompatible keys. Reported key compatibility
assumes key lock; it does not predict pitch changes when key lock is off.

Beam search keeps eight sequences and expands up to six successors per sequence.
It prefers requested length before total score and reports shortfalls instead of
relaxing constraints. Search is bounded, not globally optimal. There is no fixed
duration objective, audio loading, automatic beatmatching, phrase alignment, or
automatic crowd sensing. Intro/outro length and vocal annotations are proxies;
their presence does not prove a mix is playable.
