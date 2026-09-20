#!/bin/bash
# Prepare a locally mounted music library for BiteDJ. Originals stay untouched.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/bitedj-stems"
VENV="$CACHE/venv"

if [ "$#" -eq 0 ]; then
    echo 'Usage: prepare-library.sh /path/to/music [--dry-run] [--device auto|cpu|cuda|mps] [--force]'
    echo 'Example: prepare-library.sh /Volumes/MUSIC/Contents'
    echo 'First run installs Demucs into a private environment. Completed tracks are skipped.'
    exit 2
fi

# Help and previews must not install packages or download model weights.
for arg in "$@"; do
    case "$arg" in
        --help|-h|--dry-run) exec python3 "$HERE/separate-stems.py" "$@" ;;
    esac
done
for tool in ffmpeg ffprobe; do
    if ! command -v "$tool" >/dev/null; then
        echo "$tool is required. macOS: brew install ffmpeg; Debian/Ubuntu: sudo apt install ffmpeg" >&2
        exit 2
    fi
done
if [ ! -x "$VENV/bin/python" ]; then
    PYTHON=
    for candidate in python3.13 python3.12 python3.11 python3.10 python3; do
        if command -v "$candidate" >/dev/null &&
            "$candidate" -c 'import sys; sys.exit(not ((3,10) <= sys.version_info[:2] <= (3,13)))'; then
            PYTHON="$candidate"
            break
        fi
    done
    if [ -z "$PYTHON" ]; then
        echo 'Python 3.10–3.13 is required. On macOS: brew install python@3.13' >&2
        exit 2
    fi
    "$PYTHON" -m venv "$VENV"
fi
if [ ! -f "$VENV/.bitedj-ready-v1" ]; then
    "$VENV/bin/python" -m pip install 'torch==2.8.0' 'torchaudio==2.8.0' \
        'demucs==4.0.1' soundfile
    "$VENV/bin/python" -c 'import torch, torchaudio, demucs, soundfile'
    touch "$VENV/.bitedj-ready-v1"
fi
export PATH="$VENV/bin:$PATH"
export PYTHONUNBUFFERED=1
# MPS is opt-in: unsupported operations may fall back to CPU.
export PYTORCH_ENABLE_MPS_FALLBACK=1
REPORT="$CACHE/run-$(date +%Y%m%d-%H%M%S)-$$.json"
COMMAND=("$VENV/bin/python" "$HERE/separate-stems.py" --device auto --report "$REPORT" "$@")
# Keep a Mac awake during a long library run; release when the command exits.
if command -v caffeinate >/dev/null; then
    exec caffeinate -i "${COMMAND[@]}"
fi
exec "${COMMAND[@]}"
