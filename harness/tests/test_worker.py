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
