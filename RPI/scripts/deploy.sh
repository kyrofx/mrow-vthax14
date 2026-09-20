#!/bin/bash
# Deploy BiteDJ and the MROW extras from this workstation to the appliance.
#
# Builds in the project's Debian trixie arm64 container (same release as the
# device), stages a stripped install tree, and copies it over ssh:
#
#   binary + resources   /usr/local/bin/mixxx, /usr/local/share/mixxx
#   DJ agent             embedded in the Mixxx binary (private worker, no server)
#   crowd buttons        ~/.local/share/mrow/buttons
#   user services        ~/.config/systemd/user
#
# /usr/local is root-owned, and ssh cannot write there directly: everything is
# copied into a staging directory in the device's home first, then moved into
# place there with sudo (you are prompted for the password unless the device
# has passwordless sudo configured). --user installs under ~/.local instead and
# needs no sudo at all.
#
# Usage:
#   ./deploy.sh                     # build, then deploy everything to flx4
#   ./deploy.sh --host bitedj       # another device (any ssh alias or user@host)
#   ./deploy.sh --no-build          # deploy what is already built
#   ./deploy.sh --user              # install into ~/.local, no sudo
#   ./deploy.sh --only harness      # mixxx | harness | buttons | all (default)
#   ./deploy.sh --restart           # restart BiteDJ when done (interrupts audio)
#   ./deploy.sh --dry-run           # print what it would do
#   ./deploy.sh --agent-config FILE # provision an owner-only JSON key/model file
#
# Deploying does NOT restart BiteDJ by default: the running set matters more
# than the new build. The device picks it up at the next restart.

set -euo pipefail

HOST=flx4@10.0.0.190
BUILD=1
ONLY=all
RESTART=0
DRY_RUN=0
USER_INSTALL=0
AGENT_CONFIG=
# Set once the binary is in place, so the library check tests the right one.
INSTALLED_BIN=/usr/local/bin/mixxx
# Two spellings of the same directory. rsync 3.4 protects its arguments, so a
# remote path is never expanded by a shell there and "$HOME" would be taken as
# a literal directory name — rsync gets the path relative to the remote home,
# which it resolves itself. The ssh commands run in a shell and use $HOME.
STAGING_REL='.cache/mrow-deploy'
STAGING='$HOME/'"$STAGING_REL"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
FORK="$REPO/Mixxx/bitedj"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31mdeploy: %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs a value}"; shift 2 ;;
        --only) ONLY="${2:?--only needs a value}"; shift 2 ;;
        --no-build) BUILD=0; shift ;;
        --user) USER_INSTALL=1; shift ;;
        --restart) RESTART=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --agent-config) AGENT_CONFIG="${2:?--agent-config needs a path}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

case "$ONLY" in
    all|mixxx|harness|buttons) ;;
    *) die "--only must be all, mixxx, harness, or buttons" ;;
esac
if [ "$ONLY" = harness ]; then
    note "The agent is now embedded in Mixxx; rebuilding/deploying Mixxx."
    ONLY=mixxx
fi
if [ -n "$AGENT_CONFIG" ]; then
    python3 "$HERE/agent-config.py" --validate "$AGENT_CONFIG"
fi

# Everything that touches the device goes through these two, so --dry-run is
# honest about what would happen.
run_local() {
    if [ "$DRY_RUN" -eq 1 ]; then note "local: $*"; return 0; fi
    "$@"
}
run_remote() {
    if [ "$DRY_RUN" -eq 1 ]; then note "$HOST: $*"; return 0; fi
    # -t so sudo can prompt for a password, but only when there is a terminal
    # to prompt on; asking for one otherwise just prints a warning.
    if [ -t 0 ]; then
        ssh -t "$HOST" "$@"
    else
        ssh "$HOST" "$@"
    fi
}
copy_to_staging() {
    # copy_to_staging <local path> <path under the staging directory>
    local source="$1" target="$2"
    if [ "$DRY_RUN" -eq 1 ]; then note "rsync $source -> $HOST:$STAGING_REL/$target"; return 0; fi
    rsync -a --delete --info=stats0 "$source" "$HOST:$STAGING_REL/$target"
}

command -v rsync >/dev/null || die "rsync is required on this machine"
[ -d "$FORK" ] || die "cannot find the fork at $FORK"

say "Deploying to $HOST"
if [ "$DRY_RUN" -eq 1 ]; then
    note "Dry run: nothing is built, copied, or changed."
fi

if [ "$DRY_RUN" -eq 0 ] && ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" true 2>/dev/null; then
    die "cannot reach $HOST over ssh (key-based login required; see RPI/bitedj_docs/networking.md)"
fi

# ------------------------------------------------------------------ build ----
if [ "$ONLY" = all ] || [ "$ONLY" = mixxx ]; then
    if [ "$BUILD" -eq 1 ]; then
        say "Building in the trixie container"
        command -v docker >/dev/null || die "docker is required to build (or use --no-build)"
        run_local docker compose -f "$FORK/compose.yaml" --project-directory "$FORK" \
            run --rm -T pi cmake --build build -j6
    fi
    say "Staging the install tree"
    run_local docker compose -f "$FORK/compose.yaml" --project-directory "$FORK" \
        run --rm -T pi docker/stage.sh
    [ "$DRY_RUN" -eq 1 ] || [ -x "$FORK/install/bin/mixxx" ] ||
        die "no staged binary at $FORK/install/bin/mixxx"
fi

# ------------------------------------------------------------------ copy -----
say "Copying to $HOST:$STAGING"
run_remote "mkdir -p $STAGING"

if [ "$ONLY" = all ] || [ "$ONLY" = mixxx ]; then
    copy_to_staging "$FORK/install/bin/mixxx" mixxx
    copy_to_staging "$FORK/install/share/mixxx/" share-mixxx/
    copy_to_staging "$REPO/RPI/scripts/bitedj-session" bitedj-session
fi
if [ "$ONLY" = all ] || [ "$ONLY" = buttons ]; then
    copy_to_staging "$REPO/RPI/src/buttons/" buttons/
fi
if [ "$ONLY" = all ]; then
    copy_to_staging "$REPO/RPI/config/home/.config/systemd/user/" systemd-user/
    copy_to_staging "$REPO/RPI/config/home/.config/mrow/" mrow-config/
fi

# --------------------------------------------------------------- install -----
# Runs on the device. /usr/local needs root; everything else is the user's own.
if [ "$ONLY" = all ] || [ "$ONLY" = mixxx ]; then
    if [ "$USER_INSTALL" -eq 1 ]; then
        say "Installing BiteDJ into ~/.local on $HOST"
        # The binary finds its resources at ../share/mixxx, so this layout works
        # without touching /usr/local. bitedj-session must point at it too.
        run_remote "set -e
            mkdir -p \$HOME/.local/bin \$HOME/.local/share/mixxx
            install -m 755 $STAGING/mixxx \$HOME/.local/bin/mixxx.new
            mv \$HOME/.local/bin/mixxx.new \$HOME/.local/bin/mixxx
            rsync -a --delete $STAGING/share-mixxx/ \$HOME/.local/share/mixxx/
            ls -l \$HOME/.local/bin/mixxx"
        INSTALLED_BIN='$HOME/.local/bin/mixxx'
        note "bitedj-session will be pointed at ~/.local/bin/mixxx."
    else
        say "Installing BiteDJ into /usr/local on $HOST (sudo)"
        # Install beside the old binary and rename over it: a rename is atomic,
        # so the copy that is running keeps its own inode and is untouched.
        run_remote "set -e
            sudo install -m 755 $STAGING/mixxx /usr/local/bin/mixxx.new
            sudo mv /usr/local/bin/mixxx.new /usr/local/bin/mixxx
            [ -e /usr/local/bin/bitedj ] || sudo ln -s /usr/local/bin/mixxx /usr/local/bin/bitedj
            sudo mkdir -p /usr/local/share/mixxx
            sudo rsync -a --delete $STAGING/share-mixxx/ /usr/local/share/mixxx/
            ls -l /usr/local/bin/mixxx"
        INSTALLED_BIN=/usr/local/bin/mixxx
    fi
    # A missing library is the one failure that only shows at launch, by which
    # point the screen is black; catch it here instead.
    run_remote "ldd $INSTALLED_BIN | grep 'not found' &&
        echo 'MISSING LIBRARIES: install them with the list in install/apt-packages.txt' || true"
fi

if [ "$ONLY" = all ] || [ "$ONLY" = mixxx ]; then
    run_remote "python3 -c 'import sys; assert sys.version_info >= (3,9)'"
    run_remote "systemctl --user disable --now mrow-harness.service 2>/dev/null || true"
    run_remote "set -e
        mkdir -p \$HOME/.local/bin
        install -m 755 $STAGING/bitedj-session \$HOME/.local/bin/bitedj-session
        sed -i \"s#^BIN=.*#BIN=$INSTALLED_BIN#\" \$HOME/.local/bin/bitedj-session"
fi

# Stream through SSH, not arguments, environment, staging trees or build layers.
if [ -n "$AGENT_CONFIG" ]; then
    say "Provisioning private agent configuration"
    if [ "$DRY_RUN" -eq 1 ]; then
        note "Install protected agent.json over SSH (contents never displayed)."
    else
        copy_to_staging "$HERE/agent-config.py" agent-config.py
        python3 "$HERE/agent-config.py" --emit "$AGENT_CONFIG" |
            ssh "$HOST" "python3 $STAGING/agent-config.py --receive"
    fi
fi

if [ "$ONLY" = all ] || [ "$ONLY" = buttons ]; then
    say "Installing the crowd buttons on $HOST"
    # python3-libgpiod and python3-rtmidi are the daemon's only dependencies;
    # install-runtime.sh installs them, but a device provisioned before the
    # buttons existed will not have them.
    run_remote "set -e
        missing=
        python3 -c 'import gpiod' 2>/dev/null || missing=\"\$missing python3-libgpiod\"
        python3 -c 'import rtmidi' 2>/dev/null || missing=\"\$missing python3-rtmidi\"
        if [ -n \"\$missing\" ]; then
            echo \"Installing missing packages:\$missing\"
            sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \$missing
        fi"
    run_remote "set -e
        mkdir -p \$HOME/.local/share/mrow
        rsync -a --delete $STAGING/buttons/ \$HOME/.local/share/mrow/buttons/
        python3 \$HOME/.local/share/mrow/buttons/mrow_buttons.py --check || true"
fi

if [ "$ONLY" = all ]; then
    say "Services on $HOST"
    # Never overwrite the wiring or an API key already on the device.
    run_remote "set -e
        mkdir -p \$HOME/.config/systemd/user \$HOME/.config/mrow \$HOME/.local/bin
        install -m 644 $STAGING/systemd-user/*.service \$HOME/.config/systemd/user/
        [ -f \$HOME/.config/mrow/buttons.toml ] ||
            install -m 644 $STAGING/mrow-config/buttons.toml \$HOME/.config/mrow/buttons.toml
        systemctl --user daemon-reload
        systemctl --user enable mrow-buttons.service
        systemctl --user restart mrow-buttons.service || true
        systemctl --user --no-pager --lines=0 status mrow-buttons.service || true"
fi

# --------------------------------------------------------------- restart -----
if [ "$RESTART" -eq 1 ]; then
    say "Restarting BiteDJ on $HOST"
    # bitedj-session relaunches it; killing the process is the restart.
    run_remote "pkill -x mixxx || true"
else
    note "BiteDJ was not restarted; the new build starts with the next launch."
    note "To restart now (this stops any audio):  ssh $HOST 'pkill -x mixxx'"
fi

say "Done"
