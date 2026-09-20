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

Build from the full MROW checkout (`Mixxx/bitedj`) so CMake can embed
`../../harness/src`. For a standalone fork checkout, add
`-DBITEDJ_AGENT_SOURCE_DIR=/absolute/path/to/mrow/harness/src` below.
The deployed app needs Python 3.9+ but no source checkout or HTTP service.
Optional [OpenRouter and ElevenLabs key injection](#inject-openrouter-and-elevenlabs-keys)
uses a private install-time file. Keys are not CMake variables or embedded in the binary.

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

## Inject OpenRouter and ElevenLabs keys

Provision both keys during build/deploy so the embedded agent loads them at each
startup. The credential file stays outside the repository and build image.
Run the following commands from the **MROW repository root**.

### Build and deploy from a workstation

Create a private config using hidden prompts:

```bash
python3 RPI/scripts/agent-config.py
```

Enter the OpenRouter key, next-song model, setlist model, and ElevenLabs key.
Both key prompts hide input. Model IDs default to `openrouter/auto`. The ElevenLabs
prompt is optional: leaving it blank creates an OpenRouter-only config, compatible
with previous installs.

The default output is `~/.config/mrow-build/agent.json`. Its fields are:

```json
{
  "api_key": "<OpenRouter key>",
  "next_model": "openrouter/auto",
  "plan_model": "openrouter/auto",
  "elevenlabs_api_key": "<ElevenLabs key>"
}
```

Use the prompt to create the real file; the JSON above only documents the schema.
If `elevenlabs_api_key` is supplied, it must be a nonempty key. An invalid field
rejects the entire config before either provider is configured.

Validate and deploy, replacing `user@pi-host` with your Pi's SSH destination:

```bash
python3 RPI/scripts/agent-config.py --validate "$HOME/.config/mrow-build/agent.json"
RPI/scripts/deploy.sh --host user@pi-host \
  --agent-config "$HOME/.config/mrow-build/agent.json"
```

The deploy script builds Mixxx and transfers the config through SSH standard input
into `~/.config/mrow/agent.json` for the destination user. No key is placed in
compiler arguments, deployment staging or container layers. The file has mode
`600`, its directory `700`. Run the appliance as this same user.

Add `--no-build` to deploy an already-built binary that includes this feature.
Add `--dry-run` to validate and preview deployment without transferring the keys.
Without `--agent-config`, deployment preserves the Pi's existing credential file.
Deployment does not restart the running app by default: restart BiteDJ when audio
can stop, or explicitly add `--restart` to the deployment command.

### When building directly on the Pi

After installing Mixxx, run this from the MROW repository root as the appliance
user, **without sudo**:

```bash
python3 RPI/scripts/agent-config.py --output "$HOME/.config/mrow/agent.json"
python3 RPI/scripts/agent-config.py --validate "$HOME/.config/mrow/agent.json"
```

Both keys load when BiteDJ next starts. `cmake --install` alone does not install
credentials. A standalone fork checkout needs the provisioning script from the
full MROW checkout for this step.

### Replace an existing config

The interactive command refuses to overwrite files. To add ElevenLabs to an
existing OpenRouter setup, create a new file and enter both keys and your desired
models again:

```bash
python3 RPI/scripts/agent-config.py --output "$HOME/.config/mrow-build/agent-next.json"
RPI/scripts/deploy.sh --host user@pi-host \
  --agent-config "$HOME/.config/mrow-build/agent-next.json"
```

Deployment replaces the destination config atomically. For a direct Pi install,
prepare and validate a sibling file, then replace the config as the appliance user:

```bash
python3 RPI/scripts/agent-config.py --output "$HOME/.config/mrow/agent-next.json"
python3 RPI/scripts/agent-config.py --validate "$HOME/.config/mrow/agent-next.json"
mv "$HOME/.config/mrow/agent-next.json" "$HOME/.config/mrow/agent.json"
```

Restart BiteDJ to load the replacement. Runtime key changes and **Disconnect**
affect only the current process; provisioned keys reload after restart. To remove
a provider permanently, update the private file (omit `elevenlabs_api_key` to
remove ElevenLabs), or remove the file to disable all provisioned credentials.

### Verify startup

After restarting, **Assist → Models** should report provisioned OpenRouter
settings. **Assist → Generate song** should report a configured key with
provisioned keys reloading on restart. Neither screen displays the stored keys.
Configuration loading does not verify provider authentication. To verify music
generation, rate a played song and press **Generate & download**; expect a
completed job and an MP3 in `generated/` beside the harness database. This is a
paid ElevenLabs request. Import and analyze the downloaded MP3 in Mixxx.

If provisioning is rejected, run the validation command as the appliance user.
Check ownership, mode `600`, and the field names; symlinks and files readable by
other users are rejected. The file is protected by permissions, not encrypted.
Never commit it or include it in shared build artifacts or logs.

## Progress checks

```bash
tail -f ~/bitedj-build.log
find ~/bitedj/build/CMakeFiles/mixxx-lib.dir -name '*.o' | wc -l   # of 713
grep '=== ATTEMPT' ~/bitedj-build.log                              # OOM retries
```

Note the `AUTOMOC` stage for `mixxx-lib` sits at "10%" for roughly 25 minutes while it
generates ~900 `moc_*.cpp` files. It is single-threaded and looks hung. It is not.
