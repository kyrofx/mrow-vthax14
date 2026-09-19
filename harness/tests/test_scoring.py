import unittest
import sqlite3
import json
import http.client
import threading
from http.server import ThreadingHTTPServer

import test_harness
from scoring import harmonic, policy
from harness import Harness, make_handler


class ScoringTests(unittest.TestCase):
    setUp = test_harness.HarnessTests.setUp
    tearDown = test_harness.HarnessTests.tearDown
    def rich(self, **changes):
        return {**self.tracks[1], 'duration': 240, 'camelot': '8A', 'energy': .5,
                'genres': ['house', 'dance'], 'embedding': {'model': 'test', 'values': [1, 0]},
                'sections': [{'kind': 'intro', 'start': 0, 'end': 32, 'vocalness': .1},
                             {'kind': 'outro', 'start': 208, 'end': 240, 'vocalness': .2}], **changes}

    def test_rich_features_change_ranking_and_explain_every_point(self):
        self.h.import_library([self.rich(id='0', bpm=120), self.rich(id='1', bpm=120),
                               self.rich(id='2', bpm=120, camelot='3B', energy=.95,
                                         embedding={'model': 'test', 'values': [-1, 0]})])
        self.h.record_play('0', 'live')
        results = {t['id']: t for t in self.h.recommend('live')}
        self.assertGreater(results['1']['score'], results['2']['score'])
        for key in ('harmonic', 'energy', 'timbre', 'genre', 'intro_outro', 'vocal_safety'):
            self.assertIsNotNone(results['1']['components'][key])
        for result in results.values():
            self.assertAlmostEqual(sum(result['contributions'].values()), result['score'])
            self.assertTrue(0 <= result['score'] <= 1)

    def test_missing_features_stay_unknown_and_harmonic_constraint_fails_closed(self):
        self.h.record_play('0', 'live')
        pick = self.h.recommend('live')[0]
        self.assertIn('harmonic', pick['unknown'])
        self.assertIn('timbre', pick['unknown'])
        self.assertEqual(self.h.recommend('live', options={'harmonic_only': True}), [])
        self.assertEqual(harmonic('12A', '1A'), .9)
        self.assertLess(harmonic('12A', '1B'), .9)

    def test_feedback_is_replaceable_and_skip_does_not_poison_global_preference(self):
        play = self.h.record_play('1', 'past')['play_id']
        for _ in range(5):
            self.h.rate('1', 'past', 'good', play)
        self.assertEqual(len(self.h.state('past')['feedback']), 1)
        before = {t['id']: t['score'] for t in self.h.recommend('new')}
        self.h.rate('1', 'past', 'bad', play)
        after = {t['id']: t['score'] for t in self.h.recommend('new')}
        self.assertLess(after['1'], before['1'])
        self.h.rate('2', 'skip-session', 'skip')
        self.assertEqual(after, {t['id']: t['score'] for t in self.h.recommend('new')})

    def test_feedback_on_current_song_changes_future_candidates(self):
        self.h.import_library([self.rich(id='0', bpm=120), self.rich(id='1', bpm=122), self.rich(id='2', bpm=118)])
        play = self.h.record_play('0', 'live')['play_id']
        before = {t['id']: t for t in self.h.recommend('live')}
        self.h.rate('0', 'live', 'bad', play)
        after = {t['id']: t for t in self.h.recommend('live')}
        self.assertLess(after['1']['target_bpm'], before['1']['target_bpm'])
        self.assertEqual(self.h.recommend('live')[0]['id'], '2')

    def test_retried_play_event_is_idempotent(self):
        first = self.h.record_play('0', 'live', event_id='deck-event-1')
        second = self.h.record_play('0', 'live', event_id='deck-event-1')
        self.assertEqual(first['play_id'], second['play_id'])
        self.assertEqual(len(self.h.state('live')['plays']), 1)
        with self.assertRaises(ValueError):
            self.h.record_play('1', 'live', event_id='deck-event-1')

    def test_each_planned_edge_obeys_constraints_and_preserves_effective_tempo(self):
        self.h.import_library([self.rich(id='0', bpm=160), self.rich(id='1', bpm=80),
                               self.rich(id='2', bpm=81), self.rich(id='3', bpm=82)])
        self.h.record_play('0', 'live')
        options = {'harmonic_only': True, 'max_bpm_delta': 3, 'allow_half_double': True}
        result = self.h.setlist('live', 3, options)
        self.assertEqual(result['returned'], 3)
        self.assertEqual([t['effective_bpm'] for t in result['tracks']], [160, 162, 164])
        for t in result['tracks']:
            self.assertLessEqual(abs(t['bpm_delta']), 3)
            self.assertGreaterEqual(t['components']['harmonic'], .9)
        self.assertEqual(self.h.recommend('live', options={'allow_half_double': False}), [])

    def test_beam_search_avoids_greedy_dead_end(self):
        self.h.import_library([self.rich(id='0', bpm=120), self.rich(id='1', bpm=124),
                               self.rich(id='2', bpm=116), self.rich(id='3', bpm=112)])
        self.h.record_play('0', 'live')
        result = self.h.setlist('live', 2, {'max_bpm_delta': 4, 'allow_half_double': False, 'direction': 'up'})
        self.assertEqual([t['id'] for t in result['tracks']], ['2', '3'])

    def test_impossible_plan_reports_shortfall_without_relaxing_rules(self):
        self.h.record_play('0', 'live')
        result = self.h.setlist('live', 3, {'max_bpm_delta': 0})
        self.assertFalse(result['complete'])
        self.assertEqual(result['tracks'], [])
        self.assertIn('constraints', result['notes'][-1])

    def test_direction_and_explicit_target_are_applied(self):
        self.h.record_play('0', 'live')
        for direction, target in [('up', 122), ('down', 118), ('steady', 120)]:
            self.assertEqual(self.h.recommend('live', options={'direction': direction})[0]['target_bpm'], target)
        self.assertEqual(self.h.recommend('live', options={'target_bpm': 127})[0]['target_bpm'], 127)

    def test_invalid_features_options_and_identifiers(self):
        for changes in ({'energy': float('nan')}, {'camelot': '15A'}, {'embedding': {'model': 'x', 'values': [1, float('inf')]}},
                        {'genres': 'house'}, {'sections': [{'kind': 'intro', 'start': 20, 'end': 10}]}):
            with self.assertRaises(ValueError):
                self.h.import_library([self.rich(**changes)])
        for options in ({'max_bpm_delta': -1}, {'harmonic_only': 'true'}, {'unknown': 4}, {'target_bpm': float('nan')}):
            with self.assertRaises(ValueError):
                policy(options)
        with self.assertRaises(ValueError):
            self.h.record_play(['0'], 'live')

    def test_export_order_paths_and_duration(self):
        self.h.import_library([self.rich(id='1')])
        result = self.h.export(['1', '0'])
        self.assertTrue(result['content'].startswith('#EXTM3U\n#EXTINF:240,'))
        self.assertLess(result['content'].index('/music/1.mp3'), result['content'].index('/music/0.mp3'))
        with self.assertRaises(ValueError):
            self.h.export(['0', '0'])

    def test_http_end_to_end_import_play_feedback_constraints_export_restart(self):
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(self.h))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=5)
        def post(route, **payload):
            connection.request('POST', '/api/' + route, json.dumps({'session': 'e2e', **payload}),
                               {'Content-Type': 'application/json'})
            response = connection.getresponse()
            body = json.loads(response.read())
            self.assertEqual(response.status, 200, body)
            return body
        try:
            post('library', tracks={'tracks': [self.rich(id='0', bpm=120), self.rich(id='1', bpm=121),
                                               self.rich(id='2', bpm=122), self.rich(id='3', bpm=123)]})
            event = dict(track_id='0', bpm=120, event_id='e2e-1')
            play = post('play', **event)['play_id']
            self.assertEqual(play, post('play', **event)['play_id'])
            post('feedback', track_id='0', play_id=play, rating='good')
            post('feedback', track_id='0', play_id=play, rating='bad')
            state = post('state')
            self.assertEqual(len(state['plays']), 1)
            self.assertEqual(len(state['feedback']), 1)
            self.assertEqual(state['feedback'][0]['rating'], 'bad')
            options = {'max_bpm_delta': 3, 'harmonic_only': True, 'direction': 'up'}
            picks = post('recommend', options=options)['tracks']
            skipped = picks[-1]['id']
            post('feedback', track_id=skipped, rating='skip')
            plan = post('setlist', count=2, options=options)
            self.assertTrue(plan['complete'])
            ids = [t['id'] for t in plan['tracks']]
            self.assertNotIn(skipped, ids)
            self.assertNotIn('0', ids)
            self.assertEqual(len(set(ids)), 2)
            export = post('export', track_ids=ids)
            paths = [line for line in export['content'].splitlines() if not line.startswith('#')]
            self.assertEqual(paths, [t['path'] for t in plan['tracks']])
            self.assertEqual(len(post('state')['plays']), 1)
            self.assertEqual(Harness(self.path).setlist('e2e', 2, options), plan)
        finally:
            connection.close(); server.shutdown(); server.server_close(); thread.join()

    def test_legacy_database_migration_preserves_history(self):
        path = self.path.with_name('legacy.sqlite3')
        with sqlite3.connect(path) as db:
            db.executescript('''
                CREATE TABLE tracks(id TEXT PRIMARY KEY,title TEXT,artist TEXT,bpm REAL,path TEXT,genre TEXT);
                INSERT INTO tracks VALUES('a','A','Artist',120,'/a.mp3','house');
                CREATE TABLE plays(id INTEGER PRIMARY KEY,track_id TEXT,session TEXT,bpm REAL,created TEXT);
                INSERT INTO plays VALUES(1,'a','old',120,'2026-01-01');
            ''')
        migrated = Harness(path)
        state = migrated.state('old')
        self.assertEqual(state['plays'][0]['track_id'], 'a')
        self.assertEqual(state['tracks'][0]['genres'], ['house'])
        self.assertIsNone(state['tracks'][0]['camelot'])


if __name__ == '__main__':
    unittest.main()
