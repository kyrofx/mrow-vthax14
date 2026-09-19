"""Local DJ assistant. Python 3.9+, standard library only."""

import argparse
import json
import math
import sqlite3
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from scoring import Ranker, features


class Harness:
    def __init__(self, database):
        self.database = str(database)
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
        return {**features(track), **metadata, **track}

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

    def import_library(self, tracks):
        if isinstance(tracks, dict):
            tracks = tracks.get('tracks')
        if not isinstance(tracks, list) or not tracks:
            raise ValueError('Provide a nonempty tracks array')
        rows = []
        for track in tracks:
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
            if '\n' in track['path'] or '\r' in track['path'] or track['path'].startswith('#'):
                raise ValueError('Track path cannot contain newlines or start with #')
            if track.get('bpm') is None:
                raise ValueError('Track ' + track['id'] + ' needs an analyzed or supplied BPM before import')
            for key in ('artist', 'genre'):
                if not isinstance(track.get(key, ''), str):
                    raise ValueError(key + ' must be text')
            rows.append((track['id'], track['title'], track.get('artist', ''),
                         self.bpm(track.get('bpm')), track['path'], track.get('genre', ''),
                         json.dumps(features(track), allow_nan=False)))
        if len({row[0] for row in rows}) != len(rows):
            raise ValueError('Track IDs must be unique within an import')
        with self.connect() as db:
            db.executemany('''INSERT INTO tracks(id,title,artist,bpm,path,genre,features) VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(id) DO UPDATE SET title=excluded.title, artist=excluded.artist,
                bpm=excluded.bpm, path=excluded.path, genre=excluded.genre, features=excluded.features''', rows)
        return {'imported': len(rows)}

    def state(self, session):
        session = self.session(session)
        with self.connect() as db:
            tracks = [self.decode(r) for r in db.execute('SELECT * FROM tracks ORDER BY artist,title')]
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
            tracks = [self.decode(r) for r in db.execute('SELECT * FROM tracks ORDER BY id')]
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
            tracks = {r['id']: self.decode(r) for r in db.execute('SELECT * FROM tracks')}
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
                    result = harness.record_play(data.get('track_id'), session, data.get('bpm'), data.get('event_id'))
                elif self.path == '/api/feedback':
                    result = harness.rate(data.get('track_id'), session, data.get('rating'), data.get('play_id'))
                elif self.path == '/api/recommend':
                    result = {'tracks': harness.recommend(session, data.get('count', 5), options=data.get('options'))}
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
    parser.add_argument('--database', type=Path, default=Path(__file__).resolve().parents[1] / 'data/harness.sqlite3')
    args = parser.parse_args()
    args.database.parent.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(Harness(args.database)))
    print(f'DJ harness: http://{args.host}:{args.port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
