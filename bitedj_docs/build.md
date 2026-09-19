# Building BiteDJ on the Pi

Building on the Pi itself, not cross-compiling. It takes hours but avoids an entire
class of toolchain and sysroot problems.

**Reference build: 2 h 20 m** (12:22 → 14:42), single attempt, zero OOM kills, swap
never touched, peak 66 °C with no throttling. 713 translation units for `mixxx-lib`.

## Prerequisites

Dependencies come from upstream Mixxx's inherited `tools/debian_buildenv.sh`, which is
intact and Qt6-based. Deckshark's own OS is Arch-based and the repo only ships an Arch
build-env script; **the Debian path is the one to use here.**

```bash
cd ~/bitedj
tools/debian_buildenv.sh setup
```

## Configure

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DBUILD_BENCH=OFF \
  -DENGINEPRIME=OFF \
  -DBROADCAST=OFF \
  -DQTKEYCHAIN=OFF \
  -DBUILD_LOW_MEMORY=ON
```

### Why each flag

| Flag | Reason |
|---|---|
| `BUILD_TESTING=OFF` | Defaults **ON** whenever GTest is found, and the buildenv script installs `libgtest-dev`. Building `mixxx-test` means ~180 extra TUs all linking `mixxx-lib`. Biggest single saving. |
| `BUILD_BENCH=OFF` | **Required** alongside the above. Auto-enables from `libbenchmark-dev`, and CMake hard-errors with "Benchmark needs Unittests" if bench is on while testing is off. |
| `ENGINEPRIME=OFF` | Skips fetching and building libdjinterop as an ExternalProject. Denon Engine Prime export is useless on an appliance. |
| `BROADCAST=OFF` | Drops Shoutcast/libshout-idjc sources. |
| `QTKEYCHAIN=OFF` | Follows from `BROADCAST=OFF` — the keychain only stores broadcast credentials. |
| `BUILD_LOW_MEMORY=ON` | Disables `-pipe`, so the compiler spills intermediates to disk instead of holding them in RAM. Exists for exactly this situation. |

### Deliberately left on

- **`KEYFINDER`** — `libkeyfinder` is not in the apt deps, so CMake fetches and builds it
  (plus FFTW) during the build. **The Pi must be online for this part.** Turning it off
  costs musical key detection, which the UI exposes (key notation, harmonic Match).
  Once `build/libdjinterop-2.2.8/src/libkeyfinder-stamp/libkeyfinder-done` exists, the
  fetch is complete and the rest of the build is fully offline.
- **`VINYLCONTROL`** — the skin may reference vinyl controls. Note the fork's vinyl/CDJ
  jog mode and vinyl brake are *separate features* from timecode vinyl control.

Confirmed at configure time: CMake 3.31.6, optimization level `portable`, ccache on,
**mold** selected as the linker, FFMPEG found.

## Compile

Use `~/build-bitedj.sh`, which wraps the build with OOM recovery:

```bash
~/build-bitedj.sh          # logs to ~/bitedj-build.log
```

It retries on OOM (ccache is on and make resumes, so nothing is lost), drops to `-j1`
after three attempts, and — importantly — **stops on a genuine compile error** instead
of looping. It distinguishes the two by grepping the tail for OOM signatures.

Run it detached. An SSH drop otherwise kills the build:

```bash
setsid nohup ~/build-bitedj.sh > ~/build-driver.log 2>&1 < /dev/null &
```

`tmux` is not installed; `setsid` does the same job here.

### `-j2`, not `-j4`

Four concurrent `g++` processes on `-O3` Qt6 C++ will not fit in 4 GB and the OOM killer
takes them. `-j2` on this box ran at load 2.0 with ~1.7 GB used and never touched swap.

Swap is 2 GB of **zram** (no `dphys-swapfile`), so raising it trades RAM for RAM. With
`-j2` it was never needed.

### Thermals

Needs a fan or heatsink. The reference build peaked at 66 °C with `throttled=0x0`.
Check with `vcgencmd measure_temp` and `vcgencmd get_throttled` — anything non-zero
from the latter means you lost clock speed and the build took longer than it needed to.

### On an OOM kill

Rerun the same command; ccache is on and make resumes. Drop to `-j1` to push past
whichever heavy TU is blowing up, then go back to `-j2`. Last resort is `-DOPTIMIZE=off`,
which compiles much lighter but produces a binary too slow for real-time audio — use it
only to prove the build completes, never to ship.

Check `df -h` first; the build tree wants several GB. The reference build left 19 GB free
on a 29 GB card.

## Install

```bash
sudo cmake --install build
```

That gives you `/usr/local/bin/mixxx` plus `/usr/local/share/mixxx/{skins,controllers,effects,translations,udev}`.

Two things `cmake --install` does *not* do, both of which we did by hand:

```bash
# USB controller access without root. cmake only puts this in the datadir.
sudo cp /usr/local/share/mixxx/udev/rules.d/mixxx-usb-uaccess.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules

# Convenience only -- everything on disk is really called mixxx.
sudo ln -sfn /usr/local/bin/mixxx /usr/local/bin/bitedj
```

## Progress checks

```bash
tail -f ~/bitedj-build.log
find ~/bitedj/build/CMakeFiles/mixxx-lib.dir -name '*.o' | wc -l   # of 713
grep '=== ATTEMPT' ~/bitedj-build.log                              # OOM retries
```

Note the `AUTOMOC` stage for `mixxx-lib` sits at "10%" for roughly 25 minutes while it
generates ~900 `moc_*.cpp` files. It is single-threaded and looks hung. It is not.
