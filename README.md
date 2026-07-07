# TouchRMB

`TouchRMB` adds Windows-like right click by long press on a touchscreen in `LXQt/X11`.

It installs:

- `touchrmb` - low-overhead resident daemon in C
- `touchrmb-settings` - GUI for delay, animation, size, border width, and color

## Requirements

- Linux
- X11 session
- LXQt session
- `systemd --user`
- build tools and headers for: `x11`, `xext`, `xtst`, `xi`, `xrandr`, `gtk+-3.0`

## Install

Clone the repository, enter it, then run:

```sh
./install.sh
```

If prerequisites are missing, the script will stop and print the packages to install.

## Use

- Settings app: `touchrmb-settings`
- Config file: `~/.config/touchrmb/config.ini`

## Notes

- The daemon works with direct-touch `evdev` devices and prefers `CHPN0001:00` when present.
- It is designed for `LXQt/X11`, not for Wayland.
