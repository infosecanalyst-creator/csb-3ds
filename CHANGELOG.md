# Changelog

## 2026-11-21

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
