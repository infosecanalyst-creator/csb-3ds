# Emulator tests with a party

Scripts for the emulator lane (3ds-test-suite, tools/emu). They need Dungeon Master's DUNGEON.DAT
and GRAPHICS.DAT in the csb3ds folder (bring your own game files; nothing of the game is in this
repository).

Format: the lane's `.src` (`t X Y [shot]` tap, `p BTN [shot]` press, other lines as they are).
Presses are 14 frames with 60 frames between them: shorter ones are sometimes missed while the
dungeon view redraws. The first press needs `wait 2000` after ENTER (the gate takes that long).

- `csb-party-1-save-exit.src`: walks from the start to the first mirror, RESURRECT, START > SAVE
  (slot 3; the top screen must say GAME SAVED TO SLOT 3), a second champion from the mirror one
  square north, EXIT GAME > YES. After it the card holds csbgame3.dat and Csbgame4.dat, each with
  its .map file. Run it with KEEPSD=1 and copy the four files for the next two.
- `csb-party-2-resume.src`: RESUME at the gate, slot 3, PLAY GAME (the only button), SPELL panel, BAG.
- `csb-party-3-spell-who.src`: loads slot 4 (two champions), casts a spell, then WHO > champion, RECAST.
- `csb-menu-lefty-freeze.src`: START menu without a party (why SAVE is grey), left-hand hints, FREEZE card, manual footer.
- `csb-no-data-message.src`: run with DUNGEON.DAT removed: the house-style message box.

The way to the first mirror in Dungeon Master (start at x 1, y 3, facing south; D-pad Up = step,
Left / Right = turn): 4 steps, left, 3 steps, left, 5 steps, right, 2 steps, right, 1 step, left,
2 steps, right, 1 step, left, 2 steps, right, 1 step. The mirror is then straight ahead (the wall at
x 10, y 6). `dungeon_map.py` prints a level's map and where the mirrors (actuator type 127) are.
