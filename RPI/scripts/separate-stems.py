#!/usr/bin/env python3
"""Separate tracks into vocal and instrumental stems, beside the track.

Runs on a workstation. The output travels with the USB drive, so a stick carries its own
stems the way it already carries its own history and cue points.

    Music/Artist - Title.mp3
    Music/Artist - Title.mp3.stems/
        vocals.opus
        instrumental.opus
        manifest.json

BiteDJ plays the two stems in place of the original when the manifest matches
the track it is loading (see RPI/bitedj_docs/stems.md). Needs `demucs` and
`ffmpeg` on PATH.

    ./separate-stems.py /Volumes/MUSIC/Contents          # a drive, or a folder
    ./separate-stems.py track.mp3 --dry-run              # what it would do
    ./separate-stems.py track.mp3 --force                # redo existing stems
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MANIFEST_VERSION = 1
STEMS_SUFFIX = '.stems'
MANIFEST_NAME = 'manifest.json'
VOCALS = 'vocals.opus'
INSTRUMENTAL = 'instrumental.opus'
AUDIO_SUFFIXES = {'.mp3', '.m4a', '.aac', '.flac', '.wav', '.aiff', '.aif', '.ogg', '.opus'}
DEFAULT_MODEL = 'htdemucs'
DEFAULT_BITRATE = '128k'


def stems_dir(track):
    """Where a track's stems live: alongside it, named after it."""
    return track.with_name(track.name + STEMS_SUFFIX)


def build_manifest(track, model, bitrate, frames=None, sample_rate=None):
    """What the device checks before it trusts a stem set.

    The source's size is the guard against stale stems: a re-encoded or
    replaced track keeps its name but not its size, and checking it costs a
    stat rather than a re-read of the whole file.
    """
    return {
        'version': MANIFEST_VERSION,
        'model': model,
        'bitrate': bitrate,
        'created': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'source': {'name': track.name, 'bytes': track.stat().st_size},
        'frames': frames,
        'sample_rate': sample_rate,
        'stems': {'vocals': VOCALS, 'instrumental': INSTRUMENTAL},
    }


def read_manifest(directory):
    try:
        with open(Path(directory) / MANIFEST_NAME, 'rb') as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        return None
    return manifest if isinstance(manifest, dict) else None


def is_current(track, directory):
    """True when `directory` holds stems that belong to this exact track."""
    manifest = read_manifest(directory)
    if not manifest or manifest.get('version') != MANIFEST_VERSION:
        return False
    source = manifest.get('source')
    if not isinstance(source, dict):
        return False
    if source.get('name') != track.name or source.get('bytes') != track.stat().st_size:
        return False
    return all((Path(directory) / name).exists() for name in (VOCALS, INSTRUMENTAL))


def find_tracks(paths):
    """Audio files under the given files or directories, skipping stem output."""
    found = {}
    for path in paths:
        path = Path(path).expanduser().resolve()
        if path.is_dir():
            candidates = []
            for root, dirs, files in os.walk(path):
                dirs[:] = sorted(d for d in dirs if not d.startswith('.')
                                 and not d.endswith(('.stems', '.stems.partial')))
                candidates.extend(Path(root) / name for name in sorted(files))
        else:
            candidates = [path]
        for candidate in candidates:
            if (not candidate.name.startswith('.') and candidate.is_file()
                    and candidate.suffix.lower() in AUDIO_SUFFIXES
                    and not any(p.name.endswith(('.stems', '.stems.partial'))
                                for p in candidate.parents)):
                found.setdefault(candidate.resolve(), candidate)
    return list(found.values())


def demucs_command(track, out_dir, model, device="cpu", segment=4):
    # --two-stems=vocals gives exactly the split we play: vocals, and
    # everything else summed. Anything finer would cost CPU on the device for
    # a control the DJ does not have.
    return ['demucs', '--two-stems=vocals', '--device', device, '--segment', str(segment),
            '--shifts', '1', '--jobs', '0', '--float32', '-n', model,
            '-o', str(out_dir), str(track)]


def encode_command(source, target, bitrate):
    return ['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', str(source),
            '-c:a', 'libopus', '-b:a', bitrate, str(target)]


def probe_command(path):
    return ['ffprobe', '-v', 'error', '-select_streams', 'a:0',
            '-show_entries', 'stream=sample_rate,duration_ts,duration',
            '-of', 'json', str(path)]


def probe(path, run):
    """Sample rate and frame count of a rendered stem, for the manifest."""
    try:
        output = run(probe_command(path), capture_output=True, check=True).stdout
        stream = json.loads(output)['streams'][0]
    except (subprocess.CalledProcessError, ValueError, KeyError, IndexError, OSError):
        return None, None
    sample_rate = int(stream['sample_rate']) if stream.get('sample_rate') else None
    frames = None
    if stream.get('duration') and sample_rate:
        # Round: duration is decimal seconds, so truncating loses a frame.
        frames = round(float(stream['duration']) * sample_rate)
    return frames, sample_rate


def separate(track, model, bitrate, run, log=print, *, device="cpu", segment=4):
    """Separate one track into its stems directory. Returns the manifest."""
    target = stems_dir(track)
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        run(demucs_command(track, temp, model, device, segment), check=True)
        # demucs writes <out>/<model>/<track stem>/{vocals,no_vocals}.wav
        rendered = {p.stem: p for p in temp.rglob('*.wav')}
        if 'vocals' not in rendered or 'no_vocals' not in rendered:
            raise RuntimeError(f'{track.name}: demucs produced {sorted(rendered)}')

        staging = Path(str(target) + '.partial')
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        run(encode_command(rendered['vocals'], staging / VOCALS, bitrate), check=True)
        run(encode_command(rendered['no_vocals'], staging / INSTRUMENTAL, bitrate), check=True)

        frames, sample_rate = probe(staging / VOCALS, run)
        manifest = build_manifest(track, model, bitrate, frames, sample_rate)
        with open(staging / MANIFEST_NAME, 'w') as f:
            json.dump(manifest, f, indent=2)
            f.write('\n')

    # Swap in only once everything is written: a half-finished stems directory
    # beside a track is worse than none, because the device would try to play it.
    shutil.rmtree(target, ignore_errors=True)
    staging.rename(target)
    log(f'  {target.name}')
    return manifest


def main(argv=None, run=subprocess.run, log=print, which=shutil.which):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('paths', nargs='+', help='Audio files, or folders to walk')
    parser.add_argument('--model', default=DEFAULT_MODEL, help=f'demucs model (default {DEFAULT_MODEL})')
    parser.add_argument('--bitrate', default=DEFAULT_BITRATE, help=f'Opus bitrate (default {DEFAULT_BITRATE})')
    parser.add_argument('--device', choices=('auto', 'cpu', 'cuda', 'mps'), default='cpu',
                        help='Compute device (auto selects CUDA if available, otherwise CPU)')
    parser.add_argument('--segment', type=int, choices=range(1, 8), default=4,
                        help='Model segment seconds, 1–7 (default 4)')
    parser.add_argument('--report', type=Path, help='Write a JSON run summary after each track')
    parser.add_argument('--force', action='store_true', help='Redo tracks that already have current stems')
    parser.add_argument('--dry-run', action='store_true', help='List what would be separated')
    args = parser.parse_args(argv)

    missing = [p for p in args.paths if not Path(p).expanduser().exists()]
    if missing:
        log('Paths not found: ' + ', '.join(missing))
        return 2
    tracks = find_tracks(args.paths)
    if not tracks:
        log('No audio files found.')
        return 1

    todo = [t for t in tracks if args.force or not is_current(t, stems_dir(t))]
    log(f'{len(tracks)} track(s), {len(todo)} to separate'
        f'{" (forced)" if args.force else ""}.')
    if args.dry_run:
        for track in todo:
            log(f'  would separate {track}')
        return 0
    for tool in ('demucs', 'ffmpeg', 'ffprobe'):
        if todo and not which(tool):
            log(f'{tool} is not on PATH; activate the stems virtual environment.')
            return 2

    device = args.device
    if todo and device == 'auto':
        import torch
        device = 'cuda' if torch.cuda.is_available() else 'cpu'
    summary = {'device': device, 'model': args.model, 'total': len(tracks),
               'skipped': len(tracks) - len(todo), 'completed': [], 'failed': [],
               'interrupted': False}
    started = time.monotonic()

    def save_report():
        summary['elapsed_seconds'] = round(time.monotonic() - started, 1)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            temporary = args.report.with_name(args.report.name + '.partial')
            temporary.write_text(json.dumps(summary, indent=2) + '\n')
            temporary.replace(args.report)

    save_report()
    if todo:
        log(f'Using {device}; stems are written beside each original. Ctrl-C stops safely; rerun to resume.')
    for index, track in enumerate(todo, 1):
        log(f'[{index}/{len(todo)}] {track}')
        track_started = time.monotonic()
        try:
            separate(track, args.model, args.bitrate, run, log,
                     device=device, segment=args.segment)
            elapsed = time.monotonic() - track_started
            summary['completed'].append(str(track))
            log(f'  Ready ({elapsed / 60:.1f} min)')
        except KeyboardInterrupt:
            summary['interrupted'] = True
            save_report()
            log('Stopped. Completed stems are preserved; rerun the same command to resume.')
            return 130
        except (subprocess.CalledProcessError, RuntimeError, OSError) as error:
            summary['failed'].append({'path': str(track), 'error': str(error)})
            log(f'  failed: {error}')
        save_report()
    failed = len(summary['failed'])
    log(f"Finished: {len(summary['completed'])} prepared, {summary['skipped']} already ready, "
        f"{failed} track(s) failed. Elapsed: {(time.monotonic() - started) / 60:.1f} min.")
    if args.report:
        log(f'Report: {args.report.resolve()}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
