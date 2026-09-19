#!/bin/bash
# Runs BiteDJ on a virtual X display and serves it over VNC on port 5900.
# Connect from macOS with: open vnc://localhost:5900
set -euo pipefail

: "${SCREEN_SIZE:=1280x720}"
: "${VNC_PASSWORD:=bitedj}"
BINARY=/src/build/mixxx

if [ ! -x "$BINARY" ]; then
    echo "$BINARY not found; build it first (see README)." >&2
    exit 1
fi

# The MROW DJ harness, when this checkout sits inside the MROW repository
# (compose mounts it at /harness). Without it the Assist tab just reports the
# assistant offline, which is a valid state to look at too.
HARNESS=/harness/src/harness.py
if [ -x "$(command -v python3)" ] && [ -f "$HARNESS" ]; then
    python3 "$HARNESS" --host 127.0.0.1 --port 8765 \
        --database /tmp/mrow-harness.sqlite3 >/tmp/harness.log 2>&1 &
    echo "harness: http://127.0.0.1:8765 (log /tmp/harness.log)"
fi

export DISPLAY=:99
export QT_QPA_PLATFORM=xcb
export LIBGL_ALWAYS_SOFTWARE=1

Xvfb "$DISPLAY" -screen 0 "${SCREEN_SIZE}x24" -nolisten tcp &
for _ in $(seq 50); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done

x11vnc -display "$DISPLAY" -rfbport 5900 -passwd "$VNC_PASSWORD" \
    -forever -shared -quiet &

exec "$BINARY" --resource-path /src/res/ --full-screen "$@"
