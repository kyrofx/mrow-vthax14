"""GPIO crowd button daemon: config validation, debounce, and MIDI messages.

The GPIO and MIDI libraries are only imported inside run(), so these run
anywhere; edges are fed in directly.
"""
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from dataclasses import replace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src' / 'buttons'))
from mrow_buttons import Debouncer, Config, Twist, handle, load_config, main, message  # noqa: E402

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
                         {('load1', 17, 0x40), ('load2', 27, 0x41),
                          ('good', 22, 0x3C), ('mid', 23, 0x3D), ('bad', 24, 0x3E)})

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


class MappingTests(unittest.TestCase):
    def test_every_default_input_has_the_expected_action(self):
        root = Path(__file__).resolve().parents[2] / 'Mixxx/bitedj/res/controllers'
        mapping = ET.parse(root / 'mrow-crowd-buttons.midi.xml')
        controls = {(int(c.findtext('status'), 16), int(c.findtext('midino'), 16)):
                    (c.findtext('group'), c.findtext('key'))
                    for c in mapping.findall('.//controls/control')}
        self.assertEqual(controls, {
            (0x90, 64): ('[Channel1]', 'LoadSelectedTrack'),
            (0x90, 65): ('[Channel2]', 'LoadSelectedTrack'),
            (0x90, 60): ('[Harness]', 'rate_good'),
            (0x90, 61): ('[Harness]', 'rate_mid'),
            (0x90, 62): ('[Harness]', 'rate_bad'),
            (0x90, 66): ('[Library]', 'MROWControls.browsePress'),
            (0xB0, 64): ('[Library]', 'MROWControls.browseRotate'),
        })
        self.assertEqual(mapping.findtext('.//info/name'), load_config(None).port_name)
        for script in mapping.findall('.//scriptfiles/file'):
            self.assertTrue((root / script.attrib['filename']).is_file())


class EdgeTests(unittest.TestCase):
    def setUp(self):
        self.config = load_config(Path('/nonexistent'))
        self.debouncer = Debouncer(self.config.debounce_ms)
        self.sent = []
        for button in self.config.buttons:
            self.press(button.pin, False, 0)

    def press(self, pin, pressed, at_ms):
        return handle(self.config, self.debouncer, pin, pressed, at_ms * MS, self.sent.append)

    def test_each_switch_and_held_press(self):
        for pin, name, note in ((17, 'load1', 64), (27, 'load2', 65),
                                (22, 'good', 60), (23, 'mid', 61), (24, 'bad', 62)):
            self.assertIsNone(self.press(pin, True, 10))
            self.assertEqual(self.press(pin, True, 30), name)
            self.assertIsNone(self.press(pin, True, 100))
            self.assertIsNone(self.press(pin, False, 110))
            self.assertEqual(self.press(pin, False, 130), name)
            self.assertEqual(self.sent[-2:], [[0x90, note, 127], [0x90, note, 0]])

    def test_bounce_requires_stable_press_and_release(self):
        for at, pressed in ((1, True), (5, False), (9, True), (28, True)):
            self.assertIsNone(self.press(22, pressed, at))
        self.assertEqual(self.press(22, True, 29), 'good')
        for at, pressed in ((30, False), (32, True), (35, False), (54, False)):
            self.assertIsNone(self.press(22, pressed, at))
        self.assertEqual(self.press(22, False, 55), 'good')
        self.press(22, True, 60)
        self.assertEqual(self.press(22, True, 80), 'good')
        self.assertEqual(self.sent, [[0x90, 60, 127], [0x90, 60, 0], [0x90, 60, 127]])

    def test_startup_held_does_not_load(self):
        self.debouncer = Debouncer(20)
        self.assertIsNone(self.press(17, True, 0))
        self.assertIsNone(self.press(17, True, 100))
        self.press(17, False, 110)
        self.press(17, False, 130)
        self.assertNotIn([0x90, 64, 127], self.sent)

    def test_short_pulse_and_unknown_pin_are_ignored(self):
        self.press(22, True, 1)
        self.press(22, False, 10)
        self.press(22, False, 100)
        self.press(99, True, 100)
        self.assertEqual(self.sent, [])

    def test_channel_is_honoured(self):
        config = load_config(write('channel = 10\n[buttons.good]\npin = 5\nnote = 60\n'))
        self.assertEqual(message(config, config.buttons[0], True), [0x99, 60, 127])


class FakeBus:
    def __init__(self):
        self.count = 0
        self.status = 0
        self.id = 0x5C
        self.fail = False
        self.addresses = []

    def read_byte_data(self, address, register):
        self.addresses.append(address)
        if self.fail:
            raise OSError('disconnected')
        return self.id if register == 0 else self.status

    def read_i2c_block_data(self, address, register, length):
        self.addresses.append(address)
        assert register == 5 and length == 2
        return list((self.count & 0xffff).to_bytes(2, 'little'))


class TwistTests(unittest.TestCase):
    def setUp(self):
        self.bus = FakeBus()
        self.config = load_config(None)
        self.sent = []
        self.twist = Twist(self.bus, self.config, self.sent.append)

    def test_baseline_direction_multiple_ticks_and_wrap(self):
        self.bus.count = 32767
        self.twist.poll(0)
        self.assertEqual(self.sent, [])
        self.bus.count = -32767
        self.twist.poll(10 * MS)
        self.assertEqual(self.sent, [[0xB0, 64, 1]] * 2)
        self.bus.count = 32766
        self.twist.poll(20 * MS)
        self.assertEqual(self.sent[-3:], [[0xB0, 64, 127]] * 3)
        self.assertEqual(set(self.bus.addresses), {0x3F})

    def test_push_level_debounced_not_latched_click(self):
        self.twist.poll(0)
        self.bus.status = 2
        self.twist.poll(10 * MS)
        self.twist.poll(30 * MS)
        self.twist.poll(100 * MS)
        self.bus.status = 4  # stale click flag must not look pressed
        self.twist.poll(110 * MS)
        self.twist.poll(130 * MS)
        self.assertEqual(self.sent, [[0x90, 66, 127], [0x90, 66, 0]])

    def test_failure_does_not_emit_partial_sample_and_reconnect_rebaselines(self):
        self.twist.poll(0)
        self.bus.count = 10
        self.bus.fail = True
        with self.assertRaises(OSError):
            self.twist.poll(10 * MS)
        self.assertEqual(self.sent, [])
        self.bus.fail = False
        self.twist = Twist(self.bus, self.config, self.sent.append)
        self.twist.poll(20 * MS)
        self.assertEqual(self.sent, [])

    def test_wrong_device_rejected(self):
        self.bus.id = 0x42
        with self.assertRaises(OSError):
            Twist(self.bus, self.config, self.sent.append)

    def test_reverse_channel_and_address(self):
        config = load_config(write('channel=2\n[twist]\nreverse=true\naddress=0x3E\n'))
        twist = Twist(self.bus, config, self.sent.append)
        twist.poll(0)
        self.bus.count = 1
        twist.poll(10 * MS)
        self.assertEqual(self.sent, [[0xB1, 64, 127]])
        self.assertEqual(self.bus.addresses[-1], 0x3E)

    def test_reset_sized_jump_ignored(self):
        self.twist.poll(0)
        self.bus.count = 1000
        self.twist.poll(10 * MS)
        self.assertEqual(self.sent, [])

    def test_invalid_config(self):
        for text in ('[twist]\naddress=128', '[twist]\nbus=-1',
                     '[twist]\nenabled="yes"', '[twist]\nreverse=1',
                     '[twist]\nnote=60', '[twist]\ncc=128',
                     '[twist]\nbuss=1', '[buttons.good]\npin=2\nnote=60'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                load_config(write(text))

    def test_shipped_config_matches_defaults(self):
        path = Path(__file__).resolve().parents[1] / 'config/home/.config/mrow/buttons.toml'
        default = load_config(None)
        self.assertEqual(load_config(path), replace(default, twist=replace(default.twist, bus=10)))


if __name__ == '__main__':
    unittest.main()
