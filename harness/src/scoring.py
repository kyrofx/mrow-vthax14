"""Offline transition scoring, adapted from the local Musicsearch engine.

Scores are transparent heuristics, not calibrated audience probabilities.
Unknown features remain unknown. All policy inputs are validated here so the
UI, HTTP clients, and set planner use identical rules.
"""
import math
import re
from collections import Counter, defaultdict


def number(value, name, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f'{name} must be a number')
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f'{name} must be between {low} and {high}')
    return float(value)


def features(track):
    """Validate and preserve the Musicsearch fields used by this scorer."""
    result = {}
    for name in ('energy', 'vocalness', 'familiarity'):
        result[name] = None if track.get(name) is None else number(track[name], name, 0, 1)
    result['duration'] = None if track.get('duration') is None else number(track['duration'], 'duration', 0.01, 86400)
    key = track.get('camelot')
    if key is not None and (not isinstance(key, str) or not re.fullmatch(r'(?:[1-9]|1[0-2])[AB]', key)):
        raise ValueError('camelot must be 1A–12A or 1B–12B')
    result['camelot'] = key
    genres = track.get('genres', [track['genre']] if track.get('genre') else [])
    if not isinstance(genres, list) or any(not isinstance(g, str) for g in genres):
        raise ValueError('genres must be an array of strings')
    result['genres'] = sorted({g.strip().lower().replace('hip-hop', 'hip hop') for g in genres if g.strip()})
    vector = track.get('embedding')
    if vector is not None:
        if not isinstance(vector, dict) or not isinstance(vector.get('model'), str) or not vector['model']:
            raise ValueError('embedding requires a model name')
        values = vector.get('values')
        if not isinstance(values, list) or not 2 <= len(values) <= 4096:
            raise ValueError('embedding requires 2–4096 numeric values')
        vector = {'model': vector['model'], 'values': [number(v, 'embedding value', -1e10, 1e10) for v in values]}
    result['embedding'] = vector
    sections = track.get('sections', [])
    if not isinstance(sections, list) or len(sections) > 1000:
        raise ValueError('sections must be an array of at most 1000 entries')
    result['sections'] = []
    for section in sections:
        if not isinstance(section, dict) or section.get('kind') not in ('intro', 'outro', 'verse', 'chorus', 'build', 'drop', 'breakdown', 'unknown'):
            raise ValueError('Invalid section kind')
        start = number(section.get('start'), 'section start', 0, 86400)
        end = number(section.get('end'), 'section end', 0, 86400)
        if end <= start or (result['duration'] is not None and end > result['duration']):
            raise ValueError('Section must have positive length and fit inside duration')
        vocal = section.get('vocalness')
        result['sections'].append({'kind': section['kind'], 'start': start, 'end': end,
                                   'vocalness': None if vocal is None else number(vocal, 'section vocalness', 0, 1)})
    return result


def policy(options=None):
    defaults = {'max_bpm_delta': 12.0, 'allow_half_double': True,
                'harmonic_only': False, 'direction': 'auto', 'target_bpm': None}
    if options is None:
        return defaults
    if not isinstance(options, dict) or set(options) - set(defaults):
        raise ValueError('Unknown scoring options')
    defaults.update(options)
    defaults['max_bpm_delta'] = number(defaults['max_bpm_delta'], 'max_bpm_delta', 0, 100)
    for name in ('allow_half_double', 'harmonic_only'):
        if type(defaults[name]) is not bool:
            raise ValueError(name + ' must be boolean')
    if defaults['direction'] not in ('auto', 'steady', 'up', 'down'):
        raise ValueError('direction must be auto, steady, up, or down')
    if defaults['target_bpm'] is not None:
        defaults['target_bpm'] = number(defaults['target_bpm'], 'target_bpm', 20, 300)
    return defaults


def harmonic(a, b):
    if not a or not b:
        return None
    gap = abs(int(a[:-1]) - int(b[:-1]))
    gap = min(gap, 12 - gap)
    if a == b:
        return 1.0
    if (gap == 1 and a[-1] == b[-1]) or (gap == 0 and a[-1] != b[-1]):
        return 0.9
    return 0.45 if gap == 2 and a[-1] == b[-1] else 0.1


def effective_bpm(bpm, reference, half_double):
    choices = [bpm] + ([bpm / 2, bpm * 2] if half_double else [])
    return min(choices, key=lambda value: abs(value - reference))


def cosine(a, b):
    if not a or not b or a['model'] != b['model'] or len(a['values']) != len(b['values']):
        return None
    av, bv = a['values'], b['values']
    norm = math.sqrt(sum(v*v for v in av) * sum(v*v for v in bv))
    return None if norm == 0 else max(0, min(1, (sum(x*y for x,y in zip(av,bv))/norm + 1)/2))


WEIGHTS = {'tempo': 0.26, 'harmonic': 0.09, 'energy': 0.09, 'timbre': 0.06,
           'genre': 0.05, 'intro_outro': 0.04, 'vocal_safety': 0.03,
           'history': 0.13, 'crowd': 0.15, 'familiarity': 0.04, 'artist_variety': 0.06}


class Ranker:
    """One immutable database snapshot per recommendation or complete setlist."""
    def __init__(self, tracks, plays, votes, session, options=None):
        self.options = policy(options)
        self.session = session
        self.votes = votes
        self.tracks = {t['id']: t for t in tracks}
        self.current = [p for p in plays if p['session'] == session]
        self.excluded = {p['track_id'] for p in self.current}
        self.excluded.update(v['track_id'] for v in votes if v['session'] == session and v['rating'] == 'skip')
        self.popularity = Counter(p['track_id'] for p in plays)
        self.transitions = Counter()
        self.outgoing = Counter()
        last = {}
        deltas = []
        for p in plays:
            before = last.get(p['session'])
            if before and before['track_id'] != p['track_id']:
                pair = (before['track_id'], p['track_id'])
                self.transitions[pair] += 1
                self.outgoing[before['track_id']] += 1
                aligned = effective_bpm(p['bpm'], before['bpm'], self.options['allow_half_double'])
                deltas.append(max(-4, min(4, aligned - before['bpm'])))
            last[p['session']] = p
        self.trend = sum(deltas[-20:]) / len(deltas[-20:]) if deltas else 0
        self.seed_bpm = plays[-1]['bpm'] if plays else None
        # Legacy duplicate ratings collapse to the most recent assessment per play.
        latest = {v['play_id']: v for v in votes if v['play_id'] is not None and v['rating'] != 'skip'}
        self.feedback = defaultdict(list)
        self.genre_feedback = defaultdict(list)
        ordered = sorted(latest.values(), key=lambda v: v['id'])
        for index, vote in enumerate(ordered):
            value = {'good': 1, 'mid': 0, 'bad': -1}[vote['rating']]
            weight = 0.5 ** ((len(ordered) - 1 - index) / 100)
            self.feedback[vote['track_id']].append((value, weight))
            for genre in self.tracks[vote['track_id']].get('genres', []):
                self.genre_feedback[genre].append((value, weight))
        self.current_rating = latest.get(self.current[-1]['id'], {}).get('rating') if self.current else None

    @staticmethod
    def preference(observations):
        return sum(value * weight for value, weight in observations) / (3 + sum(weight for _, weight in observations))

    def rank(self, planned=(), limit=5):
        used = self.excluded | {t['id'] for t in planned}
        previous = planned[-1] if planned else (self.tracks[self.current[-1]['track_id']] if self.current else None)
        actual = planned[-1]['effective_bpm'] if planned else (self.current[-1]['bpm'] if self.current else self.seed_bpm)
        direction = self.options['direction']
        change = {'up': 2, 'down': -2, 'steady': 0, 'auto': self.trend}[direction]
        if direction == 'auto' and self.current_rating == 'bad':
            change = min(change, -2)
        target = self.options['target_bpm']
        if target is None and actual is not None:
            target = max(20, min(300, actual + change))
        results = []
        for t in self.tracks.values():
            # Tracks on an ejected drive stay in history but cannot be suggested.
            if t['id'] in used or not t.get('available', True):
                continue
            effective = effective_bpm(t['bpm'], actual or target or t['bpm'], self.options['allow_half_double'])
            delta = effective - actual if previous and actual is not None else None
            key = harmonic(previous.get('camelot'), t.get('camelot')) if previous else None
            if delta is not None and abs(delta) > self.options['max_bpm_delta']:
                continue
            if self.options['harmonic_only'] and (not t.get('camelot') or (previous and (key is None or key < 0.9))):
                continue
            left_genres = set(previous.get('genres', [])) if previous else set()
            right_genres = set(t.get('genres', []))
            energy = None
            if previous and previous.get('energy') is not None and t.get('energy') is not None:
                shift = {'up': .1, 'down': -.1, 'steady': 0, 'auto': .03 if change > 0 else -.03 if change < 0 else 0}[direction]
                energy = max(0, 1 - abs(t['energy'] - max(0, min(1, previous['energy'] + shift))) * 3)
            incoming = next((s for s in t.get('sections', []) if s['kind']=='intro' and s['start']==0), None)
            outgoing = next((s for s in reversed(previous.get('sections', [])) if s['kind']=='outro' and previous.get('duration') is not None and abs(s['end']-previous['duration'])<1), None) if previous else None
            vocals = None
            if incoming and outgoing and incoming['vocalness'] is not None and outgoing['vocalness'] is not None:
                vocals = 1 - min(incoming['vocalness'], outgoing['vocalness'])
            transition_count = self.transitions[(previous['id'], t['id'])] if previous else 0
            total = self.outgoing[previous['id']] if previous else 0
            own = self.preference(self.feedback[t['id']])
            related = [self.preference(self.genre_feedback[g]) for g in right_genres if self.genre_feedback[g]]
            crowd = .5 + .5 * (0.8 * own + 0.2 * (sum(related)/len(related) if related else 0))
            recent = [self.tracks[p['track_id']]['artist'].strip().casefold() for p in self.current[-3:]]
            recent = (recent + [p['artist'].strip().casefold() for p in planned])[-3:]
            artist = t['artist'].strip().casefold()
            components = {
                'tempo': math.exp(-abs(effective-target)/8) if target is not None else None,
                'harmonic': key, 'energy': energy,
                'timbre': cosine(previous.get('embedding'), t.get('embedding')) if previous else None,
                'genre': (1 if left_genres & right_genres else .25) if left_genres and right_genres else None,
                'intro_outro': min(1, min(incoming['end'], outgoing['end']-outgoing['start'])/32) if incoming and outgoing else None,
                'vocal_safety': vocals,
                'history': (transition_count + 1) / (total + 2) if total else None,
                'crowd': crowd if self.feedback[t['id']] or related else None,
                'familiarity': t.get('familiarity') if t.get('familiarity') is not None else self.popularity[t['id']] / (self.popularity[t['id']]+5),
                'artist_variety': 1 - recent.count(artist)/3 if artist else None,
            }
            denominator = sum(WEIGHTS.values())
            contributions = {k: WEIGHTS[k] * (.5 if v is None else v) / denominator for k,v in components.items()}
            score = sum(contributions.values())
            reasons = [f'{effective:g} effective BPM; target {target:.1f}' if target is not None else 'Cold start: no tempo history']
            if transition_count:
                reasons.append(f'Chosen after the previous song {transition_count} time(s) out of {total}')
            if components['crowd'] is not None:
                reasons.append(f'Crowd preference {crowd:.2f}; song and related-genre feedback')
            if key is not None:
                reasons.append(f'Camelot {previous["camelot"]} → {t["camelot"]}: {key:.2f}')
            if artist and artist in recent:
                reasons.append('Recent artist repetition penalty applied')
            results.append({**t, 'score': score, 'effective_bpm': effective, 'bpm_delta': delta,
                            'target_bpm': target, 'components': components, 'contributions': contributions,
                            'coverage': sum(WEIGHTS[k] for k,v in components.items() if v is not None)/denominator,
                            'unknown': [k for k,v in components.items() if v is None], 'reasons': reasons})
        return sorted(results, key=lambda t: (-t['score'], t['id']))[:limit]

    def plan(self, count):
        # Bounded beam search avoids a greedy dead end; every edge uses rank().
        beam = [(0.0, [])]
        finished = []
        for _ in range(count):
            expanded = []
            for utility, path in beam:
                candidates = self.rank(path, limit=6)
                if not candidates:
                    finished.append((utility, path))
                for t in candidates:
                    expanded.append((utility+t['score'], path+[t]))
            if not expanded:
                break
            beam = sorted(expanded, key=lambda pair: (-pair[0], [t['id'] for t in pair[1]]))[:8]
        finished.extend(beam)
        _, tracks = max(finished, key=lambda pair: (len(pair[1]), pair[0]))
        return {'tracks': tracks, 'requested': count, 'returned': len(tracks),
                'complete': len(tracks)==count, 'options': self.options,
                'notes': ['Bounded search; not a proof of the optimal sequence.'] +
                         ([] if len(tracks)==count else ['No longer sequence found within the constraints and search budget.'])}
