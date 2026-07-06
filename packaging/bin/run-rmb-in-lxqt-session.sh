#!/bin/sh
set -eu

find_lxqt_session_pid() {
    pgrep -n lxqt-session 2>/dev/null || true
}

load_session_env() {
    pid="$1"
    python3 - "$pid" <<'PY'
import pathlib
import sys

pid = sys.argv[1]
raw = pathlib.Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
keep = {
    "DBUS_SESSION_BUS_ADDRESS",
    "DISPLAY",
    "GTK_CSD",
    "GTK_OVERLAY_SCROLLING",
    "QT_PLATFORM_PLUGIN",
    "QT_QPA_PLATFORMTHEME",
    "XAUTHORITY",
    "XDG_CACHE_HOME",
    "XDG_CONFIG_DIRS",
    "XDG_CONFIG_HOME",
    "XDG_CURRENT_DESKTOP",
    "XDG_DATA_DIRS",
    "XDG_DATA_HOME",
    "XDG_MENU_PREFIX",
    "XDG_RUNTIME_DIR",
    "XDG_SESSION_DESKTOP",
    "XDG_SESSION_TYPE",
}
for item in raw:
    if not item or b"=" not in item:
        continue
    key, value = item.split(b"=", 1)
    key = key.decode("utf-8", "ignore")
    if key not in keep:
        continue
    value = value.decode("utf-8", "ignore").replace("\\", "\\\\").replace("\"", "\\\"")
    print(f'export {key}="{value}"')
PY
}

while :; do
    pid="$(find_lxqt_session_pid)"
    if [ -n "$pid" ] && [ -r "/proc/$pid/environ" ]; then
        eval "$(load_session_env "$pid")"
        exec "$@"
    fi
    sleep 2
done
