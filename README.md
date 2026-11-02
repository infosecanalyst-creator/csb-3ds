# csb-3ds — a New 3DS front end for CSBwin (plays your own Dungeon Master / Chaos Strikes Back files)

A 3DS front end for **CSBwin**, Paul R. Stevens' faithful re-implementation of the
Dungeon Master / Chaos Strikes Back engine (SDL2 branch maintained at
github.com/zelurker/CSB). The game logic is untouched; this project only replaces
the window, renderer and input with a dual-screen 3DS layer.

## Screen layout

The layout follows the game's own state, so there is nothing to toggle.

**In the dungeon** (normal play)

| Screen | What it shows |
|---|---|
| **Bottom** (touch) | Champion portraits and hands across the top, the dungeon view at 1.3x under them (tap to grab items, push buttons, open doors, throw), and five tabs: **FIGHT** (attack menu), **SPELL** (rune panel), **RECAST** (repeat the last spell), **BAG** (leader's inventory), **SWAP**. The attack menu and runes pop up over the right side of the view at 2x. |
| **Top** (information) | Party panel (each champion's name in their colour, labelled **HP** / **STA** / **MP** bars with current/max, **FOOD** and **WATER** bars, POISON or DEAD flags), an **auto-map** of the squares you have seen (solid lines are walls, dotted lines are open edges; a white arrow with a red tip shows where you face), doors, stairs and pits, the level, compass and light level, and a log of the last messages. With the 3D slider up the map sits behind the screen and your arrow in front. |

**SWAP** puts the big dungeon view back on the top screen (full width, with stereo 3D,
messages over the bottom and slim vitals bars on the edges) and the map on the bottom.
The choice is remembered.

**Inventory open**: the information screen on top; on the bottom, the portraits plus
the inventory at 1.4x so the slots are finger-sized.

**Everywhere else** (title, entrance, menus, champion mirror, freeze, sleep, save menu)

| Screen | What it shows |
|---|---|
| **Top** | The whole game screen at 1.2x, centred. |
| **Bottom** | Zoomed in on that screen's buttons (2x or 1.5x when they fit) so they are big enough for a finger; otherwise the whole screen at 1:1. Hold **ZL** to see the whole screen at 1:1. |

**Before anyone joins the party**, the bottom screen shows the dungeon view at 1:1
and the movement arrows at 2x, because the portrait row, spell runes and attack
menu are still empty.

## Controls

| Input | Action |
|---|---|
| D-pad / Circle Pad | Step forward/back, turn left/right (hold to keep going); there are no on-screen arrows |
| **L** / **R** | Strafe left / right |
| **A / B / X / Y** | Champion 1 / 2 / 3 / 4 attacks (first attack) in one press; hold to keep attacking whenever the weapon is ready |
| **ZL** + A/B/X/Y | That champion's second attack |
| **ZR** + A/B/X/Y | That champion's third attack |
| **FIGHT** / **SPELL** tabs | Show the attack menu / spell runes (the attack menu also appears by itself while open) |
| **RECAST** tab | Repeat the last spell; hold it to choose any champion's last spell |
| **SELECT** or **BAG** tab | Open / close the leader's inventory (with an item in hand BAG reads **STOW** and puts it in the leader's backpack) |
| Double-tap food or a flask (inventory) | Eat or drink it |
| **ZL** held (dungeon) | Whole message log on the top screen |
| **SWAP** tab | Dungeon view on the bottom (touch) or on the top (3D) |
| **HELP** tab, or **SELECT** on menu screens | The instruction manual |
| Tap | Left click on whatever is under your finger |
| **ZR** held + tap | Right click |
| **ZL** (menus) | Show the whole bottom screen at 1:1 instead of the zoomed buttons |
| **START** | Freeze game |
| **START + SELECT** | Quit (asks first) |
| 3D slider | Stereo depth (slide down to turn it off) |

The attack and inventory shortcuts only act in the dungeon (and SELECT in the
inventory), so they can never press a menu button by accident. Rationale and tap
counts: [docs/UX-AUDIT.md](docs/UX-AUDIT.md).

## Install

1. Build (below) or use a built `out/csb3ds.3dsx`.
2. On the SD card create `3ds/csb3ds/` and copy in:
   - `csb3ds.3dsx` and `csb.cfg` from `out/`
   - `GRAPHICS.DAT` and `DUNGEON.DAT` for the game you want to play
3. Launch **csb-3ds** from the Homebrew Launcher.

**Game data.** No original game data is included, and this project does not tell you
where to get it. You need `GRAPHICS.DAT` and `DUNGEON.DAT` from a copy of the game you
own.

**Switching games.** CSBwin plays DM1, Chaos Strikes Back and fan dungeons (e.g. Conflux)
from the same executable. Put each game's files under its own name and point
`dungeonName` / `graphicName` in `csb.cfg` at the pair you want.

**Saves** are written to `sdmc:/3ds/csb3ds/` by the in-game save (the disk icon).

## Build

Needs Docker Desktop (Windows) or Docker / a devkitPro install (Linux/macOS).

- **Windows:** in this folder, run `.\build-windows.ps1`
- **Linux/macOS:** `docker run --rm -v "$PWD":/work -w /work devkitpro/devkitarm bash build.sh`

`build.sh` clones CSB at a pinned commit, applies `patch_csb_3ds.py`, adds the `n3ds/`
platform layer and builds with `Makefile.3ds`. Output: `out/csb3ds.3dsx`.

The patch is anchor-checked: if upstream code moves, it stops and names the anchor that
failed instead of producing a half-patched tree. Every change is behind `#ifdef __3DS__`.

## Instruction manual

The manual is built into the game: the **HELP** tab in the dungeon, or **SELECT** on the
menus and the entrance. The section text is on the top screen; the touch screen has a big
button per section and CLOSE. Controls are the same as in the other 3DS ports: Circle Pad
or C-Stick scroll (hold to keep scrolling), the D-pad moves between the section buttons,
L / R go to the previous or next section, tap a section, B or START closes. The game waits
while it is open, and the manual reopens where you left it.

Touch buttons (the tabs, the manual's buttons, the RECAST chooser) give a very quiet click
and a visible press.

Its text is `n3ds/manual.txt`, compiled in by `tools/embed_manual.py`. A `manual.txt` in
`sdmc:/3ds/csb3ds/` replaces it without a rebuild.

## Debugging

Create an empty file named `debug.txt` in `sdmc:/3ds/csb3ds/` to get `csb3ds.log` there
(the engine's own output plus a trace of every tap and layout switch) and a magenta
crosshair on the bottom screen where each tap lands. Delete the file to turn it off.

## Status

Tested in the Azahar emulator: boots the original Dungeon Master data, menus and the
entrance respond to taps, the party walks and turns with the D-pad and the on-screen
arrows, and the layout switches between the full-screen and dungeon layouts.

Still to verify on hardware: stereo depth, sound (needs `sdmc:/3ds/dspfirm.cdc` from
DSP1), New 3DS performance, and how the enlarged spell runes and attack buttons feel
with a finger once champions are recruited.

## Next steps

- v2 stereo: take real per-object depth from `Viewport.cpp` (it draws in distance order),
  so monsters and items pop correctly.
- Dungeon Master II (Skullkeep): separate engine (SKWin / skproject); a Nintendo DS port
  (jjsimpso/dmc-ds) is the likely starting point.

## Files

| File | Purpose |
|---|---|
| `n3ds/n3ds_platform.cpp/.h` | Layouts, stereo depth, touch + button input, message box |
| `n3ds/n3ds_compat.h`, `n3ds/endian.h` | Headers glibc provides on Linux but newlib does not |
| `n3ds/n3ds_filebrowser_stub.cpp` | Replaces the desktop-only ImGui file dialog |
| `patch_csb_3ds.py` | Anchored edits to the CSBwin sources (all behind `#ifdef __3DS__`) |
| `Makefile.3ds` | devkitARM build producing `csb3ds.3dsx` |
| `build.sh` / `build-windows.ps1` | One-command Docker build |

## Licence

The files in this repository (the 3DS layer, patch script and build scripts) are MIT
licensed; see `LICENSE`. CSBwin itself is not part of this repository: the build clones it at a pinned
commit and patches it locally (the patch script quotes short anchor lines from CSBwin
only to find where each edit goes). `out/csb.cfg` is generated from CSBwin's own config
at build time.

CSBwin's source has no stated licence, so do not publish built `.3dsx` files; share the
source and let people build it.

CSBwin is (c) Paul R. Stevens and contributors (Linux/SDL2 maintenance by Emmanuel Anne,
github.com/zelurker/CSB). Dungeon Master and Chaos Strikes Back are (c) FTL Games /
Software Heaven. No game data is included or distributed.

This project is not affiliated with or endorsed by FTL Games, Software Heaven, Paul R.
Stevens or Nintendo. *Dungeon Master* and *Chaos Strikes Back* are trademarks of their
respective owners and are used here only to say which game files the engine reads.
