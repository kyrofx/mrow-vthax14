# Docker build environments

`compose.yaml` (repo root) defines two build environments, both built from
`docker/Dockerfile`:

| Service  | Image               | Base          | Purpose                                                                                                 |
| -------- | ------------------- | ------------- | ------------------------------------------------------------------------------------------------------- |
| `pi`     | `bitedj-dev:pi`     | Debian trixie | The target device: Raspberry Pi 4B on Raspberry Pi OS 64-bit (trixie), compiled for its Cortex-A72 CPU. |
| `ubuntu` | `bitedj-dev:ubuntu` | Ubuntu 24.04  | Matches CI (`.github/workflows/tests.yml`).                                                              |

Both are arm64. On Apple Silicon they run natively; on x86 hosts the `pi`
image is emulated, which is very slow.

The source tree is bind-mounted at `/src`. Each environment keeps its build
tree in its own named volume (`build-pi`, `build-ubuntu`), mounted at
`/src/build`, so the two never mix. ccache is shared between them.

Files:

- `Dockerfile`: the image. `BASE_IMAGE` and `CPU_FLAGS` build args select the
  flavour.
- `configure.sh`: runs CMake with the same feature flags as CI. Extra
  arguments are passed through to `cmake`.
- `run-vnc.sh`: starts a virtual X display and a VNC server, then runs the
  built app.
- `stage.sh`: stages a stripped install tree in `install/` for deploying to
  the Pi.

Commands run from the repo root unless marked _(inside)_.

## Pi (Debian trixie)

```sh
docker compose build pi
docker compose run --rm pi                   # shell in /src
docker/configure.sh                          # (inside) first time only
cmake --build build -j6                      # (inside)
ctest --test-dir build --output-on-failure   # (inside)
```

Run the app and connect:

```sh
docker compose up pi-run
open vnc://localhost:5900                    # password: bitedj
```

## Ubuntu (CI)

```sh
docker compose build ubuntu
docker compose run --rm ubuntu
docker/configure.sh                          # (inside) first time only
cmake --build build -j6                      # (inside)
ctest --test-dir build --output-on-failure   # (inside)
```

Run the app and connect:

```sh
docker compose up ubuntu-run
open vnc://localhost:5901                    # note: port 5901
```

## Notes

- **Memory:** with Docker's default memory limit, don't build both flavours at
  once, and lower `-j` if a compile dies with `Killed` / signal 9.
- **Display size:** `SCREEN_SIZE=800x480 docker compose up pi-run` sizes the
  virtual display to match a panel (default `1280x720`).
- **What the VNC run is good for:** rough UI checks only. It uses software
  OpenGL and has no audio device. The Devices page will be empty, so startup
  lands on Settings → Devices. Audio latency, touch, GPU performance and USB
  drive handling need testing on the device.
- **Settings:** the app's `~/.mixxx` persists in the `settings-pi` /
  `settings-ubuntu` volumes.
- **Cleanup:** `docker compose down -v` removes all build trees, ccache and
  settings.

## Deploying to the Pi

This assumes Mixxx already runs on the Pi, so its shared libraries are
installed; only the binary needs replacing. The `pi` build links against the
same Debian release as Raspberry Pi OS trixie. The build tree lives in a
Docker volume, so strip a copy into the host checkout (`install/` is
gitignored) and copy it over (replace `pi@raspberrypi.local` with your
device):

Run both from the repo root on the host, not inside the container. `install/`
is bind-mounted, so the stripped binary lands in the host checkout:

```sh
docker compose run --rm pi bash -c 'mkdir -p install && strip -o install/mixxx build/mixxx'
scp install/mixxx pi@raspberrypi.local:bitedj/bin/mixxx
```

The binary loads skins and mappings from `../share/mixxx` relative to itself,
so where it goes depends on how Mixxx is installed on the Pi
(`readlink -f $(which mixxx)` shows the path):

- **Built from this repo** (e.g. `/usr/local/bin/mixxx` next to
  `/usr/local/share/mixxx`): replace that binary; it uses the resources
  installed beside it.
- **Debian's `mixxx` package** (`/usr/bin/mixxx`): don't overwrite it. apt
  owns it, and `/usr/share/mixxx` holds upstream Mixxx's resources without
  the BiteDJ skin. Put the binary elsewhere (e.g. `~/bitedj/bin/`) and pass
  `--resource-path` pointing at this repo's `res/`.

The binary alone is not enough when:

- **`res/` changed:** skin and mapping changes live in `res/`, not in the
  binary. `docker/stage.sh` stages a full install tree
  (`install/bin/mixxx` plus `install/share/mixxx/`); sync
  `install/share/mixxx/` to the Pi as well.
- **A new library dependency was added:** on the Pi,
  `ldd ~/bitedj/bin/mixxx | grep "not found"` should print nothing.
  `docker/stage.sh` also writes `install/apt-packages.txt` listing the
  packages the binary needs.
