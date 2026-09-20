"""End-to-end agent policy, runtime configuration, and live-context regressions."""
import http.client
import json
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

import test_harness  # noqa: F401
from harness import Harness, make_handler
from model import ModelClient, ModelConfig
from test_integration import FakeEndpoint, FakeResponse, openai_reply, drive_track


class AgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'agent.sqlite3'
        self.h = Harness(self.path)
        self.h.sync_library('usb', [drive_track(i, 120 + i, energy=i / 10) for i in range(9)], True)

    def tearDown(self):
        self.temp.cleanup()

    def model(self, picks=None, content=None):
        endpoint = FakeEndpoint(openai_reply(picks or [{'id': 'c2', 'reason': 'Fits the room'}]))
        if content is not None:
            endpoint.reply = {'choices': [{'message': {'content': content}}]}
        client = ModelClient(ModelConfig('openrouter', 'test/fast', 'private-key',
                             'https://openrouter.ai/api/v1'), endpoint)
        self.h.agent.clients = {'next': client, 'plan': client}
        return endpoint

    def test_runtime_key_never_saved_or_returned_and_roles_distinct(self):
        status = self.h.agent.configure({'api_key': 'runtime-secret',
                    'next_model': 'test/fast', 'plan_model': 'test/planner'})
        self.assertTrue(status['connected'])
        self.assertEqual(self.h.agent.client('plan')[0].config.model, 'test/planner')
        self.assertNotIn('runtime-secret', json.dumps(status))
        self.assertNotIn(b'runtime-secret', self.path.read_bytes())
        self.h.agent.configure({'next_model': 'test/new', 'plan_model': 'test/planner'})
        self.assertEqual(self.h.agent.client('next')[0].config.api_key, 'runtime-secret')
        self.assertFalse(Harness(self.path).agent.settings()['connected'])
        self.h.agent.configure({'disconnect': True})
        self.assertIsNone(self.h.agent.client('next')[0])

    def test_invalid_configuration_is_atomic(self):
        for data in ({'api_key': 'x'}, {'api_key': 'x\nBad', 'next_model': 'a', 'plan_model': 'b'},
                     {'api_key': [], 'next_model': 'a', 'plan_model': 'b'}):
            with self.assertRaises(ValueError):
                self.h.agent.configure(data)
        self.assertFalse(self.h.agent.settings()['connected'])

    def test_distinct_models_used_for_next_and_planning(self):
        endpoint = self.model()
        planner_endpoint = FakeEndpoint(openai_reply([{'id': 'c1', 'reason': 'Start smoothly'}]))
        self.h.agent.clients['plan'] = ModelClient(ModelConfig('openrouter', 'test/planner', 'key',
                        'https://openrouter.ai/api/v1'), planner_endpoint)
        self.h.agent.run('live', {'count': 3})
        self.h.agent.run('live', {'action': 'generate', 'count': 3})
        self.assertEqual(endpoint.requests[0][1]['model'], 'test/fast')
        self.assertEqual(planner_endpoint.requests[0][1]['model'], 'test/planner')

    def test_catalog_fetch_is_public_and_filters_nontext_models(self):
        body = {'data': [{'id': 'test/text', 'name': 'Text', 'pricing': {'prompt': '0.001'},
                          'architecture': {'output_modalities': ['text']}},
                         {'id': 'test/image', 'architecture': {'output_modalities': ['image']}}]}
        with patch('agent.urllib.request.urlopen', return_value=FakeResponse(json.dumps(body).encode())) as fetch:
            result = self.h.agent.models()
        self.assertEqual([m['id'] for m in result['models']], ['test/text'])
        self.assertEqual(fetch.call_args.args, ('https://openrouter.ai/api/v1/models',))

    def test_play_exclusions_are_scoped_to_session(self):
        first = self.h.agent.run('one', {'action': 'generate', 'count': 3})
        self.h.record_play(first['tracks'][0]['id'], 'one')
        second = self.h.agent.run('two', {'action': 'generate', 'count': 3})
        self.assertNotIn(first['tracks'][0]['id'], [t['id'] for t in self.h.agent.run('one', {})['tracks']])
        self.assertIn(first['tracks'][0]['id'], [t['id'] for t in self.h.recommend('two', 100)])
        self.assertIsNotNone(second['plan'])

    def test_model_request_uses_openrouter_and_keeps_paths_private(self):
        endpoint = self.model()
        play = self.h.record_play(drive_track(0, 120)['id'], 'live', bpm=126)['play_id']
        self.h.rate(drive_track(0, 120)['id'], 'live', 'bad', play)
        result = self.h.agent.run('live', {'action': 'generate', 'count': 4})
        self.assertEqual(result['source'], 'model')
        request, body, _ = endpoint.requests[0]
        self.assertEqual(request.full_url, 'https://openrouter.ai/api/v1/chat/completions')
        self.assertEqual(request.get_header('Authorization'), 'Bearer private-key')
        sent = body['messages'][1]['content']
        for private in ('uuid-1', '/media/', 'Music/', 'private-key'):
            self.assertNotIn(private, sent)
        self.assertIn('"performance_bpm": 126', sent)
        self.assertIn('"crowd": "bad"', sent)

    def test_plan_persists_and_polling_does_not_repeat_model_calls(self):
        endpoint = self.model()
        first = self.h.agent.run('live', {'action': 'generate', 'count': 5})
        next_result = self.h.agent.run('live', {'action': 'next'})
        self.assertEqual(first['plan'], next_result['plan'])
        self.assertEqual(len(endpoint.requests), 1)
        self.assertEqual(Harness(self.path).agent.view('live')['plan']['tracks'], first['tracks'])
        self.assertTrue(Harness(self.path).agent.view('live')['plan']['stale'])
        self.assertEqual(self.h.state('live')['plays'], [])

    def test_feedback_replans_once_and_carries_previous_plan(self):
        endpoint = self.model()
        track_id = drive_track(0, 120)['id']
        play = self.h.record_play(track_id, 'live')['play_id']
        old = self.h.agent.run('live', {'action': 'generate', 'count': 4})
        self.h.rate(track_id, 'live', 'bad', play)
        self.assertTrue(self.h.agent.view('live')['plan']['stale'])
        new = self.h.agent.run('live', {})
        self.assertNotEqual(new['basis'], old['basis'])
        self.assertEqual(len(endpoint.requests), 2)
        sent = json.loads(endpoint.requests[-1][1]['messages'][1]['content'])
        self.assertEqual(sent['played'][-1]['crowd'], 'bad')
        self.assertTrue(sent['previous_plan'])
        self.h.agent.run('live', {})
        self.assertEqual(len(endpoint.requests), 2)

    def test_play_skip_eject_and_metadata_updates_invalidate(self):
        first = self.h.agent.run('live', {'action': 'generate', 'count': 4})
        track_id = first['tracks'][0]['id']
        self.h.record_play(track_id, 'live')
        next_result = self.h.agent.run('live', {})
        self.assertNotIn(track_id, [t['id'] for t in next_result['plan']['tracks']])
        skip_id = next_result['tracks'][0]['id']
        self.h.rate(skip_id, 'live', 'skip')
        self.assertNotIn(skip_id, [t['id'] for t in self.h.agent.run('live', {})['plan']['tracks']])
        self.h.sync_library('usb', [drive_track(i, 120 + i, title='Updated') for i in range(9)], True)
        self.assertTrue(self.h.agent.view('live')['plan']['stale'])
        self.h.set_unavailable('usb')
        self.assertEqual(self.h.agent.run('live', {})['tracks'], [])

    def test_invented_duplicate_and_malformed_model_ids_are_ignored(self):
        self.model([{'id': 'invented'}, {'id': []}, {'id': 'c1'}, {'id': 'c1'}, {'id': 'c2'}])
        result = self.h.agent.run('live', {'action': 'generate', 'count': 5})
        self.assertEqual(result['returned'], 5)
        ids = [t['id'] for t in result['tracks']]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertNotIn('invented', ids)

    def test_local_validator_rechecks_each_model_transition(self):
        # A proposed sequence jumps 8 BPM while the policy permits only 2.
        self.model([{'id': 'c9'}, {'id': 'c1'}, {'id': 'c8'}, {'id': 'c2'}])
        result = self.h.agent.run('live', {'action': 'generate', 'count': 8,
                   'options': {'max_bpm_delta': 2, 'allow_half_double': False}})
        self.assertGreaterEqual(result['returned'], 8)
        ranker = self.h.ranker('live', result['options'])
        path = []
        for track in result['tracks']:
            self.assertIn(track['id'], [t['id'] for t in ranker.rank(path, 100)])
            path.append(track)

    def test_bad_model_responses_fall_back(self):
        for response in ('not JSON', '{"picks": []}', '{"picks": null}', ['unexpected']):
            self.model(content=response)
            result = self.h.agent.run('live', {'retry': True, 'count': 3})
            self.assertEqual(result['source'], 'heuristic')
            self.assertTrue(result['model_error'])
            self.assertEqual(result['returned'], 3)

    def test_changed_live_context_discards_delayed_answer(self):
        endpoint = self.model()
        client = self.h.agent.client('plan')[0]
        original = client.complete

        def slow(*args, **kwargs):
            self.h.set_unavailable('usb')
            return original(*args, **kwargs)

        client.complete = slow
        result = self.h.agent.run('live', {'action': 'generate', 'count': 4})
        self.assertEqual(result['tracks'], [])
        self.assertEqual(result['source'], 'heuristic')
        self.assertIn('context changed', result['model_error'])
        self.assertEqual(len(endpoint.requests), 1)

    def test_stale_export_is_rejected(self):
        plan = self.h.agent.run('live', {'action': 'generate', 'count': 4})
        self.assertIn('#EXTM3U', self.h.agent.export('live', plan['basis'])['content'])
        self.h.record_play(plan['tracks'][0]['id'], 'live')
        with self.assertRaisesRegex(ValueError, 'set changed'):
            self.h.agent.export('live', plan['basis'])

    def test_reconnect_changes_cache_even_for_same_model_names(self):
        endpoint = self.model()
        first = self.h.agent.run('live', {'action': 'generate', 'count': 3})
        self.h.agent.configure({'disconnect': True})
        second = self.h.agent.run('live', {})
        self.assertNotEqual(first['basis'], second['basis'])
        self.assertEqual(second['source'], 'heuristic')
        self.assertEqual(len(endpoint.requests), 1)

    def test_parallel_refreshes_coalesce(self):
        endpoint = self.model()
        results = []
        threads = [threading.Thread(target=lambda: results.append(self.h.agent.run('live', {'count': 3})))
                   for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(len(results), 4)
        self.assertEqual(len(endpoint.requests), 1)

    def test_http_settings_plan_feedback_export_and_host_guard(self):
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(self.h))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=5)

        def post(route, body, headers=None):
            connection.request('POST', '/api/' + route, json.dumps({'session': 'live', **body}),
                               {'Content-Type': 'application/json', **(headers or {})})
            response = connection.getresponse()
            return response.status, json.loads(response.read())

        try:
            self.assertEqual(post('agent/settings', {}, {'Host': 'evil.example'})[0], 403)
            self.assertEqual(post('agent/settings', {}, {'Origin': 'https://evil.example'})[0], 403)
            self.assertEqual(post('agent/settings', {'api_key': 'x'})[0], 400)
            self.assertEqual(post('agent/settings', {'disconnect': True})[0], 200)
            status, plan = post('agent', {'action': 'generate', 'count': 3})
            self.assertEqual((status, plan['returned']), (200, 3))
            status, play = post('play', {'track_id': plan['tracks'][0]['id']})
            self.assertEqual(status, 200)
            self.assertEqual(post('feedback', {'track_id': plan['tracks'][0]['id'],
                                              'play_id': play['play_id'], 'rating': 'bad'})[0], 200)
            self.assertEqual(post('agent/export', {'basis': plan['basis']})[0], 400)
            status, updated = post('agent', {})
            self.assertEqual(status, 200)
            self.assertNotEqual(updated['basis'], plan['basis'])
            self.assertEqual(post('agent/export', {'basis': updated['basis']})[0], 200)
            self.assertIsNone(post('agent', {'action': 'clear'})[1]['plan'])
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
