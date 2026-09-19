"""GPIO crowd button daemon: config validation, debounce, and MIDI messages.

The GPIO and MIDI libraries are only imported inside run(), so these run
anywhere; edges are fed in directly.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src' / 'buttons'))
from mrow_buttons import Debouncer, Config, handle, load_config, main, message  # noqa: E402

MS = 1_000_000


def write(text):
    path = Path(tempfile.mkdtemp()) / 'buttons.toml'
    path.write_text(text)
    return path


class ConfigTests(unittest.TestCase):
    def test_defaults_match_the_shipped_mapping(self):
        config = load_config(Path('/nonexistent/buttons.toml'))
        self.assertEqual(config.port_name, 'MROW Crowd Buttons')
        self.assertEqual({(b.name, b.pin, b.note) for b in config.buttons},
                         {('good', 17, 0x3C), ('mid', 27, 0x3D), ('bad', 22, 0x3E)})

    def test_reads_a_rewired_button(self):
        config = load_config(write('chip = "/dev/gpiochip4"\ndebounce_ms = 5\n'
                                   '[buttons.good]\npin = 5\nnote = 60\n'))
        self.assertEqual(config.chip, '/dev/gpiochip4')
        self.assertEqual((config.buttons[0].pin, config.buttons[0].note), (5, 60))

    def test_rejects_unusable_configurations(self):
        for text in ('[buttons.good]\npin = 99\nnote = 60\n',
                     '[buttons.good]\npin = 5\nnote = 200\n',
                     '[buttons.good]\npin = 5\n',
                     '[buttons.good]\npin = 5\nnote = 60\n[buttons.bad]\npin = 5\nnote = 61\n',
                     '[buttons.good]\npin = 5\nnote = 60\n[buttons.bad]\npin = 6\nnote = 60\n',
                     'channel = 0\n[buttons.good]\npin = 5\nnote = 60\n',
                     'buttons = 3\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                load_config(write(text))

    def test_check_reports_the_wiring(self):
        self.assertEqual(main(['--check', '--config', str(write(
            '[buttons.good]\npin = 5\nnote = 60\n'))]), 0)
        # A broken config exits nonzero instead of starting.
        self.assertEqual(main(['--check', '--config', str(write('[buttons.good]\npin = 99\nnote = 1\n'))]), 2)


class EdgeTests(unittest.TestCase):
    def setUp(self):
        self.config = load_config(Path('/nonexistent'))
        self.debouncer = Debouncer(self.config.debounce_ms)
        self.sent = []

    def press(self, pin, pressed, at_ms):
        return handle(self.config, self.debouncer, pin, pressed, at_ms * MS, self.sent.append)

    def test_press_and_release(self):
        self.assertEqual(self.press(17, True, 0), 'good')
        self.assertEqual(self.press(17, False, 100), 'good')
        # Note on velocity 127, then the same note at velocity 0.
        self.assertEqual(self.sent, [[0x90, 0x3C, 127], [0x90, 0x3C, 0]])

    def test_bounce_is_dropped(self):
        self.assertEqual(self.press(17, True, 0), 'good')
        self.assertIsNone(self.press(17, False, 5))   # within the 30 ms window
        self.assertIsNone(self.press(17, True, 9))
        self.assertEqual(self.press(17, False, 40), 'good')
        self.assertEqual(len(self.sent), 2)

    def test_repeated_state_is_dropped(self):
        self.assertEqual(self.press(22, True, 0), 'bad')
        self.assertIsNone(self.press(22, True, 500))
        self.assertEqual(len(self.sent), 1)

    def test_release_without_a_press_is_ignored(self):
        # Starting up with a button held would otherwise send a stray note off.
        self.assertIsNone(self.press(27, False, 0))
        self.assertEqual(self.sent, [])

    def test_unknown_pin_is_ignored(self):
        self.assertIsNone(self.press(99, True, 0))
        self.assertEqual(self.sent, [])

    def test_channel_is_honoured(self):
        config = load_config(write('channel = 10\n[buttons.good]\npin = 5\nnote = 60\n'))
        self.assertEqual(message(config, config.buttons[0], True), [0x99, 60, 127])


if __name__ == '__main__':
    unittest.main()
