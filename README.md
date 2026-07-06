# Touchscreen RMB Winlike C

Lightweight touchscreen long-press to right-click daemon for the Chuwi Vi10 LXQt session.

This version is designed to replace the Python/GTK prototype with a smaller runtime footprint:

- `C` instead of Python
- direct `/dev/input/event*` reading via `linux/input.h`
- `poll()` event loop with no background busy wakeups in idle
- `X11 + XShape` border-only overlay
- existing `xinput` and `xdotool` tools are reused for pointer freeze and RMB emission

## Build

```sh
make
```

The resulting binary is:

```sh
build/touchscreen-rmb-winlike-c
```

## Install

```sh
install -Dm755 build/touchscreen-rmb-winlike-c ~/.local/bin/touchscreen-rmb-winlike-c
install -Dm755 packaging/bin/run-rmb-in-lxqt-session.sh ~/.local/bin/run-rmb-in-lxqt-session.sh
install -Dm644 packaging/systemd-user/touchscreen-rmb-winlike-c.service ~/.config/systemd/user/touchscreen-rmb-winlike-c.service
systemctl --user daemon-reload
systemctl --user disable --now touchscreen-rmb-winlike.service
systemctl --user enable --now touchscreen-rmb-winlike-c.service
```

## Runtime notes

- Touch device name is hardcoded as `CHPN0001:00`.
- Overlay is intentionally border-only to avoid heavier graphics dependencies.
- Log file: `~/.cache/touchscreen-rmb-winlike-c.log`
- Lock file: `~/.cache/touchscreen-rmb-winlike-c.lock`
