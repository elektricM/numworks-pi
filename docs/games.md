# Games

Games that run well on the Pi Zero 2 W and fit the calculator's keys and screen. Each has an icon on the desktop. `pi-linux/scripts/install-components.sh` installs them from the Debian packages, and `nw-game` (`pi-linux/apps/nw-game`) starts each one with the right settings:
- native Wayland (`SDL_VIDEODRIVER=wayland`, no XWayland);
- no audio, since the Pi has no sound card;
- a calculator-friendly config the first time a game runs, taken from `pi-linux/config/games/`.

| Icon | Game | Package | Play in |
|---|---|---|---|
| Doom | Freedoom: Phase 1 on Chocolate Doom | `chocolate-doom`, `freedoom` | keyboard mode |
| Tetris | LTris 2 | `ltris` | keyboard mode |
| Breakout | LBreakoutHD | `lbreakout2` | mouse mode |
| Steel Sky | Beneath a Steel Sky (ScummVM) | `scummvm`, `beneath-a-steel-sky` | mouse mode |
| Amazon Queen | Flight of the Amazon Queen (ScummVM) | `scummvm`, `flight-of-the-amazon-queen` | mouse mode |
| Luanti | Luanti (the open Minecraft-like) with Minetest Game, world "Calculator" | `luanti`, `luanti-game-minetest` | mouse mode |
| SuperTux | SuperTux 0.6 | `supertux` | keyboard mode |
| Neverball | Neverball (tilt the floor, roll the ball) | `neverball` | keyboard mode |
| Chromium BSU | Chromium B.S.U. (vertical shooter) | `chromium-bsu` | keyboard mode |
| Mr. Rescue | Mr. Rescue (LÖVE arcade) | `mrrescue` | keyboard mode |
| OpenTTD | OpenTTD with OpenGFX | `openttd`, `openttd-opengfx` | mouse mode |
| Puzzles (folder) | 10 of Simon Tatham's puzzles: Mines, Sudoku, Fifteen, Net, Flood, Same Game, Keen, Towers, Unequal, Light Up | `sgt-puzzles` | keyboard mode |

## Keys

Power toggles mouse mode (see [keymap](keymap.md)):
- **In keyboard mode,** arrows are arrow keys, OK is Enter and Back is Escape, which opens or leaves the menu in every game.
- **In mouse mode,** arrows move the pointer, and OK and Back are the left and right buttons.

Controls are each game's defaults; only Doom's and Tetris's were remapped. They come from the games' documentation and settings and weren't play-tested on the calculator, so adjust them in each game's options if one is wrong.

| Game | Controls |
|---|---|
| Doom | Arrows move and turn, x10^x (Ctrl) fires, space opens doors, Shift runs, ans (Alt) strafes, Back opens the menu, EXE or OK selects. To quit: Back, *Quit Game*, then Y. |
| Tetris | Left and Right move, Up rotates, Z rotates the other way, Down drops softly, space drops hard, Toolbox (Tab) holds. Back is the menu. |
| Breakout | In mouse mode: arrows move the paddle and OK fires. Back is the menu. |
| ScummVM games | In mouse mode: OK walks or uses, Back looks or examines. Ctrl+F5 (x10^x, then `var` and E) opens the ScummVM menu, which can save, load and quit. |
| Luanti | In mouse mode: arrows look, W A S D (letter keys) walk, space jumps, Shift sneaks, OK digs, Back places, I opens the inventory. Escape (Backspace after `var`) is the pause menu. |
| SuperTux | Arrows run and duck, space jumps, x10^x (Ctrl) runs or fires. Back is the menu. |
| Neverball | Arrows tilt the floor; EXE or OK starts a level. Back is the menu. |
| Chromium BSU | Arrows fly, x10^x (Ctrl) fires. Back is the menu. |
| Mr. Rescue | Arrows move, space jumps, x10^x (Ctrl) sprays water. Back is the menu. |
| OpenTTD | In mouse mode: OK clicks, Back drags the map. |
| Puzzles | Arrows move the cursor; EXE, OK or space play a move, depending on the puzzle. The window's menu has New game and Quit. |

## Notes

- **Screen grabs:** a fullscreen game is scanned out directly, so `grim` shows the desktop under it rather than the game. Use `nwpi-ctl px X Y` to read what the panel actually shows.
- **GPU limits:** the VideoCore IV runs OpenGL 2.1 / ES 2.0 only, so games that need OpenGL 3 / ES 3 or Vulkan (SuperTuxKart, most newer 3D games) can't run. SuperTux falls back to its SDL renderer. Debian's Luanti only has an X11 window, so `nw-game` starts it on XWayland. Luanti gets low settings (`config/games/luanti/minetest.conf`: view range 40, 30 fps cap, no clouds or shaders) and uses about 160 MB of RAM.
- **Tested and dropped:** X-Moto reached only 10 fps.
- **Measured:** with the link at 20 MHz the panel showed about 12 fps for Luanti; Neverball, Chromium BSU and Mr. Rescue ran at about 40 fps, OpenTTD at 32 fps (at 35 MHz).
- **Rendering:** the normal GPU path is the fastest: LTris ran at 43 fps using 33% of the CPU. Software rendering (`LIBGL_ALWAYS_SOFTWARE=1`, `SDL_RENDER_DRIVER=software`) was slower (15–23 fps), so the launcher doesn't use it.
- **Link corruption:** corrupt frames are shifted by a few bits from some point on (one side of the screen fine, the other in wrong colours); header errors (`bad=` in `nwpi-ctl stat`) don't catch them. They come in long full-motion sessions (games, the camera) and track temperature: 230 bad headers in 403 frames at 64.5 °C, 2 in 518 at 57 °C, same game and clock. A 40-second CPU-only heat test didn't reproduce it, probably because it doesn't warm the wiring. Moving the wires helped. Suspects: the damaged SPI wiring (SCLK/ground), and possibly the driver's fixed 10 µs gap between a frame and the next header (`NW_FRAME_GAP_US`), which is shorter than the calculator's per-frame handler at its worst (28 µs). Lowering `spi_hz` trades smoothness (heavy tearing at 10 MHz) for fewer errors.
- **Heat (earlier reading):** full-motion games (Doom, the camera) are what make the marginal MOSI wire fail when the Pi is warm (`bad=` in `nwpi-ctl stat`, with corrupt pixels on the panel). Until the wire is reflowed:
  - `echo 35000000 | sudo tee /sys/module/drm_spifb/parameters/spi_hz` lowers the clock, which helps somewhat, until the next reboot;
  - `echo 1 | sudo tee /sys/module/drm_spifb/parameters/repaint` clears leftover garbage.
- **Old workarounds:** the old Doom notes (software rendering, `setsid`, a labwc fullscreen rule) were for an earlier display stack and aren't needed any more.
