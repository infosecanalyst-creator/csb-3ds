# Changelog

## 2026-11-21

- Tested with a party for the first time (emulator): recruit a champion, save, exit, start again, RESUME, load, play on. It works; these fixes came out of it.
- START > SAVE now has its own slot, 3 (it was slot 4, which EXIT GAME also writes: the exit save replaced it). The button reads SAVE (SLOT 3). Keep slots 1 and 2 for the disk icon.
- After START > SAVE the top screen says GAME SAVED TO SLOT 3 (it said nothing), or SAVE FAILED; after a failed save SAVE can be tried again.
- Loading a save: the screen after it offers only PLAY GAME. The engine's debugging button ASCII DUMP is gone (it sat where the save's button was, so a second tap pressed it).
- START menu: the hints name the real buttons in left-hand mode; the greyed SAVE says why ("SAVE needs a party: tap a mirror first."); FREEZE is explained; the exit question is asked once, on the touch screen.
- FREEZE: the top screen says "Game frozen. Tap the touch screen to go on." instead of the title's help card.
- Error and question boxes are drawn in the house style with buttons to tap (OK, or YES / NO with NO in focus) instead of a text console.
- RECAST: a WHO button above the tab opens the champion list with a tap (it needed a long press).
- Manual: 12 px line pitch (was 10); the left-hand footer has margins; the map chapter says the map is kept with each save; how to come back to a saved game.
- Manual, left-hand mode: the footer line's last word (CLOSE) is no longer cut off.
- New SAVE (SLOT 4) button in the START menu: saves to slot 4 and the game goes on. It is greyed out where the game cannot be saved safely, and the menu explains slot 4.
- The game's text output is restored after a message box (a failed save's message no longer breaks later output).
- Quieter touch click.

## 2026-11-20

- Saving is safer: a save that fails on close is reported as failed, and the auto-map and the settings file are written to a temporary file first.
- HOME > Close saves to slot 4 like EXIT GAME; the game pauses cleanly on HOME or when the lid closes.
- If the exit save failed, the next EXIT GAME asks "Not saved (the save failed). Exit anyway?" instead of quitting.
- Install path: the program is /3ds/csb3ds.3dsx. The START menu shows the build version.
- README controls match the game.

## 2026-11-19

- EXIT GAME saves to slot 4 (Csbgame4) first when the party is in the dungeon.
- Manual: drag the stylus to scroll the text, with a little momentum; section buttons act on release.
