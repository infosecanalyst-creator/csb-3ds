// csb-3ds: Nintendo 3DS platform layer for CSBwin (Dungeon Master / Chaos Strikes Back)
//
// The CSBwin engine draws the original 320x200 Atari ST screen into an RGB565
// buffer. This layer takes that buffer and splits it across the two screens:
//
//   In the dungeon:
//     Top    (400x240): viewport at 1.5x + message strip, stereo depth from the 3D slider.
//     Bottom (320x240): controls only - portraits 1:1, spells/attacks/arrows at 1.5x,
//                       and a small copy of the viewport for grabbing items and buttons.
//   Everywhere else (title, entrance, menus, inventory):
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

#ifdef __cplusplus
}
#endif

#endif // __3DS__
