#!/usr/bin/env python3
"""Prepare private OpenRouter provisioning for a build/deploy. Never pass keys as arguments."""
import argparse
import getpass
import json
import os
from pathlib import Path
import stat
import sys
import tempfile


def validate(data):
    if not isinstance(data, dict) or set(data) != {'api_key', 'next_model', 'plan_model'}:
        raise ValueError('Expected api_key, next_model and plan_model.')
    if any(not isinstance(v, str) or not v.strip() for v in data.values()):
        raise ValueError('All three fields are required.')
    if (len(data['api_key']) > 4096 or any(ord(c) < 32 or ord(c) > 126 for c in data['api_key'])
            or any(len(data[k]) > 200 for k in ('next_model', 'plan_model'))):
        raise ValueError('Invalid configuration.')
    return data


def read(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd) as source:
        info = os.fstat(source.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or stat.S_IMODE(info.st_mode) & 0o077 or info.st_size > 16384):
            raise ValueError('Configuration must be owned by you with mode 600.')
        return validate(json.load(source))


def install(data, path, replace=False):
    path = path.expanduser().absolute()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = path.parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError('Configuration directory must be owned by you, not a symlink.')
    path.parent.chmod(0o700)
    fd, temporary = tempfile.mkstemp(prefix='.agent-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as output:
            json.dump(validate(data), output)
            output.flush()
            os.fsync(output.fileno())
        if replace:
            os.replace(temporary, path)
        else:
            # Atomic no-overwrite, even if another invocation races this one.
            os.link(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--validate', type=Path, metavar='FILE')
    mode.add_argument('--emit', type=Path, metavar='FILE', help='private SSH pipe input; do not redirect to logs')
    mode.add_argument('--receive', action='store_true', help='install from private SSH stdin on the Pi')
    parser.add_argument('--output', type=Path, default=Path.home() / '.config/mrow-build/agent.json')
    args = parser.parse_args()
    try:
        if args.validate:
            read(args.validate)
        elif args.emit:
            json.dump(read(args.emit), sys.stdout)
        elif args.receive:
            data = sys.stdin.read(16385)
            if len(data) > 16384:
                raise ValueError('Configuration too large.')
            install(json.loads(data), Path.home() / '.config/mrow/agent.json', replace=True)
        else:
            # The secret is read with echo disabled, never via shell arguments.
            data = {'api_key': getpass.getpass('OpenRouter key (hidden): ').strip(),
                    'next_model': input('Next-song model [openrouter/auto]: ').strip() or 'openrouter/auto',
                    'plan_model': input('Setlist model [openrouter/auto]: ').strip() or 'openrouter/auto'}
            install(data, args.output)
            print('Private configuration saved. Use deploy.sh --agent-config with this file.')
    except (OSError, ValueError, TypeError, EOFError):
        # JSON decoder and IO errors may contain source data. Never echo them.
        parser.exit(1, 'Agent config failed: check JSON fields, ownership, mode 600 and output path (must not exist).\n')


if __name__ == '__main__':
    main()
