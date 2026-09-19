#!/bin/bash
# Stages a stripped, relocatable install tree at /src/install (the `install/`
# directory in the host checkout) for copying to the device. The binary finds
# its resources in ../share/mixxx, so the tree can live anywhere.
#
# Also writes install/apt-packages.txt: the Debian packages providing the
# libraries the binary links against, plus Qt plugins that ldd cannot see.
# Run inside the `pi` container so the package names match the device.
set -euo pipefail

PREFIX=/src/install
rm -rf "$PREFIX"
cmake --install /src/build --prefix "$PREFIX" --strip >/dev/null

BINARY="$PREFIX/bin/mixxx"
{
    # Direct dependencies only; apt resolves the rest.
    readelf -d "$BINARY" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | while read -r lib; do
        path=$(ldd "$BINARY" | awk -v lib="$lib" '$1 == lib { print $3 }')
        dpkg -S "$(readlink -f "$path")" | cut -d: -f1
    done
    # Loaded at runtime: SQLite driver, SVG icons, Wayland (trixie desktop).
    echo libqt6sql6-sqlite
    echo qt6-svg-plugins
    echo qt6-wayland
} | sort -u > "$PREFIX/apt-packages.txt"

du -sh "$PREFIX"
echo "Packages needed on the device: $(wc -l < "$PREFIX/apt-packages.txt") (see apt-packages.txt)"
