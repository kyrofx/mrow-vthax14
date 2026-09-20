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

# The built-in agent starts with Mixxx; no web server or source mount is needed.

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
