# vex

Minimal BSP/dwindle tiling window manager for X11. Each new window bisects the focused tile, alternating split orientation based on aspect ratio — matching Hyprland's dwindle behavior. Windows can be toggled to float and dragged/resized freely. With Xinerama, each physical monitor hosts its own independent BSP tree, and floating windows can be dragged across screens seamlessly.

## Dependencies

- Xlib (`libX11`)
- Xinerama (`libXinerama`)
- C99 compiler

Arch: `sudo pacman -S libx11 libxinerama`
Void: `sudo xbps-install libX11-devel libxinerama-devel`
Gentoo: `x11-libs/libX11 x11-libs/libXinerama` (libX11 usually already present)

## Build

```sh
git clone https://github.com/yourname/vex
cd vex
make
```

## Install

```sh
sudo make install
```

Add to `~/.xinitrc`:

```sh
exec vex
```

## Configuration

Edit `config.conf` before launching. vex looks for it at `/etc/vex/config.conf`, then `./config.conf`. No recompile needed.

```ini
gap            = 8
border         = 2
border_focus   = 5588ff
border_normal  = 333333
terminal       = st

bind = mod+b            = firefox
bind = mod+shift+Return = alacritty
```

`mod` = Super key. Supported modifiers: `mod`, `shift`, `ctrl`, `alt`.

## Keybinds

| Key            | Action                       |
|----------------|------------------------------|
| mod+Return     | Spawn terminal               |
| mod+q          | Kill focused window          |
| mod+shift+q    | Quit vex                     |
| mod+Tab        | Cycle focus                  |
| mod+f          | Toggle float on focused      |
| mod+h          | Shrink current split         |
| mod+l          | Grow current split           |
| mod+v          | Flip split orientation       |

## Floating

`mod+f` toggles the focused window into a centered floating state. Float windows stay above tiled ones. Drag with `mod+Button1`, resize with `mod+Button3`. Both are bounded by screen edges.

Mouse events are fully isolated during drag and resize — applications inside the window (browsers, terminals, etc.) will not receive scroll or click events while you are holding the modifier.

## Layout

New windows always split the focused tile. If the tile is wider than tall, it splits horizontally (side by side); otherwise vertically (top/bottom). This mirrors Hyprland's dwindle layout. Use `mod+v` to flip the active split, `mod+h`/`mod+l` to adjust the ratio.

## Multi-Monitor (Xinerama)

vex detects all physical monitors via Xinerama at startup. Each monitor maintains its own completely independent BSP tree, so tiling on one screen never affects another.

**Window placement:** new windows are inserted into the tree of whichever monitor the mouse pointer is on at the moment the window appears.

**Focus:** mouse-over and click focus are monitor-aware. `mod+Tab` cycles windows within the current monitor.

**Floating across screens:** a floating window (`mod+f`) can be dragged freely across monitor boundaries. On `ButtonRelease`, vex detects which monitor the pointer has landed on and migrates the window's tree node there automatically — no gaps or stretching occur.

**Single-monitor fallback:** if Xinerama is unavailable or reports only one screen, vex behaves identically to v1.
