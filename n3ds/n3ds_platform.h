// csb-3ds: Nintendo 3DS platform layer for CSBwin (Dungeon Master / Chaos Strikes Back)
//
// The CSBwin engine draws the original 320x200 Atari ST screen into an RGB565
// buffer. This layer takes that buffer and splits it across the two screens:
//
//   In the dungeon:
//     Top    (400x240): viewport at 1.5x + message strip + party vitals in the margins,
//                       stereo depth from the 3D slider.
//     Bottom (320x240): controls only - portraits 1:1, spells/attacks/arrows at 1.5x,
//                       a small copy of the viewport, and RECAST / BAG buttons.
//   Inventory open:  top = whole screen 1.2x; bottom = portraits + inventory at 1.4x.
//   Everywhere else (title, entrance, menus, champion mirror, ...):
//     Top: the whole screen at 1.2x, centred.  Bottom: the whole screen at 1:1.
//
// Input is translated into the same CSBUI messages the SDL/Linux front end sends.
#pragma once

#ifdef __3DS__

#ifdef __cplusplus
extern "C++" {
#endif

// The engine renders into this (RGB565, 320x200, pitch 640 bytes).
extern unsigned short n3ds_st_buffer[320 * 200];

// Platform lifecycle -------------------------------------------------------
void n3ds_init(void);       // call first thing in main(): gfx, speedup, chdir to data folder
void n3ds_present(void);    // call where the SDL build would present a frame
bool n3ds_poll(void);       // call once per main-loop pass; returns false when the user quits
void n3ds_shutdown(void);

// Blocking message box on the bottom screen. Returns true for A (OK / Yes), false for B (No).
bool n3ds_message_box(const char *msg, const char *title, bool yesno);

// Implemented in the patched CSBlinux.cpp (they need its static state) ------
void n3ds_send_key(int sym, int scancode);
void n3ds_send_mouse_move(int x, int y);
void n3ds_send_mouse_button(bool right, bool down, int x, int y);
void n3ds_force_redraw(void);
bool n3ds_engine_in_dungeon_view(void);   // picks the dungeon layout vs. the full-screen one
bool n3ds_engine_inventory_open(void);
bool n3ds_engine_candidate_shown(void);   // Hall of Champions mirror (resurrect/reincarnate)
int  n3ds_engine_party_size(void);
int  n3ds_engine_attack_menu_owner(void); // 0 = none, else champion ordinal (1..4)
bool n3ds_engine_cursor_is_arrow(void);    // false while an item is held (cursor shows it)
void n3ds_engine_champion_stats(int i, int out[6]);   // hp,maxHp,stamina,maxSta,mana,maxMana
bool n3ds_engine_button_bounds(int *x0, int *y0, int *x1, int *y1);   // ST coords, inclusive
void n3ds_engine_champion_extra(int i, int out[3]);   // food, water, poisonCount
const char *n3ds_engine_champion_name(int i);
int  n3ds_engine_char_color(int i);        // the champion's message colour index
int  n3ds_engine_light_level(void);        // engine palette 0 (bright) .. 5 (dark)
void n3ds_engine_party(int out[4]);        // level, x, y, facing
void n3ds_engine_delta(int facing, int *dx, int *dy);
bool n3ds_engine_level_size(int level, int *w, int *h);
int  n3ds_engine_cell(int level, int x, int y);   // CELLFLAG (type in bits 5-7), -1 outside

// Called from the patched CSBCode.cpp for every message the game prints (log).
void n3ds_note_message(int color, const char *text);

// Called from the patched Magic.cpp each time a spell is cast (RECAST button).
void n3ds_note_spell(int caster, const signed char *incantation);

#ifdef __cplusplus
}
#endif

#endif // __3DS__
