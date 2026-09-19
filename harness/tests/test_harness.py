import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from harness import Harness, make_handler


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'test.sqlite3'
        self.h = Harness(self.path)
        self.tracks = [dict(id=str(i), title=f'Track {i}', artist='Test', bpm=bpm,
                            path=f'/music/{i}.mp3', genre='house')
                       for i, bpm in enumerate([120, 121, 122, 160])]
        self.h.import_library(self.tracks)

    def tearDown(self):
        self.temp.cleanup()

    def test_bpm_and_performance_tempo(self):
        self.h.record_play('0', 'live')
        self.assertEqual(self.h.recommend('live')[0]['id'], '1')
        self.h.record_play('0', 'fast', bpm=160)
        self.assertEqual(self.h.recommend('fast')[0]['id'], '3')

    def test_learned_transitions(self):
        for i in range(6):
            self.h.record_play('0', f'past-{i}')
            self.h.record_play('2', f'past-{i}')
        self.h.record_play('0', 'live')
        picks = self.h.recommend('live')
        self.assertEqual(picks[0]['id'], '2')
        self.assertIn('6 time(s)', ' '.join(picks[0]['reasons']))

    def test_skip_is_not_play_and_exclusions_are_session_specific(self):
        self.h.rate('1', 'live', 'skip')
        self.assertEqual(self.h.state('live')['plays'], [])
        self.assertNotIn('1', [t['id'] for t in self.h.recommend('live')])
        self.assertIn('1', [t['id'] for t in self.h.recommend('other')])

    def test_setlist_unique_and_no_history_mutation(self):
        self.h.record_play('0', 'live')
        self.h.rate('1', 'live', 'skip')
        result = self.h.setlist('live', 10, {'max_bpm_delta': 100})
        self.assertEqual(result['returned'], 2)
        self.assertEqual({t['id'] for t in result['tracks']}, {'2', '3'})
        self.assertEqual(len(self.h.state('live')['plays']), 1)

    def test_feedback_changes_score_and_persists(self):
        play = self.h.record_play('1', 'past')['play_id']
        before = {t['id']: t['score'] for t in self.h.recommend('new')}
        self.h.rate('1', 'past', 'bad', play)
        after = {t['id']: t['score'] for t in Harness(self.path).recommend('new')}
        self.assertLess(after['1'], before['1'])
        for track, session, play_id in [('2', 'past', play), ('1', 'other', play), ('1', 'past', None)]:
            with self.assertRaises(ValueError):
                self.h.rate(track, session, 'good', play_id)

    def test_atomic_import_and_upsert(self):
        for bpm in (float('nan'), float('inf'), 0, True, None):
            with self.assertRaises(ValueError):
                self.h.import_library([{**self.tracks[0], 'title': 'Changed'},
                                       {**self.tracks[1], 'bpm': bpm}])
            self.assertEqual(self.h.state('live')['tracks'][0]['title'], 'Track 0')
        self.h.import_library([{**self.tracks[0], 'title': 'Updated'}])
        self.assertEqual(len(self.h.state('live')['tracks']), 4)

    def test_musicsearch_export_compatibility(self):
        self.h.import_library({'name': 'Export', 'tracks': [
            {'id': 'local', 'title': 'Local', 'artist': 'Artist', 'bpm': 124,
             'identifiers': {'local': '/music/local.flac'}, 'genres': ['house', 'dance'],
             'duration': 300, 'energy': 0.7}]})
        track = next(t for t in self.h.state('live')['tracks'] if t['id'] == 'local')
        self.assertEqual(track['path'], '/music/local.flac')
        self.assertEqual(track['genre'], 'house')
        with self.assertRaises(ValueError):
            self.h.import_library([{**self.tracks[0], 'path': '/bad\npath'}])

    def test_half_double_time(self):
        self.h.import_library([dict(id='half', title='Half', bpm=80, path='/half.mp3')])
        self.h.record_play('3', 'live')
        self.assertEqual(self.h.recommend('live')[0]['id'], 'half')

    def test_http_flow(self):
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(self.h))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=5)
        try:
            connection.request('GET', '/')
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertIn(b'Now playing', response.read())
            for route, payload, expected in [
                ('play', {'track_id': '0', 'session': 'live'}, 200),
                ('recommend', {'session': 'live'}, 200),
                ('setlist', {'session': 'live', 'count': 3}, 200),
                ('play', {'track_id': 'missing'}, 400),
                ('setlist', {'count': -1}, 400),
                ('state', [], 400),
            ]:
                connection.request('POST', '/api/'+route, json.dumps(payload),
                                   {'Content-Type': 'application/json'})
                response = connection.getresponse()
                self.assertEqual(response.status, expected)
                json.loads(response.read())
            connection.request('POST', '/api/state', '{}',
                               {'Content-Type': 'application/json', 'Origin': 'https://example.com'})
            response = connection.getresponse()
            self.assertEqual(response.status, 403)
            response.read()
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
