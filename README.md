# vex

Minimal BSP/dwindle tiling window manager for X11. New windows bisect the focused tile, alternating split orientation by aspect ratio. Floating windows sit above tiled ones and can be dragged or resized freely. Multi-monitor setups are handled via Xinerama — each screen has its own independent tree.

## Dependencies

- `libX11`
- `libXinerama`
- C99 compiler

Arch: `sudo pacman -S libx11 libxinerama`
Void: `sudo xbps-install libX11-devel libxinerama-devel`
Gentoo: `x11-libs/libX11 x11-libs/libXinerama`

## Build

```sh
make
sudo make install
```

Add to `~/.xinitrc`:

```sh
exec vex
```

## Configuration

`/etc/vex/config.conf` or `./config.conf`. No recompile needed.

```ini
gap            = 8
border         = 2
border_focus   = 5588ff
border_normal  = 333333
terminal       = st

bind = mod+b            = firefox
bind = mod+shift+Return = alacritty
```

Supported modifiers: `mod` (Super), `shift`, `ctrl`, `alt`.

## Keybinds

| Key              | Action                  |
|------------------|-------------------------|
| mod+Return       | Spawn terminal          |
| mod+q            | Kill focused window     |
| mod+shift+q      | Quit vex                |
| mod+Tab          | Cycle focus             |
| mod+f            | Toggle float            |
| mod+h / mod+l    | Shrink / grow split     |
| mod+v            | Flip split orientation  |

## Floating

`mod+f` centers the window at half screen size. Drag with `mod+Button1`, resize with `mod+Button3`. Dragging a float to another monitor migrates it to that screen's tree automatically.

## Layout

Splits follow the focused tile's aspect ratio — wider tiles split horizontally, taller ones vertically. `mod+v` flips the active split, `mod+h`/`mod+l` adjust the ratio in 5% steps.

## Multi-Monitor

Each physical monitor maintains its own BSP tree. New windows open on the monitor under the pointer. Focus, cycling, and splits are scoped per monitor. Floating windows cross monitor boundaries freely.
