# Cross-compiling on a Mac

Building on the Pi takes **2h20m** from cold. Building in the project's Debian
trixie arm64 container on an Apple Silicon Mac takes minutes, and
`RPI/scripts/deploy.sh` ships the result over ssh.

Worth knowing before you invest in this: an **incremental** rebuild on the Pi is
about 50 seconds (one TU, re-archive, relink with mold). The 2h20m figure is the
cold build only. Cross-compiling wins for a first build, a clean rebuild, or
anything touching a widely-included header — not for a one-line change you can
iterate on over ssh.

The container itself is documented in
[`Mixxx/bitedj/docker/README.md`](../../Mixxx/bitedj/docker/README.md). This page
covers the part that is not: getting a working Docker on macOS, and how the
deploy fits together.

## Why Apple Silicon matters

`compose.yaml` pins the `pi` service to `platform: linux/arm64`, because the
target is a Pi 4B. On an arm64 Mac that runs **natively**. On an x86 Mac it is
emulated through QEMU, which the fork's README describes as "very slow" — true
enough that it is not worth doing.

The image also sets `CPU_FLAGS=-mcpu=cortex-a72+nocrypto`. The `+nocrypto` is not
decoration: the Pi 4B's BCM2711 lacks the ARMv8 crypto extensions, and without it
the compiler may emit instructions that fault on the device.

## Host setup

```sh
brew install colima docker docker-compose rsync
```

`rsync` is not optional on macOS. The system ships **openrsync** at
`/usr/bin/rsync`, which reports itself as "rsync 2.6.9 compatible" and has
neither `--info=stats0` nor the rsync 3.4 argument protection that `deploy.sh`
relies on for remote paths. Without GNU rsync the deploy fails partway with an
rsync usage dump. Homebrew's copy takes precedence on `PATH`; confirm with
`rsync --version` (you want 3.x).

Homebrew installs Compose where the `docker` CLI does not look by default, so
`docker compose` (the v2 subcommand, which `deploy.sh` uses) will not resolve
until you point the CLI at it. Add to `~/.docker/config.json`:

```json
{ "cliPluginsExtraDirs": ["/opt/homebrew/lib/docker/cli-plugins"] }
```

Then start the VM. **Size it explicitly** — Colima's defaults are 2 CPU / 2 GB,
and this build will thrash or get OOM-killed in that:

```sh
colima start --cpu 8 --memory 16 --disk 100 --vm-type vz --mount-type virtiofs
```

`vz` is Apple's Virtualization.framework and `virtiofs` is its fast file sharing;
both need macOS 13+. Check it came up the way you asked:

```sh
colima status
docker info --format 'arch={{.Architecture}} cpus={{.NCPU}} mem={{.MemTotal}}'
docker compose version
```

`arch` must be `aarch64`. If it says `x86_64` you are about to emulate.

Colima does not start at login unless you ask it to (`brew services start
colima`). A deploy that fails with "cannot connect to the Docker daemon" usually
just means the VM is not running.

## Build

From `Mixxx/bitedj/`:

```sh
docker compose build pi                       # once, ~5 min
docker compose run --rm pi docker/configure.sh  # once
docker compose run --rm pi cmake --build build -j6
```

The build tree lives in a named volume (`build-pi`), not a bind mount —
deliberately, since named volumes are much faster than bind mounts on macOS.
ccache is shared in its own volume, so the second build of the same tree is far
quicker than the first.

`configure.sh` uses CI's feature flags, which are **not** the flags the Pi's own
build used: `BUILD_TESTING`, `BROADCAST` and `QTKEYCHAIN` are all ON here and
were OFF there. That means more test TUs and extra runtime libraries on the
device. `stage.sh` handles the latter by writing `install/apt-packages.txt`.

To match the appliance instead, pass overrides through — `configure.sh` forwards
its arguments to cmake:

```sh
docker compose run --rm pi docker/configure.sh \
    -DBUILD_TESTING=OFF -DBROADCAST=OFF -DQTKEYCHAIN=OFF -DENGINEPRIME=OFF
```

## Deploy

`RPI/scripts/deploy.sh` does the whole pipeline: build in the container, stage a
stripped install tree, copy over ssh, and move it into place on the device.

```sh
./RPI/scripts/deploy.sh                  # build + deploy everything to flx4
./RPI/scripts/deploy.sh --dry-run        # print what it would do
./RPI/scripts/deploy.sh --no-build       # deploy what is already built
./RPI/scripts/deploy.sh --only mixxx     # binary + resources only
./RPI/scripts/deploy.sh --user           # into ~/.local, no sudo at all
./RPI/scripts/deploy.sh --restart        # restart BiteDJ afterwards
```

Three properties of it worth relying on:

- **It does not restart BiteDJ by default.** The running set matters more than
  the new build; the device picks it up at the next restart.
- **The binary goes in by atomic rename.** A running BiteDJ keeps the inode it
  started from, so deploying mid-set cannot disturb it.
- **It runs `ldd | grep "not found"` after installing.** A missing library is the
  one failure that shows up only at launch, by which point the screen is black.
  If it reports anything, install it from `install/apt-packages.txt`.

It never clobbers device-local wiring. The agent is embedded in the Mixxx binary;
no separate harness service is installed. Optional `--agent-config FILE` provisions
Google Cloud Gemini credentials/models through a private SSH pipe; without that flag,
device credentials are untouched. See [provisioning](../../harness/README.md#optional-builddeploy-provisioning).

`/usr/local` is root-owned and ssh cannot write there, so everything is staged
through `~/.cache/mrow-deploy` on the device and moved into place with sudo. That
staging directory is also where the previously deployed binary can be found if a
deploy needs backing out.

## Keeping the two source trees in step

There are two checkouts of the fork:

| Where | Used for |
|---|---|
| `Mixxx/bitedj/` in this repo | What the container builds and what deploy ships |
| `~/bitedj` on the Pi | Native builds done on the device itself |

They are separate git checkouts and will drift if you patch one and not the
other. The repo copy is the one that matters for deploys; keep local patches
there and listed in [fork-patches.md](fork-patches.md).

```sh
# Confirm they agree (order-independent; GNU and BSD sort collate differently)
ssh flx4 "cd ~/bitedj && find src -name '*.cpp' -o -name '*.h' | xargs md5sum" | sort > /tmp/pi.md5
(cd Mixxx/bitedj && find src -name '*.cpp' -o -name '*.h' | xargs md5sum) | sort > /tmp/repo.md5
diff /tmp/pi.md5 /tmp/repo.md5 && echo "in step"
```

## Teardown

```sh
docker compose down -v     # build trees, ccache and app settings
colima stop                # or: colima delete, to reclaim the whole VM
```
