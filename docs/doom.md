# Running Doom

Chocolate Doom on the Pi, displayed on the NumWorks screen.

## Install

```bash
sudo apt-get install -y chocolate-doom freedoom
```

The `freedoom` package provides `freedoom1.wad` (all episodes) and `freedoom2.wad`. For the original shareware:

```bash
sudo wget -O /usr/share/games/doom/doom1.wad \
  https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
```

## Rendering Workarounds

Chocolate Doom segfaults with GPU rendering on the DRM/SPI display. Three workarounds are needed:

1. **Software rendering**: `LIBGL_ALWAYS_SOFTWARE=1 SDL_RENDER_DRIVER=software`
2. **Unset DISPLAY**: When launched from the desktop, `DISPLAY=:1` (XWayland) causes SDL to render a transparent window. Must `unset DISPLAY` to force pure Wayland.
3. **setsid**: Launch via `setsid` to create a new session, otherwise the window renders transparently.

## labwc Fullscreen Rule

Chocolate Doom's built-in fullscreen doesn't work on the custom DRM display. Add a labwc window rule instead.

`~/.config/labwc/rc.xml`:

```xml
<?xml version="1.0"?>
<labwc_config>
  <windowRules>
    <windowRule identifier="chocolate-doom" serverDecoration="no">
      <action name="ToggleFullscreen"/>
    </windowRule>
  </windowRules>
</labwc_config>
```

Reload: `killall -SIGHUP labwc`

## Desktop Launcher

Create `~/Desktop/Doom`:

```bash
#!/bin/bash
unset DISPLAY
export LIBGL_ALWAYS_SOFTWARE=1
export SDL_RENDER_DRIVER=software
exec setsid chocolate-doom -iwad /usr/share/games/doom/freedoom1.wad -turbo 100
```

```bash
chmod +x ~/Desktop/Doom
```

Requires `quick_exec=1` in `~/.config/libfm/libfm.conf` for pcmanfm to execute on double-click.

## Controls

| NumWorks Key | Doom Action |
|---|---|
| Arrow keys | Move / Turn |
| x10^x (Ctrl) | Fire |
| Space (minus key, mode 0) | Use / Open doors |
| Shift | Run |
| EXE | Enter (menu select) |
| Backspace (mode 1 = ESC) | Open menu |
| Y key (mode 0) | Confirm quit |

Toggle mouse mode with Power button for mouse aiming.

## Config Tweaks

In `~/.local/share/chocolate-doom/`:

- `default.cfg`: `key_speed 29` for always-run
- `chocolate-doom.cfg`: `fullscreen 0` (labwc rule handles it), `window_width 480`, `window_height 360`, `grabmouse 1`

## WAD Files

| WAD | Path | Content |
|-----|------|---------|
| doom1.wad | `/usr/share/games/doom/doom1.wad` | Original shareware (Episode 1) |
| freedoom1.wad | `/usr/share/games/doom/freedoom1.wad` | Free replacement, all episodes |
| freedoom2.wad | `/usr/share/games/doom/freedoom2.wad` | Free Doom 2 replacement |
