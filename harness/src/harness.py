"""Local DJ assistant. Python 3.9+, standard library only."""

import argparse
import json
import math
import os
import sqlite3
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from model import ModelClient, ModelError, load_config
from scoring import Ranker, features

# On the device, next to Mixxx's own settings (see docs/mixxx-integration-plan.md).
DEFAULT_DATABASE = Path('~/.mixxx/harness/harness.sqlite3').expanduser()

# Features a separate metadata model may supply later. Kept apart from what
# Mixxx syncs so a library sync never overwrites them.
GENERATED_FEATURES = ('energy', 'vocalness', 'familiarity', 'embedding', 'sections')

TRACKS = """SELECT tracks.*, track_features.features AS generated,
                   track_features.source AS feature_source
            FROM tracks LEFT JOIN track_features ON track_features.track_id = tracks.id"""


class Harness:
    def __init__(self, database, model=None):
        self.database = str(database)
        self.model = model
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS tracks (
                    id TEXT PRIMARY KEY, title TEXT NOT NULL, artist TEXT NOT NULL,
                    bpm REAL NOT NULL, path TEXT NOT NULL, genre TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS plays (
                    id INTEGER PRIMARY KEY, track_id TEXT NOT NULL REFERENCES tracks(id),
                    session TEXT NOT NULL, bpm REAL NOT NULL,
                    created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );
                CREATE TABLE IF NOT EXISTS feedback (
                    id INTEGER PRIMARY KEY, track_id TEXT NOT NULL REFERENCES tracks(id),
                    play_id INTEGER REFERENCES plays(id), session TEXT NOT NULL,
                    rating TEXT NOT NULL CHECK(rating IN ('good','mid','bad','skip')),
                    created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );
            """)
            # Additive migrations preserve the first prototype's library/history.
            if 'features' not in {r['name'] for r in db.execute('PRAGMA table_info(tracks)')}:
                db.execute("ALTER TABLE tracks ADD COLUMN features TEXT NOT NULL DEFAULT '{}'")
            if 'event_id' not in {r['name'] for r in db.execute('PRAGMA table_info(plays)')}:
                db.execute('ALTER TABLE plays ADD COLUMN event_id TEXT')
            db.execute('CREATE UNIQUE INDEX IF NOT EXISTS play_events ON plays(event_id) WHERE event_id IS NOT NULL')
            columns = {r['name'] for r in db.execute('PRAGMA table_info(tracks)')}
            # scope: the drive a synced track lives on; available: whether that
            # drive is mounted. Unavailable tracks keep their history but are
            # never suggested.
            if 'scope' not in columns:
                db.execute('ALTER TABLE tracks ADD COLUMN scope TEXT')
            if 'available' not in columns:
                db.execute('ALTER TABLE tracks ADD COLUMN available INTEGER NOT NULL DEFAULT 1')
            db.execute('CREATE INDEX IF NOT EXISTS track_scopes ON tracks(scope)')
            db.execute("""CREATE TABLE IF NOT EXISTS track_features (
                track_id TEXT PRIMARY KEY REFERENCES tracks(id), source TEXT NOT NULL,
                features TEXT NOT NULL, updated TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)""")

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.database)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA foreign_keys=ON')
        try:
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def identifier(value, name='track_id'):
        if not isinstance(value, str) or not value.strip() or len(value) > 500:
            raise ValueError(name + ' must be a nonempty string of at most 500 characters')
        return value

    @staticmethod
    def decode(row):
        track = dict(row)
        metadata = json.loads(track.pop('features'))
        generated = json.loads(track.pop('generated', None) or '{}')
        track['available'] = bool(track.get('available', 1))
        return {**features(track), **metadata, **generated, **track}

    @staticmethod
    def bpm(value):
        if isinstance(value, bool):
            raise ValueError('BPM must be a number between 20 and 300')
        value = float(value)
        if not math.isfinite(value) or not 20 <= value <= 300:
            raise ValueError('BPM must be between 20 and 300')
        return value

    @staticmethod
    def session(value):
        if not isinstance(value, str) or not value.strip() or len(value) > 100:
            raise ValueError('Session must be a nonempty string of at most 100 characters')
        return value.strip()

    def track_row(self, track):
        """Validate one track into (id, title, artist, bpm, path, genre, features)."""
        if not isinstance(track, dict):
            raise ValueError('Each track must be an object')
        track = dict(track)
        if 'path' not in track:
            identifiers = track.get('identifiers', {})
            if not isinstance(identifiers, dict):
                raise ValueError('Track identifiers must be an object')
            track['path'] = identifiers.get('local')
        if 'genre' not in track:
            genres = track.get('genres', [])
            if not isinstance(genres, list) or any(not isinstance(g, str) for g in genres):
                raise ValueError('Track genres must be an array of strings')
            track['genre'] = genres[0] if genres else ''
        for key in ('id', 'title', 'path'):
            if not isinstance(track.get(key), str) or not track[key].strip():
                raise ValueError('Each track needs a nonempty id, title, and path')
        self.identifier(track['id'])
        if '\n' in track['path'] or '\r' in track['path'] or track['path'].startswith('#'):
            raise ValueError('Track path cannot contain newlines or start with #')
        if track.get('bpm') is None:
            raise ValueError('Track ' + track['id'] + ' needs an analyzed or supplied BPM before import')
        for key in ('artist', 'genre'):
            if not isinstance(track.get(key, ''), str):
                raise ValueError(key + ' must be text')
        return (track['id'], track['title'], track.get('artist', ''),
                self.bpm(track.get('bpm')), track['path'], track.get('genre', ''),
                json.dumps(features(track), allow_nan=False))

    def import_library(self, tracks):
        if isinstance(tracks, dict):
            tracks = tracks.get('tracks')
        if not isinstance(tracks, list) or not tracks:
            raise ValueError('Provide a nonempty tracks array')
        rows = [self.track_row(track) for track in tracks]
        if len({row[0] for row in rows}) != len(rows):
            raise ValueError('Track IDs must be unique within an import')
        with self.connect() as db:
            db.executemany('''INSERT INTO tracks(id,title,artist,bpm,path,genre,features) VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(id) DO UPDATE SET title=excluded.title, artist=excluded.artist,
                bpm=excluded.bpm, path=excluded.path, genre=excluded.genre, features=excluded.features''', rows)
        return {'imported': len(rows)}

    @staticmethod
    def scope(value):
        if not isinstance(value, str) or not value.strip() or len(value) > 200:
            raise ValueError('scope must be a nonempty string of at most 200 characters')
        return value

    def sync_library(self, scope, tracks, complete=False):
        """Upsert the tracks Mixxx found on one drive (`scope`).

        Unlike import, a sync is lenient: a track Mixxx has not analyzed yet
        (no BPM) is skipped and reported rather than failing the whole drive.
        With `complete`, tracks of this drive missing from the sync are marked
        unavailable; their history is kept.
        """
        scope = self.scope(scope)
        if not isinstance(tracks, list) or len(tracks) > 50_000:
            raise ValueError('tracks must be an array of at most 50000 entries')
        if type(complete) is not bool:
            raise ValueError('complete must be boolean')
        rows, skipped, seen = [], [], set()
        for track in tracks:
            try:
                row = self.track_row(track)
            except (ValueError, TypeError) as error:
                track_id = track.get('id') if isinstance(track, dict) else None
                skipped.append({'id': track_id if isinstance(track_id, str) else None, 'error': str(error)})
                continue
            if row[0] not in seen:
                seen.add(row[0])
                rows.append(row + (scope,))
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            db.executemany('''INSERT INTO tracks(id,title,artist,bpm,path,genre,features,scope,available)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)
                ON CONFLICT(id) DO UPDATE SET title=excluded.title, artist=excluded.artist,
                bpm=excluded.bpm, path=excluded.path, genre=excluded.genre, features=excluded.features,
                scope=excluded.scope, available=1''', rows)
            unavailable = 0
            if complete:
                db.execute('CREATE TEMP TABLE IF NOT EXISTS synced(id TEXT PRIMARY KEY)')
                db.execute('DELETE FROM synced')
                db.executemany('INSERT INTO synced(id) VALUES (?)', [(row[0],) for row in rows])
                unavailable = db.execute('''UPDATE tracks SET available=0 WHERE scope=? AND available=1
                    AND id NOT IN (SELECT id FROM synced)''', (scope,)).rowcount
        return {'synced': len(rows), 'skipped_count': len(skipped), 'skipped': skipped[:20],
                'unavailable': unavailable}

    def upsert_played_track(self, scope, track, bpm=None):
        """Make sure a track Mixxx just played is known, before recording the play.

        The deck's BPM stands in when the file has none analyzed. A track that
        still cannot be stored is left to record_play, which reports it.
        """
        if not isinstance(track, dict):
            raise ValueError('track must be an object')
        if track.get('bpm') is None and bpm is not None:
            track = {**track, 'bpm': bpm}
        self.sync_library(scope, [track])

    def set_unavailable(self, scope):
        """A drive was ejected: stop suggesting its tracks, keep their history."""
        scope = self.scope(scope)
        with self.connect() as db:
            count = db.execute('UPDATE tracks SET available=0 WHERE scope=? AND available=1', (scope,)).rowcount
        return {'unavailable': count}

    def store_features(self, tracks):
        """Store generated features (energy, vocals, timbre, sections...) per track.

        For a future metadata model. Only the fields in GENERATED_FEATURES are
        accepted; they are validated like imported features and override what
        a sync provides, and a later sync never overwrites them.
        """
        if not isinstance(tracks, list) or not 1 <= len(tracks) <= 10_000:
            raise ValueError('tracks must be an array of 1–10000 entries')
        rows = []
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            for entry in tracks:
                if not isinstance(entry, dict):
                    raise ValueError('Each entry must be an object')
                track_id = self.identifier(entry.get('id'))
                source = entry.get('source')
                if not isinstance(source, str) or not source.strip() or len(source) > 200:
                    raise ValueError('source must name the model that generated the features')
                unknown = set(entry) - {'id', 'source', *GENERATED_FEATURES}
                if unknown:
                    raise ValueError('Unsupported feature fields: ' + ', '.join(sorted(unknown)))
                track = db.execute('SELECT features FROM tracks WHERE id=?', (track_id,)).fetchone()
                if track is None:
                    raise ValueError('Unknown track ' + track_id)
                duration = json.loads(track['features']).get('duration')
                validated = features({'duration': duration, **{k: entry[k] for k in GENERATED_FEATURES if k in entry}})
                supplied = {k: validated[k] for k in GENERATED_FEATURES if k in entry}
                rows.append((track_id, source, json.dumps(supplied, allow_nan=False)))
            db.executemany('''INSERT INTO track_features(track_id, source, features) VALUES (?, ?, ?)
                ON CONFLICT(track_id) DO UPDATE SET source=excluded.source, features=excluded.features,
                updated=CURRENT_TIMESTAMP''', rows)
        return {'stored': len(rows)}

    def state(self, session):
        session = self.session(session)
        with self.connect() as db:
            tracks = [self.decode(r) for r in db.execute(TRACKS + ' ORDER BY artist,title')]
            plays = [dict(r) for r in db.execute('SELECT * FROM plays WHERE session=? ORDER BY id', (session,))]
            feedback = [dict(r) for r in db.execute('SELECT * FROM feedback WHERE session=? ORDER BY id', (session,))]
        return {'tracks': tracks, 'plays': plays, 'feedback': feedback, 'session': session}

    def record_play(self, track_id, session, bpm=None, event_id=None):
        self.identifier(track_id)
        session = self.session(session)
        if event_id is not None:
            self.identifier(event_id, 'event_id')
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            track = db.execute('SELECT * FROM tracks WHERE id=?', (track_id,)).fetchone()
            if track is None:
                raise ValueError('Unknown track')
            actual_bpm = track['bpm'] if bpm is None else self.bpm(bpm)
            if event_id is not None:
                existing = db.execute('SELECT * FROM plays WHERE event_id=?', (event_id,)).fetchone()
                if existing:
                    if existing['track_id'] != track_id or existing['session'] != session or existing['bpm'] != actual_bpm:
                        raise ValueError('event_id already used for a different play')
                    return {'play_id': existing['id'], 'duplicate': True}
            cursor = db.execute('INSERT INTO plays(track_id,session,bpm,event_id) VALUES (?,?,?,?)',
                                (track_id, session, actual_bpm, event_id))
            return {'play_id': cursor.lastrowid}

    def rate(self, track_id, session, rating, play_id=None):
        self.identifier(track_id)
        session = self.session(session)
        if play_id is not None and (type(play_id) is not int or play_id < 1):
            raise ValueError('play_id must be a positive integer')
        if rating not in ('good', 'mid', 'bad', 'skip'):
            raise ValueError('Rating must be good, mid, bad, or skip')
        if rating != 'skip' and (type(play_id) is not int):
            raise ValueError('Crowd feedback requires a recorded play_id')
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            if not db.execute('SELECT 1 FROM tracks WHERE id=?', (track_id,)).fetchone():
                raise ValueError('Unknown track')
            if play_id is not None and not db.execute(
                'SELECT 1 FROM plays WHERE id=? AND track_id=? AND session=?',
                (play_id, track_id, session)).fetchone():
                raise ValueError('Feedback must match the track and session of its play')
            if rating == 'skip':
                if play_id is not None:
                    raise ValueError('Suggestion skip must not reference a play')
                if db.execute('SELECT 1 FROM plays WHERE track_id=? AND session=?', (track_id, session)).fetchone():
                    raise ValueError('Cannot skip a song already recorded in this session')
                db.execute("DELETE FROM feedback WHERE track_id=? AND session=? AND rating='skip'", (track_id, session))
            else:
                db.execute('DELETE FROM feedback WHERE play_id=?', (play_id,))
            db.execute('INSERT INTO feedback(track_id,play_id,session,rating) VALUES (?,?,?,?)',
                       (track_id, play_id, session, rating))
        return {'saved': True}

    def ranker(self, session, options=None):
        session = self.session(session)
        with self.connect() as db:
            db.execute('BEGIN')
            tracks = [self.decode(r) for r in db.execute(TRACKS + ' ORDER BY tracks.id')]
            plays = [dict(r) for r in db.execute('SELECT * FROM plays ORDER BY id')]
            votes = [dict(r) for r in db.execute('SELECT * FROM feedback ORDER BY id')]
        return Ranker(tracks, plays, votes, session, options)

    @staticmethod
    def count(value):
        if type(value) is not int or not 1 <= value <= 100:
            raise ValueError('Count must be between 1 and 100')
        return value

    def recommend(self, session, count=5, planned=None, options=None):
        return self.ranker(session, options).rank(planned or [], self.count(count))

    def suggest(self, session, count=5, options=None, use_model=True):
        """Recommendations for the panel: the heuristic's picks, reordered by
        the model when one is configured and reachable.

        `source` says which one answered. On any model failure the heuristic
        order is returned with the reason in `model_error`; a suggestion is
        never withheld because the network is down.
        """
        count = self.count(count)
        ranker = self.ranker(session, options)
        width = max(count, self.model.config.candidates) if self.model and use_model else count
        candidates = ranker.rank([], width)
        result = {'tracks': candidates[:count], 'source': 'heuristic', 'model_error': None}
        if not (self.model and use_model):
            return result
        if not candidates:
            return result
        latest = {v['play_id']: v['rating'] for v in ranker.votes if v['play_id'] is not None}
        played = [{**ranker.tracks[p['track_id']], 'rating': latest.get(p['id'])} for p in ranker.current[-8:]]
        cache_key = (ranker.session, tuple((t['id'], t['rating']) for t in played),
                     tuple(t['id'] for t in candidates), count)
        try:
            picks = self.model.rerank(played, candidates, count, cache_key)
        except ModelError as error:
            result['model_error'] = str(error)
            return result
        by_id = {t['id']: t for t in candidates}
        chosen = [{**by_id[track_id], 'model_reason': reason} for track_id, reason in picks]
        rest = [t for t in candidates if t['id'] not in {track_id for track_id, _ in picks}]
        result['tracks'] = (chosen + rest)[:count]
        result['source'] = 'model'
        return result

    def status(self):
        with self.connect() as db:
            available = db.execute('SELECT COUNT(*) FROM tracks WHERE available=1').fetchone()[0]
        return {'ok': True, 'tracks_available': available,
                'model': self.model.config.describe() if self.model else None}

    def setlist(self, session, count, options=None):
        return self.ranker(session, options).plan(self.count(count))

    def export(self, track_ids):
        if not isinstance(track_ids, list) or not track_ids or len(track_ids) > 100:
            raise ValueError('Export requires 1–100 track IDs')
        for track_id in track_ids:
            self.identifier(track_id)
        if len(set(track_ids)) != len(track_ids):
            raise ValueError('Export cannot contain duplicate tracks')
        with self.connect() as db:
            tracks = {r['id']: self.decode(r) for r in db.execute(TRACKS)}
        if any(t not in tracks for t in track_ids):
            raise ValueError('Export contains an unknown track')
        lines = ['#EXTM3U']
        for track_id in track_ids:
            t = tracks[track_id]
            label = (t['artist'] + ' - ' + t['title']).replace(chr(10), ' ').replace(chr(13), ' ')
            lines.extend([f'#EXTINF:{int(t["duration"]) if t["duration"] else -1},{label}', t['path']])
        return {'filename': 'mrow-setlist.m3u', 'content': '\n'.join(lines) + '\n'}


def make_handler(harness):
    class Handler(BaseHTTPRequestHandler):
        def send(self, status, payload, content_type='application/json'):
            data = json.dumps(payload).encode() if content_type == 'application/json' else payload
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == '/':
                self.send(200, Path(__file__).with_name('index.html').read_bytes(), 'text/html; charset=utf-8')
            elif self.path == '/app.js':
                self.send(200, Path(__file__).with_name('app.js').read_bytes(), 'text/javascript; charset=utf-8')
            elif self.path == '/health':
                self.send(200, {'ok': True})
            else:
                self.send(404, {'error': 'Not found'})

        def do_POST(self):
            origin = self.headers.get('Origin')
            if origin and urlparse(origin).netloc != self.headers.get('Host'):
                self.send(403, {'error': 'Cross-origin requests are disabled'})
                return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 5_000_000:
                    raise ValueError('Request must contain at most 5 MB of JSON')
                if self.headers.get_content_type() != 'application/json':
                    raise ValueError('Use application/json')
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict):
                    raise ValueError('Request must be a JSON object')
                session = data.get('session', 'default')
                if self.path == '/api/library':
                    result = harness.import_library(data.get('tracks'))
                elif self.path == '/api/state':
                    result = harness.state(session)
                elif self.path == '/api/play':
                    if data.get('track') is not None:
                        # Mixxx sends the track with its play, so a file from a
                        # drive without a synced catalog can still be recorded.
                        harness.upsert_played_track(data.get('scope'), data['track'], data.get('bpm'))
                    result = harness.record_play(data.get('track_id'), session, data.get('bpm'), data.get('event_id'))
                elif self.path == '/api/feedback':
                    result = harness.rate(data.get('track_id'), session, data.get('rating'), data.get('play_id'))
                elif self.path == '/api/recommend':
                    result = harness.suggest(session, data.get('count', 5), data.get('options'),
                                             data.get('model', True) is not False)
                elif self.path == '/api/library/sync':
                    result = harness.sync_library(data.get('scope'), data.get('tracks'), data.get('complete', False))
                elif self.path == '/api/library/unavailable':
                    result = harness.set_unavailable(data.get('scope'))
                elif self.path == '/api/features':
                    result = harness.store_features(data.get('tracks'))
                elif self.path == '/api/status':
                    result = harness.status()
                elif self.path == '/api/setlist':
                    result = harness.setlist(session, data.get('count', 10), data.get('options'))
                elif self.path == '/api/export':
                    result = harness.export(data.get('track_ids'))
                else:
                    self.send(404, {'error': 'Not found'})
                    return
                self.send(200, result)
            except (ValueError, TypeError, sqlite3.IntegrityError) as error:
                self.send(400, {'error': str(error)})
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--database', type=Path,
                        default=Path(os.environ.get('MROW_HARNESS_DB', DEFAULT_DATABASE)).expanduser())
    parser.add_argument('--no-model', action='store_true', help='Ignore any configured model')
    args = parser.parse_args()
    args.database.parent.mkdir(parents=True, exist_ok=True)
    config = None if args.no_model else load_config()
    harness = Harness(args.database, ModelClient(config) if config else None)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(harness))
    print(f'DJ harness: http://{args.host}:{args.port} database {args.database}', flush=True)
    print('Model: ' + (f'{config.provider} {config.model} at {config.base_url}' if config else 'none (heuristic only)'),
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
