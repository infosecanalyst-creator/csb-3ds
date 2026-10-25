# csb-3ds UX audit

Goal: the information the player needs on screen at all times, and the fewest taps
or presses for the things players do most. Audited against three reference points:

- **Stylus games on DS/3DS**: Etrian Odyssey (the closest genre match: first-person
  grid dungeon, D-pad movement, touch for secondary actions), Advance Wars DS, Fire
  Emblem, Civilization Revolution DS, Zelda: Phantom Hourglass.
- **Mouse-driven PC games**: Dungeon Master itself, Eye of the Beholder, and Legend of
  Grimrock, the modern take on Dungeon Master.
- **Modern conventions**: one action per input for the most frequent actions, key
  status always in view, touch targets sized for fingers, no mode toggles the
  player has to manage, and no input that can accidentally do something destructive.

## How players spend their inputs in Dungeon Master

In rough order of frequency: moving and turning, attacking (constantly in fights),
casting (constantly for casters), watching party health/stamina/mana, picking up and
using items, managing inventory (eating, drinking, swapping weapons), and rarely
saving, sleeping or recruiting champions.

## Findings and changes

| # | Action | Before | After | Reference |
|---|---|---|---|---|
| 1 | Move / turn / strafe | D-pad + L/R, 1 press | unchanged | Etrian Odyssey and Grimrock use the same scheme |
| 2 | Attack with a champion | tap hand, tap attack (2 taps), or face button twice | **1 press**: A/B/X/Y = champion 1-4 first attack; ZL+button = second, ZR+button = third. Closes another champion's open menu automatically | Grimrock: one right-click on the weapon attacks |
| 3 | Re-cast a spell | caster tab + up to 4 runes + cast (up to 6 taps) | **1 tap** on RECAST (shows caster colour and rune count) | Spell hotbars in modern RPGs; Grimrock spell runes stay put |
| 4 | Party health / stamina / mana | only on the bottom screen while you look at the top one | **always on the top screen**: three bars per champion in the side margins, health turns red below 25% | Every stylus game keeps vitals on the screen you look at (EO keeps HP/TP on the top screen) |
| 5 | Open leader's inventory | tap the leader's portrait | SELECT or BAG button, or portrait (all 1 input) | DM's own right-click convention |
| 6 | Inventory slots | 16 px at 1:1 (stylus only) | **1.4x** (22 px): portraits plus inventory fill the bottom screen | Nintendo guideline: touch targets sized for fingers; DS games scale inventories up to the full bottom screen |
| 7 | Spell runes, attack menu, arrows | 1:1 in a crowded panel | 1.5x on the bottom screen in the dungeon | same |
| 8 | Grab items, press wall buttons | tap in the full screen | framed mini viewport on the bottom; the big view stays on top | Phantom Hourglass: look at the top, act on the bottom |
| 9 | Menus, entrance, champion mirror | duplicated crop on top | whole screen centred on top, exact 1:1 touch on the bottom | |
| 10 | 3D depth | slider + SELECT toggle | slider only (sliding it down turns depth off); SELECT freed for inventory | Platform convention |
| 11 | Accidental input | face buttons replayed clicks on every screen | shortcuts that replay clicks only run in the dungeon view, so they can't hit RESUME or menu buttons by accident | |
| 12 | Quit | START+SELECT asks first | unchanged | |
| 13 | Sustained melee | press again every time the weapon recovers | **hold** A/B/X/Y: the attack is re-issued every 0.6 s and lands as soon as the weapon is ready | Auto-attack in modern action RPGs |
| 14 | Cursor after a tap in the mini view | arrow stays in the big view on top | plain arrow parks in the empty end of the portrait strip (bottom only); a held item still follows your finger | |
| 15 | Frame cost | divisions and float math per pixel | lookup tables; same pixels, about 3.3x faster to draw | |

Typical fight, four champions, two rounds:
**before** 16 taps (2 per champion attack); **after** 8 presses, with the party's
health in view the whole time.
Healing loop (caster re-casts a 3-rune heal): **before** 5 taps; **after** 1.

## Controls after this pass

| Input | In the dungeon | Elsewhere |
|---|---|---|
| D-pad / Circle Pad | step, turn | step, turn |
| L / R | strafe | strafe |
| A / B / X / Y | champion 1/2/3/4 first attack (hold to repeat) | — |
| ZL + A/B/X/Y | second attack | — |
| ZR + A/B/X/Y | third attack | — |
| ZR + tap | right click | right click |
| SELECT | leader's inventory (open / close) | closes the inventory |
| START | freeze | freeze |
| START + SELECT | quit (asks first) | quit |
| RECAST (touch) | repeat the last spell | — |
| BAG (touch) | leader's inventory | — |

## Kept on purpose

- **The game's cursor.** When you pick up an item the cursor becomes that item's
  picture, and that's how Dungeon Master shows what's in your hand. Hiding the plain
  arrow safely would mean rewriting how the engine draws to the screen.
- **Movement keys repeat while held**, as in the original's keyboard play.

## Still open (needs hardware or play-testing)

1. Finger feel of the 1.5x runes (about 5.5 mm wide). If they are hard to hit, the
   rune row can be laid out at 2x in the RECAST/BAG area.
2. Whether RECAST should remember a spell per champion (four small buttons) instead
   of only the most recent one.
3. Champion mirror (resurrect / reincarnate) and the save menu use the full-screen
   layout at 1:1; check they are comfortable with a finger.
4. Stereo depth strength and the vanishing-point values on real hardware.
