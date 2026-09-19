#!/usr/bin/env python3
"""GPIO crowd buttons -> MIDI notes on a virtual port, for BiteDJ.

Each momentary button is wired between a GPIO pin and ground (the pin's pull-up
is enabled here). A press sends note on with velocity 127 on the "MROW Crowd
Buttons" port, a release sends velocity 0. BiteDJ picks the port up like any
controller and applies the hidden mapping res/controllers/mrow-crowd-buttons.midi.xml,
which turns the notes into [Harness],rate_good / rate_mid / rate_bad.

Remapping has two layers:
  pin -> note     ~/.config/mrow/buttons.toml (rewiring a button)
  note -> action  the Mixxx mapping (what a button does)

Needs python3-libgpiod (v2) and python3-rtmidi on the device; both are only
imported by run(), so the configuration and event handling are testable
anywhere.
"""
import argparse
import os
import signal
import socket
import sys
import tomllib
from dataclasses import dataclass
from datetime import timedelta
from pathlib import Path

DEFAULT_CONFIG = Path('~/.config/mrow/buttons.toml').expanduser()
DEFAULT_PORT_NAME = 'MROW Crowd Buttons'

# Matches mrow-crowd-buttons.midi.xml. Used when no config file exists.
DEFAULT_BUTTONS = {'good': (17, 0x3C), 'mid': (27, 0x3D), 'bad': (22, 0x3E)}


@dataclass(frozen=True)
class Button:
    name: str
    pin: int
    note: int


@dataclass(frozen=True)
class Config:
    chip: str
    port_name: str
    channel: int
    debounce_ms: int
    buttons: tuple

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
    debounce_ms = data.get('debounce_ms', 30)
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
    return Config(chip, port_name.strip(), channel, debounce_ms, tuple(buttons))


class Debouncer:
    """Drops edges that follow the previous accepted edge on the same pin too
    closely, and repeats of the state a pin is already in. The kernel debounces
    too when the GPIO controller supports it; this is the backstop."""

    def __init__(self, window_ms):
        self.window_ns = window_ms * 1_000_000
        self.last = {}  # pin -> (pressed, timestamp_ns)

    def accept(self, pin, pressed, timestamp_ns):
        previous = self.last.get(pin)
        if previous is not None:
            was_pressed, when = previous
            if was_pressed == pressed or timestamp_ns - when < self.window_ns:
                return False
        elif not pressed:
            return False  # A release with no press seen (started mid-press).
        self.last[pin] = (pressed, timestamp_ns)
        return True


def message(config, button, pressed):
    """Note on; velocity 0 is the release (running-status friendly)."""
    return [0x90 | (config.channel - 1), button.note, 127 if pressed else 0]


def handle(config, debouncer, pin, pressed, timestamp_ns, send):
    """Translate one GPIO edge. Returns the button name when a note was sent."""
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
    from gpiod.line import Bias, Direction, Edge

    midi = rtmidi.MidiOut(name=config.port_name)
    midi.open_virtual_port(config.port_name)
    settings = gpiod.LineSettings(direction=Direction.INPUT, bias=Bias.PULL_UP,
                                  edge_detection=Edge.BOTH,
                                  debounce_period=timedelta(milliseconds=config.debounce_ms))
    pins = tuple(b.pin for b in config.buttons)
    debouncer = Debouncer(config.debounce_ms)
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    with gpiod.request_lines(config.chip, consumer='mrow-buttons', config={pins: settings}) as request:
        print(f'mrow-buttons: {config.port_name!r} on {config.chip}: ' +
              ', '.join(f'{b.name}=GPIO{b.pin}->note {b.note}' for b in config.buttons), flush=True)
        notify_systemd('READY=1')
        while not stopping:
            if not request.wait_edge_events(timedelta(seconds=1)):
                continue
            for event in request.read_edge_events():
                # Active low: the button pulls the pin to ground.
                pressed = event.event_type == gpiod.EdgeEvent.Type.FALLING_EDGE
                name = handle(config, debouncer, event.line_offset, pressed,
                              event.timestamp_ns, midi.send_message)
                if name and pressed:
                    print(f'mrow-buttons: {name}', flush=True)
    midi.close_port()


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
        return 0
    run(config)
    return 0


if __name__ == '__main__':
    sys.exit(main())
