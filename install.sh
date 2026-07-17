#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
TARGET_USER="${1:-${SUDO_USER:-${USER:-}}}"
TARGET_UID=""
MISSING=""
PKG_HINT=""
WARNINGS=""

append_line() {
    name="$1"
    line="${2:-}"
    [ -n "$line" ] || return 0
    eval "current=\${$name-}"
    if [ -n "$current" ]; then
        current="$current
$line"
    else
        current="$line"
    fi
    eval "$name=\$current"
}

have_pkg_config_module() {
    pkg-config --exists "$@" 2>/dev/null
}

append_pkg_hint() {
    if command -v apt-get >/dev/null 2>&1; then
        append_line PKG_HINT "sudo apt-get install build-essential pkg-config libx11-dev libxext-dev libxtst-dev libxi-dev libxrandr-dev libgtk-3-dev"
        return
    fi
    if command -v dnf >/dev/null 2>&1; then
        append_line PKG_HINT "sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXext-devel libXtst-devel libXi-devel libXrandr-devel gtk3-devel"
        return
    fi
    if command -v pacman >/dev/null 2>&1; then
        append_line PKG_HINT "sudo pacman -S base-devel pkgconf libx11 libxext libxtst libxi libxrandr gtk3"
        return
    fi
}

if [ "$(uname -s)" != "Linux" ]; then
    echo "TouchRMB supports Linux only." >&2
    exit 1
fi

if [ -z "$TARGET_USER" ] || ! id "$TARGET_USER" >/dev/null 2>&1; then
    echo "Usage: ./install.sh [linux-user]" >&2
    exit 1
fi
TARGET_UID="$(id -u "$TARGET_USER")"

for command in make cc pkg-config systemctl install id getent; do
    if ! command -v "$command" >/dev/null 2>&1; then
        append_line MISSING "Missing command: $command"
    fi
done

if [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
    append_line MISSING "Missing command: sudo"
fi

if ! have_pkg_config_module x11 xext xtst xi xrandr gtk+-3.0; then
    append_line MISSING "Missing development packages for X11/XInput/XRandR/GTK3"
    append_pkg_hint
fi

if [ "${XDG_SESSION_TYPE:-}" != "" ] && [ "${XDG_SESSION_TYPE:-}" != "x11" ]; then
    append_line WARNINGS "Current session type is ${XDG_SESSION_TYPE}. TouchRMB targets X11 desktop sessions."
fi

if [ ! -S "/run/user/$TARGET_UID/bus" ]; then
    append_line WARNINGS "systemd user bus for $TARGET_USER is not reachable right now. Installation can complete, but service start may wait for a real graphical login."
fi

if [ -n "$MISSING" ]; then
    printf 'Cannot install TouchRMB:\n%s\n' "$MISSING" >&2
    if [ -n "$PKG_HINT" ]; then
        printf '\nSuggested packages:\n%s\n' "$PKG_HINT" >&2
    fi
    exit 1
fi

if [ -n "$WARNINGS" ]; then
    printf 'Warnings:\n%s\n\n' "$WARNINGS"
fi

printf 'Installing TouchRMB for %s...\n' "$TARGET_USER"

if [ "$(id -u)" -eq 0 ]; then
    exec sh "$SCRIPT_DIR/install-user.sh" "$TARGET_USER"
fi

exec sudo sh "$SCRIPT_DIR/install-user.sh" "$TARGET_USER"
