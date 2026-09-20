"""Music API contract, feedback provenance, download failures and job lifecycle."""
import io
import json
from email.message import Message
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace
import urllib.error

import test_harness  # noqa: F401
from harness import Harness, dispatch


class Audio(io.BytesIO):
    def __init__(self, content=b'ID3test-mp3', mime='audio/mpeg'):
        super().__init__(content)
        self.headers = Message()
        self.headers['Content-Type'] = mime


class MusicTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.h = Harness(Path(self.temp.name) / 'harness.sqlite3')
        self.h.import_library([
            dict(id='liked', title='Private song title', artist='Private artist', bpm=126,
                 genre='house', path='/private/music.mp3'),
            dict(id='skipped', title='Unplayed', bpm=90, genre='unplayed genre', path='/private/other.mp3')])
        self.h.music.settings({'api_key': 'secret-eleven-key'})

    def tearDown(self):
        self.temp.cleanup()

    def feedback(self):
        play = self.h.record_play('liked', 'past-set', bpm=128)
        self.h.rate('liked', 'past-set', 'bad', play['play_id'])
        self.h.rate('liked', 'past-set', 'good', play['play_id'])
        self.h.rate('skipped', 'past-set', 'skip')

    def queued(self, **data):
        with patch('music.threading.Thread') as thread:
            result = dispatch(self.h, '/api/agent/music/generate', {'session': 'new-set', **data})
        args = thread.call_args.kwargs['args']
        return result, args

    def test_requires_actual_rated_play_and_valid_options(self):
        with self.assertRaisesRegex(ValueError, 'Rate at least'):
            self.h.music.start('s', {})
        self.feedback()
        for data in ({'duration_seconds': True}, {'duration_seconds': 601},
                     {'duration_seconds': 2}, {'duration_seconds': 2.5},
                     {'instrumental': 'yes'}, {'direction': 'anything'}):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.h.music.start('s', data)
        self.assertEqual(self.h.music.view()['jobs'], [])

    def test_prompt_uses_updated_historical_feedback_without_identifiers(self):
        self.feedback()
        self.h.set_unavailable('usb')
        prompt = self.h.music.prompt('build')
        self.assertIn('"good": 1', prompt)
        self.assertIn('"bad": 0', prompt)
        self.assertIn('128.0', prompt)
        self.assertIn('house', prompt)
        for private in ('Private song title', 'Private artist', '/private/', 'unplayed genre', 'past-set'):
            self.assertNotIn(private, prompt)

    def test_download_request_and_persistent_result(self):
        self.feedback()
        job, args = self.queued(duration_seconds=30, instrumental=False)
        with patch('music.urllib.request.urlopen', return_value=Audio()) as endpoint:
            self.h.music.generate(*args)
        request = endpoint.call_args.args[0]
        self.assertEqual(request.full_url, 'https://api.elevenlabs.io/v1/music?output_format=mp3_44100_128')
        self.assertEqual(request.get_header('Xi-api-key'), 'secret-eleven-key')
        self.assertEqual(json.loads(request.data)['music_length_ms'], 30000)
        self.assertFalse(json.loads(request.data)['force_instrumental'])
        saved = self.h.music.view()['jobs'][0]
        self.assertEqual(saved['state'], 'complete')
        self.assertEqual(Path(saved['path']).read_bytes(), b'ID3test-mp3')
        self.assertEqual(Path(saved['path']).stat().st_mode & 0o777, 0o600)
        self.assertEqual(Path(saved['path']).name, job['id'] + '.mp3')
        self.assertFalse(list(self.h.music.directory.glob('*.part')))
        fresh = Harness(self.h.database)
        self.assertEqual(fresh.music.view()['jobs'][0]['path'], saved['path'])
        self.assertFalse(fresh.music.settings({})['connected'])
        self.assertNotIn(b'secret-eleven-key', Path(self.h.database).read_bytes())
        self.assertEqual(len(self.h.state('new-set')['tracks']), 2)  # No invented analyzed BPM.

    def test_concurrent_generation_rejected_and_polling_never_spends(self):
        self.feedback()
        self.queued()
        with patch('music.urllib.request.urlopen') as endpoint:
            for _ in range(3):
                self.h.music.view()
            with self.assertRaisesRegex(ValueError, 'already generating'):
                self.h.music.start('other-session', {})
            endpoint.assert_not_called()

    def test_empty_non_audio_and_error_responses_never_publish_file(self):
        self.feedback()
        for audio in (Audio(b''), Audio(b'{"error": "secret"}', 'application/json'), Audio(b'not mp3')):
            with self.subTest(audio=audio):
                _, args = self.queued()
                with patch('music.urllib.request.urlopen', return_value=audio):
                    self.h.music.generate(*args)
                self.assertEqual(self.h.music.view()['jobs'][0]['state'], 'failed')
                self.assertEqual(list(self.h.music.directory.iterdir()), [])

    def test_http_error_is_redacted_and_not_retried(self):
        self.feedback()
        _, args = self.queued()
        error = urllib.error.HTTPError('url', 401, 'secret-eleven-key', {}, None)
        with patch('music.urllib.request.urlopen', side_effect=error) as endpoint:
            self.h.music.generate(*args)
            endpoint.assert_called_once()
        result = self.h.music.view()
        self.assertIn('401', result['jobs'][0]['error'])
        self.assertNotIn('secret-eleven-key', json.dumps(result))

    def test_partial_download_failure_cleans_up(self):
        self.feedback()
        _, args = self.queued()
        response = Audio()
        response.read = unittest.mock.Mock(side_effect=[b'ID3partial', OSError('secret-eleven-key')])
        with patch('music.urllib.request.urlopen', return_value=response):
            self.h.music.generate(*args)
        result = self.h.music.view()['jobs'][0]
        self.assertEqual(result['state'], 'failed')
        self.assertIsNone(result['path'])
        self.assertNotIn('secret-eleven-key', result['error'])
        self.assertEqual(list(self.h.music.directory.iterdir()), [])

    def test_background_generation_does_not_block_feedback_or_status(self):
        self.feedback()
        started, release, done = threading.Event(), threading.Event(), threading.Event()
        original = self.h.music.generate
        def generate(*args):
            try:
                original(*args)
            finally:
                done.set()
        def endpoint(*args, **kwargs):
            started.set()
            release.wait(5)
            return Audio()
        with patch.object(self.h.music, 'generate', side_effect=generate), patch('music.urllib.request.urlopen', side_effect=endpoint):
            try:
                self.h.music.start('s', {})
                self.assertTrue(started.wait(2))
                self.assertEqual(self.h.music.view()['jobs'][0]['state'], 'generating')
                self.h.record_play('liked', 's')
                self.h.music.settings({'disconnect': True})
                self.assertFalse(self.h.music.settings({})['connected'])
            finally:
                release.set()
                self.assertTrue(done.wait(3))
        self.assertEqual(self.h.music.view()['jobs'][0]['state'], 'complete')

    def test_restart_marks_unfinished_job_failed_without_retry(self):
        self.feedback()
        self.queued()
        with patch('music.urllib.request.urlopen') as endpoint:
            fresh = Harness(self.h.database)
            self.assertEqual(fresh.music.view()['jobs'][0]['state'], 'failed')
            self.assertIn('interrupted', fresh.music.view()['jobs'][0]['error'])
            endpoint.assert_not_called()

    def test_settings_do_not_disclose_key_and_invalid_update_is_atomic(self):
        for key in ('bad\nkey', ['key'], 'x' * 4097):
            with self.assertRaises(ValueError):
                self.h.music.settings({'api_key': key})
        self.assertNotIn('secret-eleven-key', json.dumps(self.h.music.settings({})))
        self.assertTrue(self.h.music.settings({'api_key': ''})['connected'])
        self.assertFalse(self.h.music.settings({'disconnect': True})['connected'])
        with self.assertRaisesRegex(ValueError, 'Enter an ElevenLabs'):
            self.h.music.settings({'api_key': ''})

    def test_inspiration_uses_current_session_traits_without_names(self):
        self.h.record_play('liked', 'current', bpm=130)
        prompt = self.h.music.prompt('hold', 'current', True)
        self.assertIn('130', prompt)
        self.assertIn('house', prompt)
        self.assertNotIn('Private song title', prompt)
        self.assertNotIn('Private artist', prompt)
        with self.assertRaisesRegex(ValueError, 'Play a song'):
            self.h.music.prompt('hold', 'different', True)
        with self.assertRaises(ValueError):
            self.h.music.start('current', {'inspire_current': 'yes'})

    def test_completed_queue_persists_and_consumes_idempotently(self):
        self.feedback()
        job, args = self.queued()
        with patch('music.urllib.request.urlopen', return_value=Audio()):
            self.h.music.generate(*args)
        upcoming = self.h.music.view()['upcoming']
        self.assertEqual(len(upcoming), 1)
        self.assertTrue(upcoming[0]['title'].startswith('Crowd Mix '))
        restarted = Harness(self.h.database)
        self.assertEqual(len(restarted.music.view()['upcoming']), 1)
        for _ in range(2):
            dispatch(restarted, '/api/agent/music/consume', {'path': upcoming[0]['path']})
        self.assertEqual(restarted.music.view()['upcoming'], [])
        self.assertEqual(restarted.music.view()['jobs'][0]['state'], 'complete')

    def brief_client(self, response=None):
        client = Mock()
        client.config = SimpleNamespace(provider='gemini')
        client.complete.return_value = json.dumps(response or {
            'summary': 'A spacious house instrumental with a gradual build.',
            'target_bpm': 128, 'style': 'House, proposed from liked genre feedback',
            'arrangement': 'Intro, gradual build, central section, outro',
            'instrumentation': 'Creative choices: drum machine and warm synth chords',
            'intro_outro': 'Sparse percussion intro and outro for transitions',
            'reasoning': 'House was rated good; instrumentation is a creative choice, not heard evidence.'})
        self.h.agent.clients = {'plan': client}
        return client

    def test_gemini_brief_is_sent_once_and_persisted_before_download(self):
        self.feedback()
        self.h.record_play('liked', 'new-set', bpm=131)
        client = self.brief_client()
        job, args = self.queued(inspire_current=True, duration_seconds=45)
        self.assertEqual(self.h.music.view()['jobs'][0]['phase'], 'briefing')
        def download(request, **kwargs):
            saved = self.h.music.view()['jobs'][0]
            self.assertEqual(saved['brief_source'], 'gemini')
            self.assertEqual(saved['phase'], 'composing')
            self.assertEqual(json.loads(request.data)['prompt'], saved['prompt'])
            self.assertIn('Target tempo: 128 BPM', saved['prompt'])
            self.assertIn('Creative choices', saved['prompt'])
            self.assertEqual(json.loads(request.data)['music_length_ms'], 45000)
            return Audio()
        with patch('music.urllib.request.urlopen', side_effect=download) as endpoint:
            self.h.music.generate(*args)
            endpoint.assert_called_once()
        client.complete.assert_called_once()
        context = json.loads(client.complete.call_args.args[0])
        self.assertEqual(context['current_song_inspiration']['bpm'], 131)
        self.assertEqual(context['duration_seconds'], 45)
        self.assertTrue(context['instrumental'])
        for secret in ('/private/', 'secret-eleven-key', 'Private artist', 'Private song title'):
            self.assertNotIn(secret, json.dumps(context))
        self.assertIn('NOT audio', client.complete.call_args.args[1])
        persisted = Harness(self.h.database).music.view()['jobs'][0]
        self.assertEqual(persisted['brief_source'], 'gemini')
        self.assertIn('creative choice', persisted['reasoning'])
        for _ in range(3):
            self.h.music.view()
        client.complete.assert_called_once()

    def test_bad_gemini_briefs_fall_back_without_retry_or_secret_leak(self):
        self.feedback()
        for response in ('not json', '{}', '[]', json.dumps({'target_bpm': True})):
            client = self.brief_client()
            client.complete.return_value = response
            _, args = self.queued()
            original = self.h.music.view()['jobs'][0]['prompt']
            with patch('music.urllib.request.urlopen', return_value=Audio()) as endpoint:
                self.h.music.generate(*args)
                self.assertEqual(json.loads(endpoint.call_args.args[0].data)['prompt'], original)
                endpoint.assert_called_once()
            saved = self.h.music.view()['jobs'][0]
            self.assertEqual(saved['state'], 'complete')
            self.assertEqual(saved['brief_source'], 'direct')
            self.assertIn('using the direct', saved['brief_error'])
            client.complete.assert_called_once()
        client = self.brief_client()
        client.complete.side_effect = TimeoutError('secret-gemini-key')
        _, args = self.queued()
        with patch('music.urllib.request.urlopen', return_value=Audio()):
            self.h.music.generate(*args)
        self.assertNotIn('secret-gemini-key', json.dumps(self.h.music.view()))

    def test_brief_context_keeps_sequence_ratings_and_toggle_boundary(self):
        self.feedback()
        first = self.h.record_play('liked', 'new-set', bpm=127)
        self.h.rate('liked', 'new-set', 'mid', first['play_id'])
        self.h.record_play('skipped', 'new-set', bpm=92)
        client = self.brief_client()
        _, args = self.queued(inspire_current=False)
        # Subsequent activity must not mutate the job's captured context.
        self.h.rate('liked', 'new-set', 'bad', first['play_id'])
        with patch('music.urllib.request.urlopen', return_value=Audio()):
            self.h.music.generate(*args)
        context = json.loads(client.complete.call_args.args[0])
        self.assertIsNone(context['current_song_inspiration'])
        self.assertEqual(context['current_transition']['bpm'], 92)
        self.assertEqual(context['recent_sequence'][0]['crowd'], 'mid')
        self.assertIn('"good": 1', context['longer_term_preferences'])

    def test_briefing_is_background_and_rejects_duplicate_generation(self):
        self.feedback()
        client = self.brief_client()
        response = client.complete.return_value
        started, release, done = threading.Event(), threading.Event(), threading.Event()
        def compose(*args):
            started.set()
            release.wait(5)
            return response
        client.complete.side_effect = compose
        original = self.h.music.generate
        def generate(*args):
            try:
                original(*args)
            finally:
                done.set()
        with patch.object(self.h.music, 'generate', side_effect=generate), patch('music.urllib.request.urlopen', return_value=Audio()):
            try:
                self.h.music.start('new-set', {})
                self.assertTrue(started.wait(2))
                self.assertEqual(self.h.music.view()['jobs'][0]['phase'], 'briefing')
                self.h.record_play('liked', 'new-set')
                with self.assertRaisesRegex(ValueError, 'already generating'):
                    self.h.music.start('new-set', {})
            finally:
                release.set()
                self.assertTrue(done.wait(3))
        client.complete.assert_called_once()


if __name__ == '__main__':
    unittest.main()
