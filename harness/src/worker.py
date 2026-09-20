"""Mixxx's bundled agent worker. JSON lines over private stdin/stdout; no server."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import sqlite3
import stat
import sys
import threading

from harness import Harness, dispatch


def read_provision(path):
    """Reject symlinks, other owners and group/world-readable credential files."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd) as source:
        info = os.fstat(source.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or stat.S_IMODE(info.st_mode) & 0o077 or info.st_size > 16384):
            raise ValueError('Agent config must be an owner-only regular file (chmod 600).')
        config = json.load(source)
    required = {'api_key', 'next_model', 'plan_model'}
    if (not isinstance(config, dict) or not config
            or (bool(required & set(config)) and not required <= set(config))
            or set(config) - required - {'elevenlabs_api_key'}):
        raise ValueError('Agent config needs api_key, next_model, plan_model and optional elevenlabs_api_key.')
    # Validate the entire file before configuring either provider.
    if (any(not isinstance(v, str) or not v.strip() for v in config.values())
            or any(len(config[k]) > 4096 or any(ord(c) < 32 or ord(c) > 126 for c in config[k])
                   for k in ('api_key', 'elevenlabs_api_key') if k in config)
            or any(len(config[k]) > 200 for k in ('next_model', 'plan_model') if k in config)):
        raise ValueError('Invalid agent configuration.')
    return config


def provision(harness, path):
    if not path.exists() and not path.is_symlink():
        return
    try:
        config = read_provision(path)
        if 'api_key' in config:
            harness.agent.configure({k: config[k] for k in ('api_key', 'next_model', 'plan_model')})
            harness.agent.provisioned = True
        harness.agent.provision_error = ''
        if 'elevenlabs_api_key' in config:
            harness.music.settings({'api_key': config['elevenlabs_api_key']})
            harness.music.provisioned = True
        harness.music.provision_error = ''
    except (OSError, ValueError, TypeError):
        # A malformed secret must neither break local suggestions nor be echoed.
        error = 'Provisioned config rejected. Check ownership, chmod 600 and JSON fields.'
        harness.agent.provision_error = error
        harness.music.provision_error = error


def serve(harness, incoming, outgoing):
    output_lock = threading.Lock()

    def emit(value):
        with output_lock:
            outgoing.write(json.dumps(value, allow_nan=False) + '\n')
            outgoing.flush()

    def execute(request):
        try:
            result = dispatch(harness, request['path'], request.get('body', {}))
            response = {'id': request['id'], 'status': 200, 'body': result}
        except LookupError:
            response = {'id': request['id'], 'status': 404, 'body': {'error': 'Unknown command'}}
        except (ValueError, TypeError, sqlite3.IntegrityError) as error:
            response = {'id': request['id'], 'status': 400, 'body': {'error': str(error)}}
        except Exception:
            # Never print request bodies or exceptions containing credentials.
            response = {'id': request['id'], 'status': 500, 'body': {'error': 'Agent command failed'}}
        emit(response)

    emit({'ready': True})
    with ThreadPoolExecutor(max_workers=2, thread_name_prefix='agent') as pool:
        for line in incoming:
            try:
                if len(line) > 5_000_000:
                    raise ValueError('Command too large')
                request = json.loads(line)
                if (not isinstance(request, dict) or type(request.get('id')) is not int
                        or not isinstance(request.get('path'), str)
                        or not isinstance(request.get('body', {}), dict)):
                    raise ValueError('Invalid command')
            except (ValueError, TypeError):
                emit({'id': None, 'status': 400, 'body': {'error': 'Invalid command'}})
                continue
            # Plays, ratings, configuration and catalog writes stay FIFO and
            # responsive while a model call is running on a separate thread.
            if request['path'] in ('/api/agent', '/api/agent/models', '/api/recommend', '/api/setlist'):
                pool.submit(execute, request)
            else:
                execute(request)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', required=True, type=Path)
    parser.add_argument('--config', type=Path, default=Path.home() / '.config/mrow/agent.json')
    args = parser.parse_args()
    os.umask(0o077)
    args.database.parent.mkdir(parents=True, exist_ok=True)
    if hasattr(os, 'nice'):
        os.nice(10)  # Keep CPU-heavy planning below the audio engine's priority.
    harness = Harness(args.database)
    provision(harness, args.config)
    serve(harness, sys.stdin, sys.stdout)


if __name__ == '__main__':
    main()
