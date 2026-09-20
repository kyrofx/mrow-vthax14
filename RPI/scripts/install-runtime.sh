#!/bin/bash
# Apply the BiteDJ appliance runtime configuration to a Raspberry Pi.
#
# This sets up everything AROUND the application: real-time audio scheduling,
# CPU governor, kernel preemption, the Wayland session, USB automount, polkit,
# and console autologin. It does NOT build BiteDJ -- see build-bitedj.sh for
# that, and run it first (or install a prebuilt binary to /usr/local/bin/mixxx).
#
# Safe to re-run; every step is idempotent. It never reboots on its own.
#
# Usage:  sudo -v && ./install-runtime.sh          # run as the appliance user
#
# See ../bitedj_docs/runtime.md for what each piece does and why.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="$HERE/../config"
USER_NAME="${SUDO_USER:-$USER}"
USER_HOME="$(getent passwd "$USER_NAME" | cut -d: -f6)"

if [ "$(id -u)" -eq 0 ] && [ -z "${SUDO_USER:-}" ]; then
    echo "Run this as the appliance user (it will sudo as needed), not as root." >&2
    exit 1
fi

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }

say "Installing for user '$USER_NAME' (home: $USER_HOME)"

# ---------------------------------------------------------------- packages ---
say "Installing packages"
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
# qt6-wayland is NOT optional: without it Qt has no `wayland` platform plugin
# and BiteDJ aborts on every launch.
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    sway swayidle waybar udiskie udisks2 \
    qt6-wayland \
    xdg-desktop-portal xdg-desktop-portal-wlr xdg-desktop-portal-gtk \
    pipewire pipewire-pulse wireplumber \
    policykit-1 mate-polkit \
    foot pcmanfm \
    lisgd wvkbd uhubctl \
    python3 python3-gi gir1.2-gtk-3.0 gir1.2-gtklayershell-0.1 python3-libevdev \
    bluez pipewire-alsa libspa-0.2-bluetooth \
    grim mesa-utils evtest evemu-tools \
    python3-libgpiod python3-rtmidi python3-smbus2 i2c-tools

# python3-libgpiod (v2) and python3-rtmidi are for the GPIO crowd buttons; the
# DJ harness itself needs nothing beyond the Python standard library.

# lisgd + wvkbd are what make the box recoverable without a keyboard: lisgd
# turns touchscreen swipes into commands (sway's own bindgesture only sees
# libinput *gesture* events, which touchscreens do not emit), and wvkbd is the
# on-screen keyboard. Without them the appliance has no escape hatch.

# ------------------------------------------------------------ audio / RT -----
say "Real-time audio limits"
sudo install -m 644 "$CONFIG/etc/security/limits.d/99-bitedj-audio.conf" \
    /etc/security/limits.d/99-bitedj-audio.conf
if ! id -nG "$USER_NAME" | tr ' ' '\n' | grep -qx audio; then
    note "Adding $USER_NAME to the 'audio' group"
    sudo usermod -aG audio "$USER_NAME"
    note "Group change needs a re-login to take effect."
fi

say "Bluetooth: clear the persisted rfkill soft-block"
# systemd-rfkill restores a saved rfkill state at every boot. If Bluetooth was
# ever saved as blocked, it comes up dead and every bluetoothctl command fails
# with org.bluez.Error.NotReady -- an error that never mentions rfkill.
sudo install -m 644 "$CONFIG/etc/systemd/system/bluetooth-unblock.service" \
    /etc/systemd/system/bluetooth-unblock.service
sudo systemctl daemon-reload
sudo systemctl enable --now bluetooth-unblock.service

say "CPU governor -> performance"
sudo install -m 644 "$CONFIG/etc/systemd/system/cpu-performance.service" \
    /etc/systemd/system/cpu-performance.service
sudo systemctl daemon-reload
sudo systemctl enable --now cpu-performance.service

# ------------------------------------------------------------ kernel args ----
say "Kernel preemption"
CMDLINE=/boot/firmware/cmdline.txt
if grep -q 'preempt=full' "$CMDLINE"; then
    note "preempt=full already present"
else
    sudo cp -n "$CMDLINE" "$CMDLINE.bak-prebitedj"
    # cmdline.txt MUST remain a single line; a stray newline makes the Pi unbootable.
    sudo sed -i '1s/$/ preempt=full/' "$CMDLINE"
    note "Added preempt=full (backup at $CMDLINE.bak-prebitedj)"
fi

# ----------------------------------------------------------------- polkit ----
say "Polkit rules (udisks2 + power-off without a password prompt)"
sudo install -m 644 "$CONFIG/etc/polkit-1/rules.d/50-bitedj.rules" \
    /etc/polkit-1/rules.d/50-bitedj.rules
sudo systemctl restart polkit

# ------------------------------------------------------------------ udev -----
say "USB controller udev rules"
if [ -f /usr/local/share/mixxx/udev/rules.d/mixxx-usb-uaccess.rules ]; then
    # `cmake --install` only puts these in the datadir, not where udev reads them.
    sudo cp /usr/local/share/mixxx/udev/rules.d/mixxx-usb-uaccess.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules
else
    note "BiteDJ not installed yet -- re-run this script after 'cmake --install'."
fi

# ------------------------------------------------------- user-level config ---
say "Session configuration"
install -d "$USER_HOME/.config/sway" "$USER_HOME/.config/waybar" \
           "$USER_HOME/.config/wireplumber/wireplumber.conf.d" \
           "$USER_HOME/.local/bin" "$USER_HOME/.local/share"

install -m 644 "$CONFIG/home/.config/sway/config"        "$USER_HOME/.config/sway/config"
install -m 644 "$CONFIG/home/.config/waybar/style.css"   "$USER_HOME/.config/waybar/style.css"
install -m 644 "$CONFIG/home/.config/waybar/config.jsonc" "$USER_HOME/.config/waybar/config.jsonc"
# The waybar custom module is referenced by absolute path.
sed -i "s#/home/flx4/#$USER_HOME/#g" "$USER_HOME/.config/waybar/config.jsonc"

install -m 644 "$CONFIG/home/.config/wireplumber/wireplumber.conf.d/50-bitedj-reserve-controller.conf" \
    "$USER_HOME/.config/wireplumber/wireplumber.conf.d/50-bitedj-reserve-controller.conf"

for s in bitedj-session waybar-usb add-wifi bitedj-bt bitedj-bt-ui \
         bitedj-screen bitedj-gestures bitedj-osk bitedj-power \
         bitedj-cursor-park bitedj-volume bitedj-usb-recover; do
    install -m 755 "$HERE/$s" "$USER_HOME/.local/bin/$s"
done

# ------------------------------------------------------------- MROW extras ---
say "Built-in agent and crowd buttons"
install -d "$USER_HOME/.local/share/mrow" "$USER_HOME/.config/mrow" \
           "$USER_HOME/.config/systemd/user" "$USER_HOME/.mixxx/harness"
# Copies rather than links: the services must survive this checkout moving.
rm -rf "$USER_HOME/.local/share/mrow/buttons"
cp -r "$HERE/../src/buttons" "$USER_HOME/.local/share/mrow/buttons"
# Never overwrite a working wiring or an API key that is already in place.
[ -f "$USER_HOME/.config/mrow/buttons.toml" ] ||
    install -m 644 "$CONFIG/home/.config/mrow/buttons.toml" "$USER_HOME/.config/mrow/buttons.toml"
# Retire the old web sidecar without deleting history or credentials.
systemctl --user disable --now mrow-harness.service 2>/dev/null || true
install -m 644 "$CONFIG/home/.config/systemd/user/mrow-buttons.service" \
    "$USER_HOME/.config/systemd/user/mrow-buttons.service"
systemctl --user daemon-reload 2>/dev/null || true
systemctl --user enable mrow-buttons.service 2>/dev/null ||
    note "Enable the buttons after the next login: systemctl --user enable --now mrow-buttons"

# The GPIO character device is owned by the 'gpio' group on Raspberry Pi OS.
if ! id -nG "$USER_NAME" | tr ' ' '\n' | grep -qx gpio; then
    note "Adding $USER_NAME to the 'gpio' group (needed for the crowd buttons)"
    sudo usermod -aG gpio "$USER_NAME"
fi

# Qwiic Twist shares the existing display bus; do not change bus speed/overlays.
if ! id -nG "$USER_NAME" | tr ' ' '\n' | grep -qx i2c; then
    sudo usermod -aG i2c "$USER_NAME"
fi

# lisgd reads the touchscreen's evdev node directly.
if ! id -nG "$USER_NAME" | tr ' ' '\n' | grep -qx input; then
    note "Adding $USER_NAME to the 'input' group (needed for touch gestures)"
    sudo usermod -aG input "$USER_NAME"
fi

# --------------------------------------------------------------- autologin ---
say "Console autologin -> sway"
sudo raspi-config nonint do_boot_behaviour B2
sudo systemctl disable lightdm 2>/dev/null || true

if grep -q 'BiteDJ appliance session' "$USER_HOME/.profile" 2>/dev/null; then
    note "~/.profile already launches the session"
else
    printf '\n' >> "$USER_HOME/.profile"
    cat "$CONFIG/home/profile-snippet.sh" >> "$USER_HOME/.profile"
    note "Appended the session launcher to ~/.profile"
fi

# ------------------------------------------------------------------ done -----
say "Done"
cat <<EOF

Reboot to bring the appliance up:   sudo systemctl reboot

Not done automatically, because both are choices rather than defaults:

  * The cloud model. The agent starts inside Mixxx automatically. Use Assist >
    Models for a runtime Google Cloud Gemini key, or provision ~/.config/mrow/agent.json
    with scripts/agent-config.py during build/deploy. Local scoring needs no key.

  * Audio output. BiteDJ needs its device set in SETTINGS > AUDIO. If you use a
    DJ controller, edit the WirePlumber rule first so it names YOUR controller --
    the shipped one matches a DDJ-FLX4. Otherwise PipeWire holds the card and the
    controller stays silent. See ../bitedj_docs/audio.md
    A known-good config for a DDJ-FLX4 is at config/home/.mixxx/soundconfig.xml

  * Passwordless sudo. config/etc/sudoers.d/010-bitedj-nopasswd is convenience
    for remote administration, not required. To use it:
        sed "s/BITEDJ_USER/\$USER/" config/etc/sudoers.d/010-bitedj-nopasswd \\
          | sudo tee /etc/sudoers.d/010-bitedj-nopasswd >/dev/null
        sudo chmod 440 /etc/sudoers.d/010-bitedj-nopasswd
        sudo visudo -c        # ALWAYS validate; a bad file locks you out of sudo

Getting out of the appliance once it boots:

  * Three fingers swiped DOWN  -> the debug desktop (terminal, on-screen
    keyboard, file manager, raspi-config).
  * Three fingers swiped UP    -> back to BiteDJ.
  * Three fingers swiped LEFT  -> the Bluetooth speaker picker, drawn over
    BiteDJ without disturbing it.
  * With a keyboard: Super+Escape toggles, Super+Return opens a terminal.

  Switching does not interrupt playback. See ../bitedj_docs/desktop-access.md

Verify after reboot:  see ../bitedj_docs/runtime.md ("Verifying a boot")
EOF
