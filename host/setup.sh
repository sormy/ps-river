#!/usr/bin/env bash
# Sets up a headless Debian 13 + NVIDIA host to stream the game with Sunshine: a virtual monitor
# (Xorg on :1) with a kiosk window manager that makes every window fill it, PipeWire audio without a login, uinput access for virtual gamepads, and Sunshine
# with an app for every unpacked game in GAMES (named after the game; run again after adding
# one), streamed at the client's resolution. Idempotent; run as the streaming user (needs sudo).
set -euo pipefail

readonly SUNSHINE_VERSION=2026.914.233613
readonly SUNSHINE_DEB=sunshine_${SUNSHINE_VERSION}-1+debiantrixie_amd64.deb
readonly SUNSHINE_URL=https://github.com/LizardByte/Sunshine/releases/download/v${SUNSHINE_VERSION}/${SUNSHINE_DEB}
readonly DISPLAY_NUMBER=:1

readonly ROOT=$(cd "$(dirname "$0")/.." && pwd)
readonly GAMES=${GAMES:-$ROOT/games}
readonly SUNSHINE_CONFIG=$HOME/.config/sunshine
readonly NVIDIA_PATCH_URL=https://raw.githubusercontent.com/keylase/nvidia-patch/7f30a61d159fa4c9644a7bf9d71a679c07062f5f/patch-fbc.sh
readonly USER_UNITS=$HOME/.config/systemd/user

install_packages() {
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        xserver-xorg-input-libinput matchbox-window-manager pipewire pipewire-pulse wireplumber curl
    if [[ $(dpkg-query -W -f '${Version}' sunshine 2>/dev/null) != "${SUNSHINE_VERSION}-1" ]]; then
        curl -fsSL -o "/tmp/$SUNSHINE_DEB" "$SUNSHINE_URL"
        sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "/tmp/$SUNSHINE_DEB"
    fi
}

# Sunshine's udev rules open uinput (virtual keyboard) and uhid (virtual DualSense) to the
# input group, once the modules are loaded.
allow_virtual_input() {
    printf 'uinput\nuhid\n' | sudo tee /etc/modules-load.d/ps-river.conf >/dev/null
    sudo modprobe -a uinput uhid
    # The X server's bell otherwise beeps through the PC speaker on synthetic key presses.
    echo 'blacklist pcspkr' | sudo tee /etc/modprobe.d/ps-river-no-beep.conf >/dev/null
    sudo modprobe -r pcspkr 2>/dev/null || true
    sudo usermod -aG input "$USER"
}

# Sunshine's NvFBC capture takes the X screen's frames on the GPU and hands them to NVENC
# without a copy through the CPU (KMS capture cannot: the NVIDIA X driver's virtual monitor is
# not a DRM plane). GeForce drivers keep NvFBC to professional cards: keylase/nvidia-patch
# unlocks it (it backs the library up first; patch-fbc.sh -r restores it). Run again after a
# driver update.
enable_gpu_capture() {
    curl -fsSL -o /tmp/patch-fbc.sh "$NVIDIA_PATCH_URL"
    sudo bash /tmp/patch-fbc.sh
}

start_display() {
    sudo tee /etc/systemd/system/ps-river-display.service >/dev/null <<EOF
[Unit]
Description=Virtual monitor for game streaming (Xorg $DISPLAY_NUMBER)
After=systemd-user-sessions.service

[Service]
ExecStart=/usr/lib/xorg/Xorg $DISPLAY_NUMBER -config $ROOT/host/xorg.conf -ac -nolisten tcp -noreset vt7
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
    sudo systemctl daemon-reload
    sudo systemctl enable --now ps-river-display.service
}

configure_sunshine() {
    mkdir -p "$SUNSHINE_CONFIG" "$USER_UNITS"
    cat >"$SUNSHINE_CONFIG/sunshine.conf" <<EOF
capture = nvfbc
encoder = nvenc
gamepad = ds5
EOF
    python3 -I "$ROOT/host/sunshine_apps.py" "$GAMES" "$ROOT" >"$SUNSHINE_CONFIG/apps.json"
    cat >"$USER_UNITS/ps-river-wm.service" <<EOF
[Unit]
Description=Kiosk window manager on the virtual monitor $DISPLAY_NUMBER

[Service]
Environment=DISPLAY=$DISPLAY_NUMBER
ExecStart=/usr/bin/matchbox-window-manager -use_titlebar no -use_cursor no
Restart=on-failure
RestartSec=2s

[Install]
WantedBy=default.target
EOF
    # The packaged unit waits for a desktop session, which this host never has.
    cat >"$USER_UNITS/ps-river-sunshine.service" <<EOF
[Unit]
Description=Sunshine streaming the virtual monitor $DISPLAY_NUMBER
After=pipewire-pulse.service

[Service]
Environment=DISPLAY=$DISPLAY_NUMBER
ExecStart=/usr/bin/sunshine
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=default.target
EOF
}

# A gamepad drives every game here: the emulator's mouse pointer stays hidden (cursor_state 2),
# not shown over the stream whenever the client moves its mouse.
hide_cursor() {
    local config=$HOME/.local/share/shadPS4/config.json
    [[ -f $config ]] || return 0
    python3 -I - "$config" <<'EOF'
import json, sys
path = sys.argv[1]
with open(path) as f:
    config = json.load(f)
for section in config.values():
    if isinstance(section, dict) and "cursor_state" in section:
        section["cursor_state"] = 2
with open(path, "w") as f:
    json.dump(config, f, indent=4)
EOF
}

start_user_services() {
    sudo loginctl enable-linger "$USER"
    systemctl --user daemon-reload
    systemctl --user enable --now pipewire pipewire-pulse wireplumber
    systemctl --user disable --now app-dev.lizardbyte.app.Sunshine.service
    systemctl --user enable ps-river-wm.service ps-river-sunshine.service
    # (A restarted user manager picks up the input group.)
    sudo systemctl restart "user@$(id -u).service"
}

install_packages
allow_virtual_input
enable_gpu_capture
start_display
hide_cursor
configure_sunshine
start_user_services
echo "Sunshine: https://$(hostname):47990 (set its login on first visit)"
