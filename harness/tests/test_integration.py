"""Mixxx sync, generated features, and the optional cloud model."""
import http.client
import io
import json
import tempfile
import threading
import unittest
import urllib.error
from http.server import ThreadingHTTPServer
from pathlib import Path

import test_harness  # noqa: F401  (puts harness/src on sys.path)
from harness import Harness, make_handler
from model import ModelClient, ModelConfig, ModelError, load_config, parse_picks


def drive_track(i, bpm, **extra):
    return {'id': f'uuid-1:Music/{i}.mp3', 'title': f'Song {i}', 'artist': f'Artist {i}',
            'bpm': bpm, 'path': f'/media/dj/STICK/Music/{i}.mp3', 'genre': 'house',
            'camelot': '8A', 'duration': 240, **extra}


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class FakeEndpoint:
    """Stands in for urllib.request.urlopen; records requests, replies with `reply`."""
    def __init__(self, reply=None, error=None):
        self.reply, self.error, self.requests = reply, error, []

    def __call__(self, request, timeout):
        self.requests.append((request, json.loads(request.data), timeout))
        if self.error:
            raise self.error
        return FakeResponse(json.dumps(self.reply).encode())


def openai_reply(picks):
    return {'choices': [{'message': {'content': json.dumps({'picks': picks})}}]}


def anthropic_reply(picks, stop_reason='end_turn'):
    return {'stop_reason': stop_reason,
            'content': [{'type': 'thinking', 'thinking': ''},
                        {'type': 'text', 'text': 'Here you go:\n' + json.dumps({'picks': picks})}]}


def config(provider='openai', **changes):
    base = {'openai': 'https://example.test/v1', 'anthropic': 'https://example.test'}[provider]
    return ModelConfig(provider, 'test-model', 'secret-key', base, **changes)


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.h = Harness(Path(self.temp.name) / 'db.sqlite3')

    def tearDown(self):
        self.temp.cleanup()

    def ids(self, session='live'):
        return {t['id'] for t in self.h.recommend(session, 50)}

    def test_sync_is_lenient_about_unanalyzed_tracks(self):
        result = self.h.sync_library('uuid-1', [drive_track(1, 120), drive_track(2, None), 'junk'])
        self.assertEqual((result['synced'], result['skipped_count']), (1, 2))
        self.assertEqual(result['skipped'][0]['id'], 'uuid-1:Music/2.mp3')
        self.assertIn('BPM', result['skipped'][0]['error'])

    def test_complete_sync_and_eject_keep_history_but_stop_suggesting(self):
        self.h.sync_library('uuid-1', [drive_track(1, 120), drive_track(2, 122), drive_track(3, 124)])
        self.h.sync_library('uuid-2', [{**drive_track(9, 121), 'id': 'uuid-2:x.mp3'}])
        play = self.h.record_play('uuid-1:Music/1.mp3', 'live')['play_id']
        self.h.rate('uuid-1:Music/1.mp3', 'live', 'good', play)

        result = self.h.sync_library('uuid-1', [drive_track(1, 120), drive_track(2, 122)], complete=True)
        self.assertEqual(result['unavailable'], 1)
        self.assertEqual(self.ids(), {'uuid-1:Music/2.mp3', 'uuid-2:x.mp3'})

        self.assertEqual(self.h.set_unavailable('uuid-1')['unavailable'], 2)
        self.assertEqual(self.ids(), {'uuid-2:x.mp3'})
        state = self.h.state('live')
        self.assertEqual(len(state['plays']), 1)
        self.assertEqual(state['feedback'][0]['rating'], 'good')

        self.h.sync_library('uuid-1', [drive_track(1, 120)])
        self.assertIn('uuid-1:Music/1.mp3', {t['id'] for t in self.h.recommend('other', 50)})

    def test_generated_features_survive_sync_and_change_scores(self):
        self.h.sync_library('uuid-1', [drive_track(1, 120), drive_track(2, 120), drive_track(3, 120)])
        self.h.store_features([
            {'id': 'uuid-1:Music/1.mp3', 'source': 'meta-model-v1', 'energy': 0.5},
            {'id': 'uuid-1:Music/2.mp3', 'source': 'meta-model-v1', 'energy': 0.55},
            {'id': 'uuid-1:Music/3.mp3', 'source': 'meta-model-v1', 'energy': 1.0}])
        self.h.sync_library('uuid-1', [drive_track(1, 120), drive_track(2, 120), drive_track(3, 120)])
        track = next(t for t in self.h.state('live')['tracks'] if t['id'] == 'uuid-1:Music/2.mp3')
        self.assertEqual((track['energy'], track['feature_source']), (0.55, 'meta-model-v1'))
        self.h.record_play('uuid-1:Music/1.mp3', 'live')
        picks = self.h.recommend('live')
        self.assertEqual(picks[0]['id'], 'uuid-1:Music/2.mp3')
        self.assertIsNotNone(picks[0]['components']['energy'])

    def test_generated_features_are_validated(self):
        self.h.sync_library('uuid-1', [drive_track(1, 120)])
        for entry in ({'id': 'uuid-1:Music/1.mp3', 'source': 'm', 'energy': 2},
                      {'id': 'uuid-1:Music/1.mp3', 'source': 'm', 'bpm': 90},
                      {'id': 'uuid-1:Music/1.mp3', 'energy': 0.5},
                      {'id': 'missing', 'source': 'm', 'energy': 0.5},
                      {'id': 'uuid-1:Music/1.mp3', 'source': 'm',
                       'sections': [{'kind': 'intro', 'start': 0, 'end': 999}]}):
            with self.assertRaises(ValueError):
                self.h.store_features([entry])


class ModelClientTests(unittest.TestCase):
    candidates = [dict(id=f'uuid-1:Music/{i}.mp3', title=f'Song {i}', artist='A', bpm=120.0,
                       camelot='8A', genres=['house'], score=0.5 - i / 100) for i in range(4)]

    def test_openai_request_hides_paths_and_maps_aliases(self):
        endpoint = FakeEndpoint(openai_reply([{'id': 'c3', 'reason': 'Lifts the energy'},
                                              {'id': 'c99', 'reason': 'invented'},
                                              {'id': 'c3', 'reason': 'duplicate'},
                                              {'id': 'c1'}]))
        picks = ModelClient(config(), endpoint).rerank([], self.candidates, 3)
        self.assertEqual(picks, [('uuid-1:Music/2.mp3', 'Lifts the energy'), ('uuid-1:Music/0.mp3', '')])
        request, body, timeout = endpoint.requests[0]
        self.assertEqual(request.full_url, 'https://example.test/v1/chat/completions')
        self.assertEqual(request.get_header('Authorization'), 'Bearer secret-key')
        self.assertNotIn('Music/', json.dumps(body))
        self.assertEqual(timeout, 10.0)

    def test_anthropic_request_shape_and_text_blocks(self):
        endpoint = FakeEndpoint(anthropic_reply([{'id': 'c2', 'reason': 'Same key'}]))
        picks = ModelClient(config('anthropic', effort='low'), endpoint).rerank([], self.candidates, 2)
        self.assertEqual(picks, [('uuid-1:Music/1.mp3', 'Same key')])
        request, body, _ = endpoint.requests[0]
        self.assertEqual(request.full_url, 'https://example.test/v1/messages')
        self.assertEqual(request.get_header('X-api-key'), 'secret-key')
        self.assertEqual(request.get_header('Anthropic-version'), '2023-06-01')
        self.assertEqual(body['output_config'], {'effort': 'low'})
        self.assertIn('max_tokens', body)

    def test_failures_raise_model_error(self):
        for endpoint in (FakeEndpoint(anthropic_reply([], 'refusal')),
                         FakeEndpoint(error=urllib.error.URLError('no route to host')),
                         FakeEndpoint(error=TimeoutError()),
                         FakeEndpoint(error=urllib.error.HTTPError('u', 429, 'slow down', {}, None)),
                         FakeEndpoint({'unexpected': True}),
                         FakeEndpoint(openai_reply([{'id': 'nope'}]))):
            provider = 'anthropic' if isinstance(endpoint.reply, dict) and 'stop_reason' in endpoint.reply else 'openai'
            with self.assertRaises(ModelError):
                ModelClient(config(provider), endpoint).rerank([], self.candidates, 2)

    def test_parse_picks_tolerates_surrounding_text(self):
        aliases = {'c1': 'a', 'c2': 'b'}
        self.assertEqual(parse_picks('```json\n{"picks":[{"id":"c2","reason":"r"}]}\n```', aliases, 5), [('b', 'r')])
        with self.assertRaises(ModelError):
            parse_picks('no json here', aliases, 5)

    def test_cache_avoids_repeat_calls(self):
        endpoint = FakeEndpoint(openai_reply([{'id': 'c1'}]))
        client = ModelClient(config(), endpoint)
        for _ in range(3):
            client.rerank([], self.candidates, 1, cache_key=('s', ()))
        self.assertEqual(len(endpoint.requests), 1)


class ConfigTests(unittest.TestCase):
    def test_unconfigured_is_none(self):
        self.assertIsNone(load_config({}, env_file=None))

    def test_env_file_and_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'harness.env'
            path.write_text('# comment\nMROW_MODEL_PROVIDER=anthropic\nMROW_MODEL="claude-opus-5"\n'
                            'MROW_MODEL_API_KEY=from-file\n')
            result = load_config({'MROW_MODEL_API_KEY': 'from-env'}, env_file=path)
        self.assertEqual((result.provider, result.model, result.api_key), ('anthropic', 'claude-opus-5', 'from-env'))
        self.assertEqual((result.base_url, result.effort), ('https://api.anthropic.com', 'low'))

    def test_partial_configuration_fails_loudly(self):
        for environ in ({'MROW_MODEL_PROVIDER': 'gemini', 'MROW_MODEL': 'm', 'MROW_MODEL_API_KEY': 'k'},
                        {'MROW_MODEL_PROVIDER': 'openai', 'MROW_MODEL': 'm'},
                        {'MROW_MODEL_PROVIDER': 'openai', 'MROW_MODEL': 'm', 'MROW_MODEL_API_KEY': 'k',
                         'MROW_MODEL_TIMEOUT': 'soon'}):
            with self.assertRaises(ValueError):
                load_config(environ, env_file=None)


class SuggestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'db.sqlite3'
        Harness(self.path).sync_library('uuid-1', [drive_track(i, 120 + i) for i in range(6)])

    def tearDown(self):
        self.temp.cleanup()

    def test_without_model_is_heuristic(self):
        result = Harness(self.path).suggest('live', 3)
        self.assertEqual((result['source'], result['model_error'], len(result['tracks'])), ('heuristic', None, 3))

    def test_model_reorders_and_fills_from_heuristic(self):
        endpoint = FakeEndpoint(openai_reply([{'id': 'c5', 'reason': 'Crowd liked this artist'}]))
        h = Harness(self.path, ModelClient(config(candidates=6), endpoint))
        play = h.record_play('uuid-1:Music/0.mp3', 'live')['play_id']
        h.rate('uuid-1:Music/0.mp3', 'live', 'bad', play)
        heuristic = [t['id'] for t in h.recommend('live', 6)]
        result = h.suggest('live', 3)
        self.assertEqual(result['source'], 'model')
        self.assertEqual(result['tracks'][0]['id'], heuristic[4])
        self.assertEqual(result['tracks'][0]['model_reason'], 'Crowd liked this artist')
        self.assertEqual([t['id'] for t in result['tracks'][1:]], [i for i in heuristic if i != heuristic[4]][:2])
        sent = endpoint.requests[0][1]['messages'][1]['content']
        self.assertIn('"crowd": "bad"', sent)
        self.assertEqual(len(json.loads(sent)['candidates']), 5)

    def test_model_failure_falls_back_to_heuristic(self):
        h = Harness(self.path, ModelClient(config(), FakeEndpoint(error=urllib.error.URLError('offline'))))
        result = h.suggest('live', 3)
        self.assertEqual(result['source'], 'heuristic')
        self.assertIn('offline', result['model_error'])
        self.assertEqual([t['id'] for t in result['tracks']], [t['id'] for t in h.recommend('live', 3)])

    def test_http_routes(self):
        h = Harness(self.path)
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(h))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=5)

        def post(route, payload):
            connection.request('POST', route, json.dumps(payload), {'Content-Type': 'application/json'})
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        try:
            self.assertEqual(post('/api/library/sync', {'scope': 'uuid-2', 'complete': True,
                                                        'tracks': [{**drive_track(1, 128), 'id': 'uuid-2:a'}]}),
                             (200, {'synced': 1, 'skipped_count': 0, 'skipped': [], 'unavailable': 0}))
            status, body = post('/api/recommend', {'session': 'live', 'count': 2})
            self.assertEqual((status, body['source'], len(body['tracks'])), (200, 'heuristic', 2))
            self.assertEqual(post('/api/features', {'tracks': [{'id': 'uuid-2:a', 'source': 'm', 'energy': 0.4}]}),
                             (200, {'stored': 1}))
            self.assertEqual(post('/api/library/unavailable', {'scope': 'uuid-2'}), (200, {'unavailable': 1}))
            self.assertEqual(post('/api/status', {}), (200, {'ok': True, 'tracks_available': 6, 'model': None}))
            self.assertEqual(post('/api/library/sync', {'scope': '', 'tracks': []})[0], 400)
            # A play from Mixxx carries its track; no BPM on file uses the deck's.
            track = {'id': 'uuid-3:new.mp3', 'title': 'New', 'path': '/media/x/new.mp3'}
            status, body = post('/api/play', {'session': 'live', 'track_id': 'uuid-3:new.mp3', 'scope': 'uuid-3',
                                              'bpm': 126.5, 'event_id': 'e1', 'track': track})
            self.assertEqual(status, 200)
            self.assertEqual(post('/api/play', {'session': 'live', 'track_id': 'uuid-3:new.mp3', 'scope': 'uuid-3',
                                                'bpm': 126.5, 'event_id': 'e1', 'track': track})[1],
                             {'play_id': body['play_id'], 'duplicate': True})
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
