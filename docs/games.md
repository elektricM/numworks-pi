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
| Puzzles (folder) | 10 of Simon Tatham's puzzles: Mines, Sudoku, Fifteen, Net, Flood, Same Game, Keen, Towers, Unequal, Light Up | `sgt-puzzles` | keyboard mode |

## Keys

Power toggles mouse mode (see [keymap](keymap.md)):
- **In keyboard mode,** arrows are arrow keys, OK is Enter and Back is Escape, which opens or leaves the menu in every game.
- **In mouse mode,** arrows move the pointer, and OK and Back are the left and right buttons.

| Game | Controls |
|---|---|
| Doom | Arrows move and turn, x10^x (Ctrl) fires, space opens doors, Shift runs, ans (Alt) strafes, Back opens the menu, EXE or OK selects. To quit: Back, *Quit Game*, then Y. |
| Tetris | Left and Right move, Up rotates, Z rotates the other way, Down drops softly, space drops hard, Toolbox (Tab) holds. Back is the menu. |
| Breakout | In mouse mode: arrows move the paddle and OK fires. Back is the menu. |
| ScummVM games | In mouse mode: OK walks or uses, Back looks or examines. Ctrl+F5 (x10^x, then `var` and E) opens the ScummVM menu, which can save, load and quit. |
| Puzzles | Arrows move the cursor; EXE, OK or space play a move, depending on the puzzle. The window's menu has New game and Quit. |

## Notes

- **Screen grabs:** a fullscreen game is scanned out directly, so `grim` shows the desktop under it rather than the game. Use `nwpi-ctl px X Y` to read what the panel actually shows.
- **Rendering:** the normal GPU path is the fastest: LTris ran at 43 fps using 33% of the CPU. Software rendering (`LIBGL_ALWAYS_SOFTWARE=1`, `SDL_RENDER_DRIVER=software`) was slower (15–23 fps), so the launcher doesn't use it.
- **Heat:** full-motion games (Doom, the camera) are what make the marginal MOSI wire fail when the Pi is warm (`bad=` in `nwpi-ctl stat`, with corrupt pixels on the panel). Until the wire is reflowed:
  - `echo 35000000 | sudo tee /sys/module/drm_spifb/parameters/spi_hz` lowers the clock, which helps somewhat, until the next reboot;
  - `echo 1 | sudo tee /sys/module/drm_spifb/parameters/repaint` clears leftover garbage.
- **Old workarounds:** the old Doom notes (software rendering, `setsid`, a labwc fullscreen rule) were for an earlier display stack and aren't needed any more.
