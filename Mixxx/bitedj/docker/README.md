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
- **Settings:** the app's `~/.bitedj` persists in the `settings-pi` /
  `settings-ubuntu` volumes.
- **Cleanup:** `docker compose down -v` removes all build trees, ccache and
  settings.

## Deploying to the Pi

The `pi` build is a native arm64 build against the same Debian release as
Raspberry Pi OS trixie, so the binary runs on the Pi as long as its shared
libraries are installed there. It has no RPATH, and bundled libraries such as
libdjinterop are linked statically, so everything it needs comes from system
packages. It is not enough to copy just the binary:

1. **Copy `res/` too:** the binary needs skins, controller mappings and other
   resources from it. Run `mixxx --resource-path /path/to/res/`.
2. **Install runtime libraries:** the non-`-dev` counterparts of the packages
   in `tools/debian_buildenv.sh`. On the Pi, `ldd ./mixxx | grep "not found"`
   lists what's missing.
3. **Install Qt plugins:** `ldd` does not show them. The app needs
   `libqt6sql6-sqlite` and `qt6-svg-plugins`, and a platform plugin. The trixie
   desktop runs Wayland (labwc), so install `qt6-wayland` or run with
   `QT_QPA_PLATFORM=xcb` (XWayland).
4. **Allow real-time priority:** add these lines to a file in
   `/etc/security/limits.d/`, and put the user in the `audio` group:
   ```
   @audio - rtprio 95
   @audio - memlock unlimited
   ```
5. **Strip the binary:** the RelWithDebInfo build is ~490 MB. `strip` a copy
   before deploying.

A `.deb` built with the existing CPack packaging (`packaging/`) would take care
of steps 1–3.
