#!/usr/bin/env python3
"""Pi GPIO switches and Qwiic Twist -> BiteDJ virtual MIDI controls.

BCM inputs use pull-ups and active-low software debounce. The Twist shares
a configurable I2C bus with the display. Dependencies are imported only at runtime.
"""
import argparse
import os
import signal
import socket
import sys
import tomllib
import time
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path

DEFAULT_CONFIG = Path('~/.config/mrow/buttons.toml').expanduser()
DEFAULT_PORT_NAME = 'MROW Crowd Buttons'

# Matches mrow-crowd-buttons.midi.xml. Used when no config file exists.
DEFAULT_BUTTONS = {'load1': (17, 0x40), 'load2': (27, 0x41),
                   'good': (22, 0x3C), 'mid': (23, 0x3D), 'bad': (24, 0x3E)}


@dataclass(frozen=True)
class Button:
    name: str
    pin: int
    note: int


@dataclass(frozen=True)
class TwistConfig:
    enabled: bool = True
    bus: int = 1
    address: int = 0x3F
    cc: int = 0x40
    note: int = 0x42
    reverse: bool = False


@dataclass(frozen=True)
class Config:
    chip: str
    port_name: str
    channel: int
    debounce_ms: int
    buttons: tuple
    twist: TwistConfig = TwistConfig()

    def button_for_pin(self, pin):
        return next((b for b in self.buttons if b.pin == pin), None)


def load_config(path=DEFAULT_CONFIG):
    """Read and validate the TOML config; defaults when the file is absent."""
    data = {}
    if path is not None and Path(path).exists():
        with open(path, 'rb') as f:
            data = tomllib.load(f)
    chip = data.get('chip', '/dev/gpiochip0')
    port_name = data.get('port_name', DEFAULT_PORT_NAME)
    channel = data.get('channel', 1)
    debounce_ms = data.get('debounce_ms', 20)
    if not isinstance(chip, str) or not chip:
        raise ValueError('chip must be a GPIO character device path, e.g. /dev/gpiochip0')
    if not isinstance(port_name, str) or not port_name.strip():
        raise ValueError('port_name must be a nonempty string')
    if type(channel) is not int or not 1 <= channel <= 16:
        raise ValueError('channel must be 1-16')
    if type(debounce_ms) is not int or not 0 <= debounce_ms <= 1000:
        raise ValueError('debounce_ms must be 0-1000')

    table = data.get('buttons')
    if table is None:
        table = {name: {'pin': pin, 'note': note} for name, (pin, note) in DEFAULT_BUTTONS.items()}
    if not isinstance(table, dict) or not table:
        raise ValueError('[buttons] must define at least one button')
    buttons = []
    for name, entry in table.items():
        if not isinstance(entry, dict):
            raise ValueError(f'buttons.{name} must be a table with pin and note')
        pin, note = entry.get('pin'), entry.get('note')
        if type(pin) is not int or not 0 <= pin <= 53:
            raise ValueError(f'buttons.{name}.pin must be a GPIO line number (BCM), 0-53')
        if type(note) is not int or not 0 <= note <= 127:
            raise ValueError(f'buttons.{name}.note must be a MIDI note, 0-127')
        buttons.append(Button(name, pin, note))
    for attribute in ('pin', 'note'):
        values = [getattr(b, attribute) for b in buttons]
        if len(set(values)) != len(values):
            raise ValueError(f'Two buttons share a {attribute}')
    twist_data = data.get('twist', {})
    if not isinstance(twist_data, dict):
        raise ValueError('[twist] must be a table')
    if set(twist_data) - set(TwistConfig.__dataclass_fields__):
        raise ValueError('Unknown [twist] option')
    twist = TwistConfig(**twist_data)
    for name in ('enabled', 'reverse'):
        if type(getattr(twist, name)) is not bool:
            raise ValueError(f'twist.{name} must be true or false')
    for name, low, high in (('bus', 0, 255), ('address', 0x08, 0x77),
                            ('cc', 0, 127), ('note', 0, 127)):
        value = getattr(twist, name)
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f'twist.{name} must be {low}-{high}')
    if twist.enabled and twist.note in [b.note for b in buttons]:
        raise ValueError('Twist pushbutton and GPIO buttons must use different notes')
    if twist.enabled and twist.bus == 1 and any(b.pin in (2, 3) for b in buttons):
        raise ValueError('GPIO2/3 are reserved for the shared I2C bus 1')
    return Config(chip, port_name.strip(), channel, debounce_ms, tuple(buttons), twist)


class Debouncer:
    """Accept a state only after it remains stable for the entire window.

    Called on every sample, including unchanged levels: a release that bounces
    inside the window is eventually delivered rather than leaving a note held.
    Inputs held at startup must be released before they can trigger an action.
    """

    def __init__(self, window_ms):
        self.window_ns = window_ms * 1_000_000
        self.last = {}

    def accept(self, pin, pressed, timestamp_ns):
        if pin not in self.last:
            self.last[pin] = (pressed, pressed, timestamp_ns)
            return False
        stable, candidate, since = self.last[pin]
        if candidate != pressed:
            candidate, since = pressed, timestamp_ns
        changed = stable != candidate and timestamp_ns - since >= self.window_ns
        self.last[pin] = (candidate if changed else stable, candidate, since)
        return changed


class Twist:
    """Read-only register access; never resets counts or writes to the display.

    SparkFun firmware: count is little-endian at 0x05; status bit 1 is
    the current button level (bit 2 is a latched click, not a level).
    https://github.com/sparkfun/Qwiic_Twist/blob/master/Firmware/Qwiic_Twist/Qwiic_Twist.ino
    """

    def __init__(self, bus, config, send):
        self.bus, self.config, self.send = bus, config, send
        if bus.read_byte_data(config.twist.address, 0x00) != 0x5C:
            raise OSError('I2C device is not a Qwiic Twist (expected ID 0x5C)')
        self.count = None
        self.debouncer = Debouncer(config.debounce_ms)
        self.pressed = False

    def poll(self, now_ns):
        t = self.config.twist
        # Read both before changing state: a failed transaction emits no events.
        data = self.bus.read_i2c_block_data(t.address, 0x05, 2)
        if len(data) != 2:
            raise OSError('Short Twist count read')
        count = int.from_bytes(bytes(data), 'little')
        pressed = bool(self.bus.read_byte_data(t.address, 0x01) & 0x02)
        if self.count is not None:
            delta = (count - self.count + 32768) % 65536 - 32768
            if t.reverse:
                delta = -delta
            # Bound stale/reset jumps; reconnect always establishes a baseline.
            if abs(delta) <= 96:
                for _ in range(abs(delta)):
                    self.send([0xB0 | (self.config.channel - 1), t.cc,
                               1 if delta > 0 else 127])
        self.count = count
        if self.debouncer.accept('twist', pressed, now_ns):
            self.pressed = pressed
            self.send([0x90 | (self.config.channel - 1), t.note, 127 if pressed else 0])

    def release(self):
        if self.pressed:
            self.send([0x90 | (self.config.channel - 1), self.config.twist.note, 0])
            self.pressed = False


def message(config, button, pressed):
    """Note on; velocity 0 is the release (running-status friendly)."""
    return [0x90 | (config.channel - 1), button.note, 127 if pressed else 0]


def handle(config, debouncer, pin, pressed, timestamp_ns, send):
    """Translate one GPIO level sample. Returns the button name when a note was sent."""
    button = config.button_for_pin(pin)
    if button is None or not debouncer.accept(pin, pressed, timestamp_ns):
        return None
    send(message(config, button, pressed))
    return button.name


def notify_systemd(state):
    """sd_notify without libsystemd: lets `systemctl start` wait until the MIDI
    port exists, so BiteDJ (started right after) finds it when it scans."""
    address = os.environ.get('NOTIFY_SOCKET')
    if not address:
        return
    if address.startswith('@'):
        address = '\0' + address[1:]
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        sock.connect(address)
        sock.sendall(state.encode())


def run(config):
    import gpiod
    import rtmidi
    from gpiod.line import Bias, Direction, Value

    settings = gpiod.LineSettings(direction=Direction.INPUT, bias=Bias.PULL_UP)
    pins = tuple(b.pin for b in config.buttons)
    debouncer = Debouncer(config.debounce_ms)
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    with ExitStack() as stack:
        midi = rtmidi.MidiOut(name=config.port_name)
        midi.open_virtual_port(config.port_name)
        stack.callback(midi.close_port)
        request = stack.enter_context(gpiod.request_lines(
            config.chip, consumer='mrow-buttons', config={pins: settings}))
        print(f'mrow-buttons: {config.port_name!r} on {config.chip}: ' +
              ', '.join(f'{b.name}=GPIO{b.pin}->note {b.note}' for b in config.buttons), flush=True)
        notify_systemd('READY=1')
        bus = twist = None
        next_twist = 0
        try:
            while not stopping:
                now = time.monotonic_ns()
                for pin, value in zip(pins, request.get_values(pins)):
                    pressed = value == Value.INACTIVE  # physical LOW; active_low is false
                    name = handle(config, debouncer, pin, pressed, now, midi.send_message)
                    if name and pressed:
                        print(f'mrow-buttons: {name}', flush=True)
                if config.twist.enabled and now >= next_twist:
                    next_twist = now + 10_000_000
                    try:
                        if bus is None:
                            from smbus2 import SMBus
                            bus = SMBus(config.twist.bus)
                        if twist is None:
                            twist = Twist(bus, config, midi.send_message)
                            print(f'mrow-buttons: Twist on I2C {config.twist.bus} '
                                  f'address 0x{config.twist.address:02x}', flush=True)
                        twist.poll(now)
                    except (OSError, ImportError) as error:
                        if twist is not None:
                            twist.release()
                        if bus is not None:
                            bus.close()
                        bus = twist = None
                        next_twist = now + 2_000_000_000
                        print(f'mrow-buttons: Twist unavailable: {error}; retry in 2s',
                              file=sys.stderr, flush=True)
                time.sleep(0.005)
        finally:
            if twist is not None:
                twist.release()
            if bus is not None:
                bus.close()
            for button in config.buttons:
                midi.send_message(message(config, button, False))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--check', action='store_true', help='Validate the config and exit')
    args = parser.parse_args(argv)
    try:
        config = load_config(args.config)
    except (ValueError, tomllib.TOMLDecodeError) as error:
        print(f'mrow-buttons: {args.config}: {error}', file=sys.stderr)
        return 2
    if args.check:
        for b in config.buttons:
            print(f'{b.name}: GPIO{b.pin} -> note {b.note} (0x{b.note:02X}), channel {config.channel}')
        print(f'Twist: enabled={config.twist.enabled}, bus={config.twist.bus}, '
              f'address=0x{config.twist.address:02X}, CC={config.twist.cc}, '
              f'push note={config.twist.note}')
        return 0
    run(config)
    return 0


if __name__ == '__main__':
    sys.exit(main())
