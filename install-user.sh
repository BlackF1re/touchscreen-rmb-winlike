#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

TARGET_USER="${1:-${SUDO_USER:-}}"
if [ -z "$TARGET_USER" ] || ! id "$TARGET_USER" >/dev/null 2>&1; then
    echo "Usage: sudo ./install-user.sh <linux-user>" >&2
    exit 1
fi

TARGET_UID="$(id -u "$TARGET_USER")"
TARGET_RUNTIME_DIR="/run/user/$TARGET_UID"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
TARGET_USER_SYSTEMD_DIR="$TARGET_HOME/.config/systemd/user"

cd "$SCRIPT_DIR"
su - "$TARGET_USER" -c "cd '$SCRIPT_DIR' && make clean && make"
make install-only PREFIX=/usr/local

rm -f \
    "$TARGET_USER_SYSTEMD_DIR/touchrmb.service" \
    "$TARGET_HOME/.local/bin/touchrmb" \
    "$TARGET_HOME/.local/bin/touchrmb-settings" \
    "$TARGET_HOME/.local/bin/run-touchrmb-in-lxqt-session.sh"

install -d "$TARGET_HOME/.local/share/applications"
install -m 0644 packaging/applications/touchrmb.desktop "$TARGET_HOME/.local/share/applications/touchrmb.desktop"
chown -R "$TARGET_USER:$TARGET_USER" "$TARGET_HOME/.local/share/applications"

if command -v systemctl >/dev/null 2>&1; then
    if [ -S "$TARGET_RUNTIME_DIR/bus" ]; then
        su - "$TARGET_USER" -c "XDG_RUNTIME_DIR='$TARGET_RUNTIME_DIR' systemctl --user disable --now touchrmb.service >/dev/null 2>&1 || true"
        su - "$TARGET_USER" -c "XDG_RUNTIME_DIR='$TARGET_RUNTIME_DIR' systemctl --user daemon-reload"
        su - "$TARGET_USER" -c "XDG_RUNTIME_DIR='$TARGET_RUNTIME_DIR' systemctl --user enable --now touchrmb.service >/dev/null"
    fi
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/local/share/applications >/dev/null 2>&1 || true
fi

echo "TouchRMB installed for $TARGET_USER"
