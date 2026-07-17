#!/bin/sh
set -eu

find_session_pid() {
    for name in \
        lxqt-session \
        xfce4-session \
        mate-session \
        cinnamon-session \
        gnome-session-binary \
        gnome-session \
        startplasma-x11 \
        plasmashell \
        openbox
    do
        pid="$(pgrep -n -x "$name" 2>/dev/null || true)"
        if [ -n "$pid" ]; then
            printf '%s\n' "$pid"
            return 0
        fi
    done
    return 1
}

load_session_env() {
    pid="$1"
    tr '\0' '\n' <"/proc/$pid/environ" | while IFS='=' read -r key value; do
        case "$key" in
            DBUS_SESSION_BUS_ADDRESS|DISPLAY|GTK_CSD|GTK_OVERLAY_SCROLLING|QT_PLATFORM_PLUGIN|QT_QPA_PLATFORMTHEME|XAUTHORITY|XDG_CACHE_HOME|XDG_CONFIG_DIRS|XDG_CONFIG_HOME|XDG_CURRENT_DESKTOP|XDG_DATA_DIRS|XDG_DATA_HOME|XDG_MENU_PREFIX|XDG_RUNTIME_DIR|XDG_SESSION_DESKTOP)
                escaped="$(printf '%s' "$value" | sed 's/[\\"]/\\&/g')"
                printf 'export %s="%s"\n' "$key" "$escaped"
                ;;
        esac
    done
}

while :; do
    pid="$(find_session_pid || true)"
    if [ -n "$pid" ] && [ -r "/proc/$pid/environ" ]; then
        eval "$(load_session_env "$pid")"
        if [ "${DISPLAY:-}" = "" ]; then
            sleep 2
            continue
        fi
        exec "$@"
    fi
    sleep 2
done
