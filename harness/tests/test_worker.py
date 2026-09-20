"""Private-pipe worker, embedded boot configuration and secret provisioning."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

import test_harness  # noqa: F401
from harness import Harness
from worker import provision, read_provision, serve

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('agent_config', ROOT / 'RPI/scripts/agent-config.py')
config_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config_tool)
SECRET = {'api_key': 'test-private-key', 'next_model': 'test/quick', 'plan_model': 'test/plan'}


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.config = self.root / 'credentials' / 'agent.json'
        self.h = Harness(self.root / 'history.sqlite3')

    def tearDown(self):
        self.temp.cleanup()

    def test_installed_config_private_atomic_and_valid(self):
        config_tool.install(SECRET, self.config)
        self.assertEqual(self.config.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.config.parent.stat().st_mode & 0o777, 0o700)
        self.assertEqual(read_provision(self.config), SECRET)
        with self.assertRaises(FileExistsError):
            config_tool.install(SECRET, self.config)
        config_tool.install(dict(SECRET, plan_model='test/new'), self.config, replace=True)
        self.assertEqual(read_provision(self.config)['plan_model'], 'test/new')
        self.assertEqual(list(self.config.parent.iterdir()), [self.config])

    def test_provision_loads_both_models_without_key_in_status_or_database(self):
        config_tool.install(SECRET, self.config)
        provision(self.h, self.config)
        status = self.h.agent.settings()
        self.assertTrue(status['connected'])
        self.assertTrue(status['provisioned'])
        self.assertEqual(status['plan_model'], 'test/plan')
        self.assertNotIn(SECRET['api_key'], json.dumps(status))
        self.assertNotIn(SECRET['api_key'].encode(), (self.root / 'history.sqlite3').read_bytes())
        self.h.agent.configure({'disconnect': True})
        self.assertFalse(self.h.agent.settings()['connected'])
        # Runtime disconnect does not modify the install-time credentials.
        provision(self.h, self.config)
        self.assertTrue(self.h.agent.settings()['connected'])

    def test_world_readable_provision_rejected_without_breaking_local_agent(self):
        config_tool.install(SECRET, self.config)
        self.config.chmod(0o644)
        provision(self.h, self.config)
        self.assertFalse(self.h.agent.settings()['connected'])
        self.assertTrue(self.h.agent.settings()['provision_error'])
        self.assertEqual(self.h.agent.run('test', {})['tracks'], [])
        with self.assertRaises(ValueError):
            config_tool.read(self.config)

    def test_symlink_provision_rejected(self):
        config_tool.install(SECRET, self.config)
        link = self.root / 'link.json'
        link.symlink_to(self.config)
        provision(self.h, link)
        self.assertFalse(self.h.agent.settings()['connected'])
        self.assertTrue(self.h.agent.settings()['provision_error'])
        with self.assertRaises(OSError):
            config_tool.read(link)

    def test_missing_config_is_optional(self):
        provision(self.h, self.config)
        self.assertEqual(self.h.agent.settings()['provision_error'], '')

    def test_bad_config_does_not_echo_secret(self):
        self.config.parent.mkdir()
        self.config.write_text('test-private-key NOT JSON')
        self.config.chmod(0o600)
        provision(self.h, self.config)
        self.assertNotIn(SECRET['api_key'], json.dumps(self.h.agent.settings()))
        result = subprocess.run([sys.executable, str(ROOT / 'RPI/scripts/agent-config.py'),
                                 '--validate', str(self.config)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(SECRET['api_key'], result.stdout + result.stderr)

    def test_worker_protocol_without_network_listener(self):
        incoming = io.StringIO('\n'.join([json.dumps({'id': 1, 'path': '/health'}),
                       'bad-json', json.dumps({'id': 2, 'path': '/unknown'})]) + '\n')
        outgoing = io.StringIO()
        with patch('socket.socket.bind', side_effect=AssertionError('No listener allowed')):
            serve(self.h, incoming, outgoing)
        rows = [json.loads(line) for line in outgoing.getvalue().splitlines()]
        self.assertEqual(rows[0], {'ready': True})
        self.assertTrue(rows[1]['body']['ok'])
        self.assertEqual(rows[2]['status'], 400)
        self.assertEqual(rows[3]['status'], 404)

    def test_actual_subprocess_loads_provision_and_exits_cleanly(self):
        config_tool.install(SECRET, self.config)
        commands = [{'id': 1, 'path': '/api/agent/settings'},
                    {'id': 2, 'path': '/api/agent/settings', 'body': {'disconnect': True}},
                    {'id': 3, 'path': '/api/agent', 'body': {'session': 'worker'}}]
        result = subprocess.run([sys.executable, '-E', '-s', '-u',
                    str(ROOT / 'harness/src/worker.py'), '--database', str(self.root / 'child.sqlite3'),
                    '--config', str(self.config)],
                    input=''.join(json.dumps(c) + '\n' for c in commands),
                    capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(SECRET['api_key'], result.stdout + result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(rows[0], {'ready': True})
        replies = {row['id']: row for row in rows[1:]}
        self.assertTrue(replies[1]['body']['provisioned'])
        self.assertFalse(replies[2]['body']['connected'])
        self.assertEqual(replies[3]['status'], 200)

    def test_elevenlabs_only_provision(self):
        data = {'elevenlabs_api_key': 'test-eleven-only'}
        config_tool.install(data, self.config)
        provision(self.h, self.config)
        self.assertTrue(self.h.music.settings({})['connected'])
        self.assertTrue(self.h.music.settings({})['provisioned'])
        self.assertFalse(self.h.agent.settings()['connected'])
        self.assertEqual(self.h.agent.settings()['provision_error'], '')
        self.assertNotIn(data['elevenlabs_api_key'], json.dumps(self.h.music.settings({})))

    def test_partial_openrouter_config_rejected(self):
        for data in ({}, {'api_key': 'test'},
                     {'elevenlabs_api_key': 'test', 'next_model': 'test/model'}):
            with self.subTest(fields=list(data)):
                with self.assertRaises(ValueError):
                    config_tool.validate(data)

    def test_both_provider_keys_load_and_disconnect_independently(self):
        both = dict(SECRET, elevenlabs_api_key='test-eleven-private')
        config_tool.install(both, self.config)
        provision(self.h, self.config)
        self.assertTrue(self.h.music.settings({})['provisioned'])
        self.assertEqual(self.h.music.key, both['elevenlabs_api_key'])
        self.assertEqual(self.h.agent.client('next')[0].config.api_key, SECRET['api_key'])
        self.h.music.settings({'disconnect': True})
        self.assertFalse(self.h.music.settings({})['connected'])
        self.assertTrue(self.h.agent.settings()['connected'])
        restarted = Harness(self.root / 'restart.sqlite3')
        provision(restarted, self.config)
        self.assertTrue(restarted.music.settings({})['connected'])
        visible = json.dumps([restarted.music.view(), restarted.agent.settings()])
        for key in (SECRET['api_key'], both['elevenlabs_api_key']):
            self.assertNotIn(key, visible)
            self.assertNotIn(key.encode(), (self.root / 'restart.sqlite3').read_bytes())

    def test_invalid_optional_key_rejects_entire_config_before_loading(self):
        self.config.parent.mkdir()
        for value in ('', '   ', 'bad\nkey', 'é', 'x' * 4097, None, 42, []):
            data = dict(SECRET, elevenlabs_api_key=value)
            with self.subTest(value_type=type(value).__name__):
                with self.assertRaises(ValueError):
                    config_tool.validate(data)
                self.config.write_text(json.dumps(data))
                self.config.chmod(0o600)
                with self.assertRaises(ValueError):
                    read_provision(self.config)
                provision(self.h, self.config)
                self.assertFalse(self.h.agent.settings()['connected'])
                self.assertFalse(self.h.music.settings({})['connected'])
                self.assertTrue(self.h.music.settings({})['provision_error'])
        config_tool.install(dict(SECRET, elevenlabs_api_key='valid-key'), self.config, replace=True)
        provision(self.h, self.config)
        self.assertEqual(self.h.music.settings({})['provision_error'], '')

    def test_hidden_prompts_accept_optional_elevenlabs_key(self):
        for optional in ('eleven-private', ''):
            path = self.root / ('both.json' if optional else 'legacy.json')
            with patch.object(sys, 'argv', ['agent-config.py', '--output', str(path)]), \
                    patch.object(config_tool.getpass, 'getpass', side_effect=[SECRET['api_key'], optional]) as hidden, \
                    patch('builtins.input', side_effect=['', '']), patch('sys.stdout', new_callable=io.StringIO) as output:
                config_tool.main()
                self.assertEqual(hidden.call_count, 2)
                self.assertNotIn('eleven-private', output.getvalue())
                self.assertNotIn(SECRET['api_key'], output.getvalue())
            data = read_provision(path)
            self.assertEqual(data.get('elevenlabs_api_key'), optional or None)
            self.assertEqual(data['next_model'], 'openrouter/auto')

    def test_dual_key_emit_receive_and_real_worker_startup(self):
        both = dict(SECRET, elevenlabs_api_key='eleven-private')
        config_tool.install(both, self.config)
        script = str(ROOT / 'RPI/scripts/agent-config.py')
        emitted = subprocess.run([sys.executable, script, '--emit', str(self.config)],
                                 capture_output=True, text=True, check=True)
        # Receive installs under the appliance user's home, including permissions.
        with patch.object(sys, 'argv', ['agent-config.py', '--receive']), \
                patch('sys.stdin', io.StringIO(emitted.stdout)), \
                patch.object(config_tool.Path, 'home', return_value=self.root):
            config_tool.main()
        installed = self.root / '.config/mrow/agent.json'
        self.assertEqual(read_provision(installed), both)
        self.assertEqual(installed.stat().st_mode & 0o777, 0o600)
        command = {'id': 1, 'path': '/api/agent/music/settings'}
        result = subprocess.run([sys.executable, '-E', '-s', '-u',
                    str(ROOT / 'harness/src/worker.py'), '--database', str(self.root / 'child.sqlite3'),
                    '--config', str(installed)], input=json.dumps(command) + '\n',
                    capture_output=True, text=True, timeout=10, check=True)
        reply = json.loads(result.stdout.splitlines()[1])
        self.assertTrue(reply['body']['connected'])
        self.assertTrue(reply['body']['provisioned'])
        for key in (SECRET['api_key'], both['elevenlabs_api_key']):
            self.assertNotIn(key, result.stdout + result.stderr + emitted.stderr)

    def test_slow_advice_does_not_block_feedback_commands(self):
        started, released = threading.Event(), threading.Event()
        output = io.StringIO()

        def dispatch_fake(harness, path, body):
            if path == '/api/agent':
                started.set()
                if not released.wait(2):
                    raise RuntimeError('Feedback was blocked by advice')
                return {'tracks': []}
            self.assertTrue(started.wait(1))
            released.set()
            return {'saved': True}

        incoming = io.StringIO(json.dumps({'id': 1, 'path': '/api/agent'}) + '\n' +
                               json.dumps({'id': 2, 'path': '/api/feedback'}) + '\n')
        with patch('worker.dispatch', side_effect=dispatch_fake):
            serve(self.h, incoming, output)
        replies = [json.loads(line) for line in output.getvalue().splitlines()[1:]]
        self.assertEqual({r['id'] for r in replies}, {1, 2})
        self.assertTrue(all(r['status'] == 200 for r in replies))


if __name__ == '__main__':
    unittest.main()
