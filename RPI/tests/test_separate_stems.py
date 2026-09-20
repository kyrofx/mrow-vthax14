"""The offline stem separation pass.

demucs and ffmpeg are not run here: the commands are built as data and the
runner is injected, so what is tested is the layout, the staleness rule and
the atomic swap — the parts the device depends on.
"""
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'separate_stems', Path(__file__).resolve().parents[1] / 'scripts' / 'separate-stems.py')
stems = importlib.util.module_from_spec(spec)
sys.modules['separate_stems'] = stems
spec.loader.exec_module(stems)


class FakeRun:
    """Stands in for subprocess.run; records argv and fakes demucs' output."""

    def __init__(self, probe_json=None, fail=None):
        self.calls = []
        self.probe_json = probe_json or '{"streams":[{"sample_rate":"48000","duration":"137.2"}]}'
        self.fail = fail

    def __call__(self, argv, check=False, capture_output=False):
        self.calls.append(argv)
        if self.fail and argv[0] == self.fail:
            raise subprocess.CalledProcessError(1, argv)
        if argv[0] == 'demucs':
            out = Path(argv[argv.index('-o') + 1]) / 'htdemucs' / 'track'
            out.mkdir(parents=True, exist_ok=True)
            (out / 'vocals.wav').write_bytes(b'RIFF')
            (out / 'no_vocals.wav').write_bytes(b'RIFF')
        if argv[0] == 'ffmpeg':
            Path(argv[-1]).write_bytes(b'OggS')
        if argv[0] == 'ffprobe':
            return subprocess.CompletedProcess(argv, 0, stdout=self.probe_json)
        return subprocess.CompletedProcess(argv, 0)


class StemsTestCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.music = Path(self.temp.name).resolve() / 'Music'
        self.music.mkdir()
        self.track = self.music / 'track.mp3'
        self.track.write_bytes(b'x' * 4096)
        self.log = []

    def tearDown(self):
        self.temp.cleanup()

    def run_main(self, *args):
        self.runner = FakeRun()
        return stems.main([str(a) for a in args], run=self.runner, log=self.log.append,
                          which=lambda tool: '/usr/bin/' + tool)


class LayoutTests(StemsTestCase):
    def test_stems_live_beside_the_track(self):
        self.assertEqual(stems.stems_dir(self.track).name, 'track.mp3.stems')
        self.assertEqual(stems.stems_dir(self.track).parent, self.music)

    def test_separation_writes_both_stems_and_a_manifest(self):
        manifest = stems.separate(self.track, 'htdemucs', '128k', FakeRun(), self.log.append)
        directory = stems.stems_dir(self.track)
        self.assertTrue((directory / 'vocals.opus').exists())
        self.assertTrue((directory / 'instrumental.opus').exists())
        self.assertEqual(manifest['source'], {'name': 'track.mp3', 'bytes': 4096})
        self.assertEqual((manifest['sample_rate'], manifest['frames']), (48000, 6585600))
        self.assertEqual(json.loads((directory / 'manifest.json').read_text()), manifest)

    def test_commands_ask_for_a_two_stem_split_and_opus(self):
        run = FakeRun()
        stems.separate(self.track, 'htdemucs', '96k', run, self.log.append)
        demucs = next(c for c in run.calls if c[0] == 'demucs')
        self.assertIn('--two-stems=vocals', demucs)
        self.assertEqual(demucs[demucs.index('-n') + 1], 'htdemucs')
        encodes = [c for c in run.calls if c[0] == 'ffmpeg']
        self.assertEqual(len(encodes), 2)
        for call in encodes:
            self.assertEqual(call[call.index('-c:a') + 1], 'libopus')
            self.assertEqual(call[call.index('-b:a') + 1], '96k')

    def test_a_failed_run_leaves_no_half_written_stems(self):
        # Half-written stems are worse than none: the device would try them.
        with self.assertRaises(subprocess.CalledProcessError):
            stems.separate(self.track, 'htdemucs', '128k', FakeRun(fail='ffmpeg'), self.log.append)
        self.assertFalse(stems.stems_dir(self.track).exists())

    def test_redoing_a_track_replaces_the_old_stems(self):
        stems.separate(self.track, 'htdemucs', '128k', FakeRun(), self.log.append)
        stale = stems.stems_dir(self.track) / 'leftover.opus'
        stale.write_bytes(b'old')
        stems.separate(self.track, 'htdemucs', '128k', FakeRun(), self.log.append)
        self.assertFalse(stale.exists())


class StalenessTests(StemsTestCase):
    def setUp(self):
        super().setUp()
        stems.separate(self.track, 'htdemucs', '128k', FakeRun(), self.log.append)
        self.directory = stems.stems_dir(self.track)

    def test_current_stems_are_recognised(self):
        self.assertTrue(stems.is_current(self.track, self.directory))

    def test_a_re_encoded_track_invalidates_its_stems(self):
        self.track.write_bytes(b'y' * 5000)
        self.assertFalse(stems.is_current(self.track, self.directory))

    def test_damage_invalidates_the_set(self):
        for break_it, restore in (
                (lambda: (self.directory / 'vocals.opus').unlink(),
                 lambda: (self.directory / 'vocals.opus').write_bytes(b'OggS')),
                (lambda: (self.directory / 'manifest.json').unlink(), None),
        ):
            break_it()
            self.assertFalse(stems.is_current(self.track, self.directory))
            if restore:
                restore()

    def test_a_manifest_from_a_future_version_is_not_trusted(self):
        manifest = json.loads((self.directory / 'manifest.json').read_text())
        manifest['version'] = stems.MANIFEST_VERSION + 1
        (self.directory / 'manifest.json').write_text(json.dumps(manifest))
        self.assertFalse(stems.is_current(self.track, self.directory))


class WalkTests(StemsTestCase):
    def test_walks_a_drive_and_ignores_its_own_output(self):
        (self.music / 'sub').mkdir()
        (self.music / 'sub' / 'other.flac').write_bytes(b'fLaC')
        (self.music / 'notes.txt').write_bytes(b'hi')
        stems.separate(self.track, 'htdemucs', '128k', FakeRun(), self.log.append)
        found = stems.find_tracks([self.music])
        self.assertEqual({p.name for p in found}, {'track.mp3', 'other.flac'})


class MainTests(StemsTestCase):
    def test_skips_tracks_that_already_have_current_stems(self):
        self.assertEqual(self.run_main(self.track), 0)
        self.assertEqual(self.run_main(self.track), 0)
        self.assertIn('0 to separate', ' '.join(self.log))
        self.assertEqual([c for c in self.runner.calls if c[0] == 'demucs'], [])

    def test_force_redoes_them(self):
        self.run_main(self.track)
        self.run_main(self.track, '--force')
        self.assertEqual(len([c for c in self.runner.calls if c[0] == 'demucs']), 1)

    def test_dry_run_changes_nothing(self):
        self.assertEqual(self.run_main(self.track, '--dry-run'), 0)
        self.assertEqual(self.runner.calls, [])
        self.assertFalse(stems.stems_dir(self.track).exists())
        self.assertIn('would separate', ' '.join(self.log))

    def test_no_audio_found_is_reported(self):
        empty = Path(self.temp.name) / 'empty'
        empty.mkdir()
        self.assertEqual(self.run_main(empty), 1)

    def test_one_failure_does_not_stop_the_rest(self):
        second = self.music / 'second.mp3'
        second.write_bytes(b'x' * 2048)
        runner = FakeRun(fail='demucs')
        code = stems.main([str(self.music)], run=runner, log=self.log.append,
                          which=lambda tool: '/usr/bin/' + tool)
        self.assertEqual(code, 1)
        self.assertEqual(len([c for c in runner.calls if c[0] == 'demucs']), 2)
        self.assertIn('2 track(s) failed', ' '.join(self.log))


class LibraryPreparationTests(StemsTestCase):
    def test_overlapping_library_paths_are_deduplicated(self):
        self.assertEqual(stems.find_tracks([self.music, self.track]), [self.track])

    def test_partial_stems_and_appledouble_files_are_ignored(self):
        partial = self.music / 'track.mp3.stems.partial'
        partial.mkdir()
        (partial / 'vocals.opus').write_bytes(b'incomplete')
        (self.music / '._track.mp3').write_bytes(b'metadata')
        self.assertEqual(stems.find_tracks([self.music]), [self.track])

    def test_device_and_segment_reach_demucs(self):
        runner = FakeRun()
        stems.separate(self.track, 'htdemucs', '128k', runner, self.log.append,
                       device='cuda', segment=7)
        command = runner.calls[0]
        self.assertEqual(command[command.index('--device') + 1], 'cuda')
        self.assertEqual(command[command.index('--segment') + 1], '7')

    def test_report_records_failures_and_rerun_resumes(self):
        report = Path(self.temp.name) / 'report.json'
        second = self.music / 'second.mp3'
        second.write_bytes(b'music')
        fake = FakeRun()

        def run(argv, **kwargs):
            if argv[0] == 'demucs' and argv[-1] == str(second):
                raise subprocess.CalledProcessError(1, argv)
            return fake(argv, **kwargs)

        code = stems.main([str(self.music), '--report', str(report)], run=run,
                          log=self.log.append, which=lambda _: '/bin/tool')
        self.assertEqual(code, 1)
        data = json.loads(report.read_text())
        self.assertEqual(data['completed'], [str(self.track)])
        self.assertEqual(data['failed'][0]['path'], str(second))
        self.assertEqual(self.run_main(self.music, '--report', report), 0)
        data = json.loads(report.read_text())
        self.assertEqual(data['skipped'], 1)
        self.assertEqual(data['completed'], [str(second)])

    def test_interrupted_run_saves_report(self):
        report = Path(self.temp.name) / 'report.json'

        def interrupted(*args, **kwargs):
            raise KeyboardInterrupt()

        code = stems.main([str(self.track), '--report', str(report)], run=interrupted,
                          log=self.log.append, which=lambda _: '/bin/tool')
        self.assertEqual(code, 130)
        self.assertTrue(json.loads(report.read_text())['interrupted'])
        self.assertFalse(stems.stems_dir(self.track).exists())

    def test_missing_library_is_an_error(self):
        self.assertEqual(self.run_main(self.music / 'missing', '--dry-run'), 2)


if __name__ == '__main__':
    unittest.main()
