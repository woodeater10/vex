# vex

Minimal BSP/dwindle tiling window manager for X11. Each new window bisects the focused tile, 
alternating split orientation based on aspect ratio — matching Hyprland's dwindle behavior. 
Windows can be toggled to float and dragged/resized freely within screen bounds.

## Dependencies

- Xlib (`libX11`)
- C99 compiler

Arch: `sudo pacman -S libx11`
Void: `sudo xbps-install libX11-devel`
Gentoo: `x11-libs/libX11` (usually already present)

## Build

```sh
git clone https://github.com/woodeater10/vex
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

`mod+f` toggles the focused window into a centered floating state. Float windows stay above tiled ones. Drag with `mod+Button1`, resize with `mod+Button3`. Both operations are bounded by screen edges.

## Layout

New windows always split the focused tile. 
If the tile is wider than tall, it splits horizontally (side by side); 
otherwise vertically (top/bottom). This mirrors Hyprland's dwindle layout. 
Use `mod+v` to flip the active split, `mod+h`/`mod+l` to adjust the ratio.
