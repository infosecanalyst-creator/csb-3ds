# csb-3ds — Dungeon Master & Chaos Strikes Back on the New 3DS

A 3DS front end for **CSBwin**, Paul R. Stevens' faithful re-implementation of the
Dungeon Master / Chaos Strikes Back engine (SDL2 branch maintained at
github.com/zelurker/CSB). The game logic is untouched; this project only replaces
the window, renderer and input with a dual-screen 3DS layer.

## Screen layout

The layout follows the game's own state, so there is nothing to toggle.

**In the dungeon** (normal play, no inventory open)

| Screen | What it shows |
|---|---|
| **Top** (400x240) | The dungeon view at 1.5x (336x204) with the message strip underneath, and every champion's health / stamina / mana bars in the side margins (health turns red below 25%). The 3D slider adds stereoscopic depth. |
| **Bottom** (320x240) | Controls only: champion portraits and hands at 1:1, spell runes / attack menu / movement arrows at 1.5x, a small framed copy of the dungeon view for grabbing items, pressing wall buttons and opening doors, and the RECAST and BAG buttons. |

**Inventory open**: the whole screen on top; on the bottom, the portraits plus the
inventory at 1.4x so the slots are finger-sized.

**Everywhere else** (title, entrance, menus, champion mirror, freeze, sleep, save menu)

| Screen | What it shows |
|---|---|
| **Top** | The whole game screen at 1.2x, centred. |
| **Bottom** | The whole game screen at 1:1, so every tap lands exactly where a mouse click would on the Atari ST, and the inventory is full size. |

## Controls

| Input | Action |
|---|---|
| D-pad / Circle Pad | Step forward/back, turn left/right (hold to keep going) |
| **L** / **R** | Strafe left / right |
| **A / B / X / Y** | Champion 1 / 2 / 3 / 4 attacks (first attack) in one press; hold to keep attacking whenever the weapon is ready |
| **ZL** + A/B/X/Y | That champion's second attack |
| **ZR** + A/B/X/Y | That champion's third attack |
| **RECAST** button | Repeat the last spell (shows the caster's colour and rune count) |
| **SELECT** or **BAG** button | Open / close the leader's inventory |
| Tap | Left click on whatever is under your finger |
| **ZR** held + tap | Right click |
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
3. Launch **Dungeon Master** from the Homebrew Launcher.

**Game data.** No original game data is included. You need GRAPHICS.DAT and DUNGEON.DAT from a copy of the game you own.

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
| `csb.cfg` | Upstream config with the TrueType overlay off and native window size |

## Licence

The files in this repository (the 3DS layer, patch script and build scripts) are MIT
licensed; see `LICENSE`. They contain no CSBwin source: the build clones CSBwin at a
pinned commit and patches it locally.

CSBwin is (c) Paul R. Stevens and contributors (Linux/SDL2 maintenance by Emmanuel Anne,
github.com/zelurker/CSB). Dungeon Master and Chaos Strikes Back are (c) FTL Games /
Software Heaven. No game data is included or distributed.
