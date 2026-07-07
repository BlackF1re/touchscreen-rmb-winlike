# TouchRMB

`TouchRMB` adds Windows-like right click by long press on a touchscreen in `LXQt/X11`.

It is an independent implementation inspired by the long-press right-click UX found on Windows tablets. No Microsoft code or assets are included.

## Preview

Long-press animation:

![TouchRMB demo](touchrmb-demo.gif)

Settings window:

![TouchRMB settings](touchrmb-settings.png)

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

Copy and paste this:

```sh
git clone https://github.com/BlackF1re/touchscreen-rmb-winlike.git
cd touchscreen-rmb-winlike
chmod +x install.sh
./install.sh
```

If the system is compatible but missing prerequisites, the installer will stop and print the exact packages to install.

## Use

- Settings app: `touchrmb-settings`
- Config file: `~/.config/touchrmb/config.ini`

## Notes

- The daemon works with direct-touch `evdev` devices and prefers `CHPN0001:00` when present.
- It is designed for `LXQt/X11`, not for Wayland.

## License

MIT. See [LICENSE](LICENSE).
