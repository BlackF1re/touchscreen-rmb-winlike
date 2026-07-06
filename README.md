# TouchRMB

TouchRMB is a lightweight touchscreen long-press to right-click implementation for the Chuwi Vi10 LXQt session.

The project contains two binaries:

- `touchrmb`: low-overhead daemon in plain C
- `touchrmb-settings`: settings UI in C/GTK

Design goals:

- no Python runtime for the resident daemon
- direct `/dev/input/event*` reading via `linux/input.h`
- `poll()` event loop with idle CPU near zero
- `X11 + XShape` overlay for the expanding square
- user-configurable hold delay, animation duration, square size, border width, and color

## Build

```sh
make
```

Artifacts:

```sh
build/touchrmb
build/touchrmb-settings
```

## Install

```sh
install -Dm755 build/touchrmb ~/.local/bin/touchrmb
install -Dm755 build/touchrmb-settings ~/.local/bin/touchrmb-settings
install -Dm755 packaging/bin/run-touchrmb-in-lxqt-session.sh ~/.local/bin/run-touchrmb-in-lxqt-session.sh
install -Dm644 packaging/systemd-user/touchrmb.service ~/.config/systemd/user/touchrmb.service
install -Dm644 packaging/applications/touchrmb.desktop ~/.local/share/applications/touchrmb.desktop
systemctl --user daemon-reload
systemctl --user enable --now touchrmb.service
```

## Configuration

User configuration is stored at:

```sh
~/.config/touchrmb/config.ini
```

The settings application writes the config and restarts the user service.

## Runtime notes

- Touch device name is hardcoded as `CHPN0001:00`
- Border-only overlay keeps the daemon light
- Daemon log: `~/.cache/touchrmb.log`
- Daemon lock: `~/.cache/touchrmb.lock`
