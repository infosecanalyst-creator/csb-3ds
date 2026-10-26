#!/usr/bin/env python3
"""Apply the csb-3ds changes to a CSBwin source tree (github.com/zelurker/CSB).

Usage:  python3 patch_csb_3ds.py path/to/CSB/src

Every edit is anchored to exact upstream text and must match exactly once;
if upstream changes, the script stops and tells you which anchor failed
instead of silently producing a broken tree. All changes are wrapped in
#ifdef __3DS__ so the Linux/Windows builds are unaffected.
Re-running on an already patched tree is a no-op.
"""
import pathlib
import sys

MARK = "/* csb-3ds patched */"


def edit(text, name, old, new, count=1):
    found = text.count(old)
    if found != count:
        sys.exit(f"[csb-3ds] {name}: anchor matched {found} times (expected {count}); "
                 f"upstream source has changed - update patch_csb_3ds.py")
    return text.replace(old, new)


def patch_csblinux(t):
    t = edit(t, "CSBlinux TDescKeys alignment", DESCKEYS_DEF,
             PACK_OPEN + DESCKEYS_DEF + PACK_CLOSE)
    t = edit(t, "CSBlinux include", '#include "Data.h"\n',
             '#include "Data.h"\n#ifdef __3DS__\n#include "n3ds_platform.h"\n#endif\n')

    t = edit(t, "CSBlinux main entry",
             "int main(int argc, char* argv[])\n{\n",
             "int main(int argc, char* argv[])\n{\n#ifdef __3DS__\n  n3ds_init();\n#endif\n")

    t = edit(t, "CSBlinux SDL_Init",
             "  if ( SDL_Init ( SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER|SDL_INIT_EVENTS)<0)",
             "#ifdef __3DS__\n"
             "  // No SDL video on 3DS: n3ds_platform owns both screens.\n"
             "  if ( SDL_Init ( SDL_INIT_AUDIO|SDL_INIT_TIMER|SDL_INIT_EVENTS)<0)\n"
             "#else\n"
             "  if ( SDL_Init ( SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER|SDL_INIT_EVENTS)<0)\n"
             "#endif")

    # Skip window / renderer / texture / ImGui / font setup.
    t = edit(t, "CSBlinux display guard start",
             "#if defined SDL20\n  SDL_SetWindowTitle(sdlWindow,szCSBVersion);",
             "#ifndef __3DS__\n#if defined SDL20\n  SDL_SetWindowTitle(sdlWindow,szCSBVersion);")
    t = edit(t, "CSBlinux display guard end",
             "  UI_Initialize_sounds();\n",
             "#else\n  screenSize = 1;            // 1:1 ST coordinates\n"
             "  cursorIsShowing = false;\n#endif // __3DS__\n  UI_Initialize_sounds();\n")

    # Main loop: poll 3DS input every pass, and never block forever waiting for SDL events.
    t = edit(t, "CSBlinux main loop poll",
             "    if (SDL_PollEvent(&evert) == 0)\n    {\n\tSDL_SemWait(sem);",
             "#ifdef __3DS__\n    if (!n3ds_poll()) cbAppDestroy();\n#endif\n"
             "    if (SDL_PollEvent(&evert) == 0)\n    {\n\tSDL_SemWait(sem);")
    t = edit(t, "CSBlinux wait event",
             "      if(SDL_WaitEvent(&evert) == 0) {",
             "#ifdef __3DS__\n      if(SDL_WaitEventTimeout(&evert, 8) == 0) {\n"
             "\t  continue; // timeout: poll 3DS input again\n#else\n"
             "      if(SDL_WaitEvent(&evert) == 0) {\n#endif")
    t = edit(t, "CSBlinux imgui event",
             "    ImGui_ImplSDL2_ProcessEvent(&evert);\n    if (imgui_active) {",
             "#ifndef __3DS__\n    ImGui_ImplSDL2_ProcessEvent(&evert);\n#endif\n    if (imgui_active) {")

    # Present: hand the frame to the 3DS layer instead of ImGui + SDL_Renderer.
    t = edit(t, "CSBlinux post_render",
             "void post_render() {\n    static bool was_active;\n    drawn = 1;\n",
             "void post_render() {\n    static bool was_active;\n    drawn = 1;\n"
             "#ifdef __3DS__\n    n3ds_present();\n    return;\n#endif\n")

    t = edit(t, "CSBlinux done_imgui",
             "void done_imgui() {\n",
             "void done_imgui() {\n#ifdef __3DS__\n    return;\n#endif\n")
    t = edit(t, "CSBlinux shutdown",
             "  gtk_exit (0);\n#endif //USE_OLD_GTK\n    exit(0);\n}",
             "  gtk_exit (0);\n#endif //USE_OLD_GTK\n#ifdef __3DS__\n    n3ds_shutdown();\n#endif\n"
             "    exit(0);\n}")

    t += r'''
#ifdef __3DS__
// ---- csb-3ds input bridge: reuse the SDL front end's own handlers ----
extern void ForceScreenDraw(void);

void n3ds_send_key(int sym, int scancode)
{
  memset(&evert, 0, sizeof(evert));
  evert.type = SDL_KEYDOWN;
  evert.key.keysym.sym = (SDL_Keycode)sym;
  evert.key.keysym.scancode = (SDL_Scancode)scancode;
  Process_SDL_KEYDOWN();
}

// A stylus "teleports" the cursor and clicks in the same instant. The engine only
// copies absMouseX/Y into its own cursor (d.NewMouseX/Y) once per VBL, and button
// lists (menus, the dungeon UI) hit-test against that copy. So update the engine's
// cursor immediately, as the source's own Nokia 770 stylus path does via checkVBL().
static void n3ds_sync_engine_cursor(int x, int y)
{
  absMouseX = x;   // screenSize is 1, so these are ST coordinates
  absMouseY = y;
  d.NewMouseX = (i16)x;
  d.NewMouseY = (i16)y;
  d.CurMouseX = (i16)x;
  d.CurMouseY = (i16)y;
}

void n3ds_send_mouse_move(int x, int y)
{
  n3ds_sync_engine_cursor(x, y);
}

void n3ds_send_mouse_button(bool right, bool down, int x, int y)
{
  n3ds_sync_engine_cursor(x, y);
  memset(&evert, 0, sizeof(evert));
  evert.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  evert.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
  evert.button.x = x;
  evert.button.y = y;
  if (down) Process_SDL_MOUSEBUTTONDOWN(); else Process_SDL_MOUSEBUTTONUP();
}

void n3ds_force_redraw(void)
{
  ForceScreenDraw();   // repaint every region on the next display()
}

// True while the party is looking at the dungeon: the engine's normal-play button
// lists are active (characters/spells/movement + viewport) and no inventory is open.
// Entrance, menus, freeze, sleep, disk menu, inventory and game over all swap them.
bool n3ds_engine_in_dungeon_view(void)
{
  return d.PrimaryButtonList == &d.Buttons17196[0]
      && d.SecondaryButtonList == &d.Buttons17304[0]
      && d.SelectedCharacterOrdinal == 0;
}

// Read-only engine state used by the 3DS layouts and shortcuts.
bool n3ds_engine_inventory_open(void)  { return d.SelectedCharacterOrdinal != 0; }
bool n3ds_engine_candidate_shown(void) { return d.PotentialCharacterOrdinal != 0; } // Hall of Champions mirror
int  n3ds_engine_party_size(void)      { return d.NumCharacter < 0 ? 0 : (d.NumCharacter > 4 ? 4 : d.NumCharacter); }
int  n3ds_engine_attack_menu_owner(void) { return d.AttackingCharacterOrdinal; }
bool n3ds_engine_cursor_is_arrow(void)  { return d.CursorShape == 0; }
// Bounding box of every clickable button the engine is currently hit-testing
// (menus, entrance, mirror, freeze...). Used to zoom the bottom screen so those
// buttons are finger-sized. Returns false when there are no buttons.
bool n3ds_engine_button_bounds(int *x0, int *y0, int *x1, int *y1)
{
  int ax = 9999, ay = 9999, bx = -1, by = -1;
  btn *lists[2] = { d.PrimaryButtonList, d.SecondaryButtonList };
  for (int l = 0; l < 2; l++) {
    btn *b = lists[l];
    for (int k = 0; b && k < 64 && b[k].word0 != 0; k++) {
      if (b[k].xMax < b[k].xMin || b[k].yMax < b[k].yMin) continue;
      if (b[k].xMin < ax) ax = b[k].xMin;
      if (b[k].yMin < ay) ay = b[k].yMin;
      if (b[k].xMax > bx) bx = b[k].xMax;
      if (b[k].yMax > by) by = b[k].yMax;
    }
  }
  if (bx < 0) return false;
  *x0 = ax; *y0 = ay; *x1 = bx; *y1 = by;
  return true;
}
void n3ds_engine_champion_stats(int i, int out[6])
{
  CHARDESC &c = d.hero[i];
  out[0] = c.HP();      out[1] = c.MaxHP();
  out[2] = c.Stamina(); out[3] = c.MaxStamina();
  out[4] = c.Mana();    out[5] = c.MaxMana();
}
#endif // __3DS__
'''
    return t


def patch_lincsbui(t):
    t = edit(t, "LinCSBUI include", '#include "UI.h"\n',
             '#include "UI.h"\n#ifdef __3DS__\n#include "n3ds_platform.h"\n#endif\n')
    t = edit(t, "LinCSBUI message box",
             "i32 UI_MessageBox(const char *msg, const char *title, i32 flags ) {\n",
             "i32 UI_MessageBox(const char *msg, const char *title, i32 flags ) {\n"
             "#ifdef __3DS__\n"
             "    {\n"
             "      bool r = n3ds_message_box(msg, title, flags == MESSAGE_YESNO);\n"
             "      if (flags == MESSAGE_YESNO) return r ? MESSAGE_IDYES : MESSAGE_IDNO;\n"
             "      return MESSAGE_IDOK;\n"
             "    }\n"
             "#endif\n")
    t = edit(t, "LinCSBUI start updates",
             "void UI_ScreenStartUpdates(void)\n{\n",
             "void UI_ScreenStartUpdates(void)\n{\n"
             "#ifdef __3DS__\n  dstPixels = n3ds_st_buffer;\n  pitch = 320 * 2;\n  return;\n#endif\n")
    t = edit(t, "LinCSBUI end updates",
             "void UI_ScreenEndUpdates(void)\n{\n",
             "void UI_ScreenEndUpdates(void)\n{\n#ifdef __3DS__\n  return;\n#endif\n")
    t = edit(t, "LinCSBUI blit guard",
             "  if (sdlTexture == NULL) return;\n//  textureRect.x = 0;",
             "#ifndef __3DS__\n  if (sdlTexture == NULL) return;\n#endif\n//  textureRect.x = 0;")
    return t


def patch_soundmixer(t):
    t = edit(t, "SoundMixer open error detail",
             '\tprintf("Audio: opening device failed!\\n");\n',
             '\tprintf("Audio: opening device failed! (%s)\\n", SDL_GetError());\n')
    # The 3DS SDL audio backend may not report a "default device"; fall back to sane values.
    return edit(t, "SoundMixer default audio",
                '\tprintf("SDL_GetDefaultAudioInfo failed!\\n");\n\texit(1);\n',
                '\tprintf("SDL_GetDefaultAudioInfo failed!\\n");\n'
                '#ifdef __3DS__\n'
                '\tmixer.freq = 22050; mixer.channels = 2; mixer.samples = 1024;\n'
                '\tSoundCard = (char *)"ndsp";\n'
                '#else\n\texit(1);\n#endif\n')


def patch_csbtypes(t):
    # On arm-none-eabi, int32_t is 'long', so i32& can't bind to the plain ints the
    # engine passes around (it assumes glibc, where int32_t is 'int').
    return edit(t, "CSBTypes i32",
                "  typedef int32_t i32;  // signed 32-bit integer\n  typedef uint32_t ui32;\n",
                "#ifdef __3DS__\n  typedef int i32;\n  typedef unsigned int ui32;\n#else\n"
                "  typedef int32_t i32;  // signed 32-bit integer\n  typedef uint32_t ui32;\n#endif\n")


PACK_OPEN = "#ifdef __3DS__\n#pragma pack(push, 4)  // pointers must be 4-byte aligned for 3dsxtool\n#endif\n"
PACK_CLOSE = "#ifdef __3DS__\n#pragma pack(pop)\n#endif\n"

# The engine compiles under a global #pragma pack(1) (needed for its file formats).
# A few in-memory UI tables that are statically initialised with pointers then end
# up with pointers at odd offsets, and 3dsxtool rejects those ("Unaligned relocation!").
# These structs are never read from or written to disk, so normal alignment is safe.
S12406_DEF = ("struct S12406\n{\n  i8        byte0;\n  i8        byte1;\n"
              "  DlgButton *pDlgButton2; // An array of DlgButton\n"
              "  pnt       pnt6;\n  pnt       pnt10;\n};\n")
S20_DEF = ("struct S20\n{\n  i16  w0;\n  i8   b2;\n  i8   b3;\n  wordRectPos rectPos4;\n"
           "  i16  w12;\n  const char*  pnt14;\n  i16  w18;\n};\n")
DESCKEYS_DEF = ("typedef struct {\n    const char *desc;\n    int key;\n    int key1, key2;\n"
                "    bool active1,active2;\n} TDescKeys;\n")


def patch_chaos(t):
    t = edit(t, "Chaos S12406 alignment", S12406_DEF, PACK_OPEN + S12406_DEF + PACK_CLOSE)
    # Debug tracing of the engine's click queue (goes to csb3ds.log on 3DS).
    t = edit(t, "Chaos EnqueMouseClick trace entry",
             "  D7W = sw(P1);\n  D6W = sw(P2);\n  if (e.Byte9212 != 0)\n",
             "  D7W = sw(P1);\n  D6W = sw(P2);\n"
             "#ifdef __3DS__\n"
             "  printf(\"[eng] EnqueMouseClick x=%d y=%d btn=%d busy=%d first=%d last=%d\\n\",\n"
             "         (int)P1, (int)P2, (int)P3, (int)e.Byte9212, (int)e.FirstQueuedMouseEvent,\n"
             "         (int)e.LastQueuedMouseEvent);\n"
             "  for (int dbg = 0; dbg < 6; dbg++)\n"
             "    if (ppS12406_9142[dbg])\n"
             "      printf(\"[eng]   dialog[%d] byte1=%d buttons=%p\\n\", dbg,\n"
             "             (int)ppS12406_9142[dbg]->byte1, (void *)ppS12406_9142[dbg]->pDlgButton2);\n"
             "#endif\n"
             "  if (e.Byte9212 != 0)\n")
    t = edit(t, "Chaos EnqueMouseClick trace hit",
             "    D4W = TAG002cdc((*pS12406A4)->pDlgButton2, D7W, D6W, P3);\n",
             "    D4W = TAG002cdc((*pS12406A4)->pDlgButton2, D7W, D6W, P3);\n"
             "#ifdef __3DS__\n"
             "    printf(\"[eng]   hit-test dialog %d -> %d\\n\", (int)w_2, (int)D4W);\n"
             "#endif\n")
    return t


TEXT_DEF = ("struct TEXT\n{\n  i16  w0;\n  const char *pnt2;\n  wordRectPos rectPos6;\n"
            "  i16  w14;\n  i16  w16;\n  i16  w18;\n  i16  w20;\n};\n")
CLICKMAP_DEF = ("struct CLICKMAP\n{\n  i8   b0;\n  i8   b1;\n  T12  *pt12_2;\n"
                "  pnt  *pPnt6;\n  pnt  p10;\n};\n")


def patch_magic(t):
    # RECAST: hand each cast incantation to the 3DS layer before the engine clears it.
    t = edit(t, "Magic include", '#include "stdafx.h"\n',
             '#include "stdafx.h"\n#ifdef __3DS__\n#include "n3ds_platform.h"\n#endif\n')
    return edit(t, "Magic spell hook",
                "  FillSpellParameters(d.MagicCaster, spellParameters);\n",
                "  FillSpellParameters(d.MagicCaster, spellParameters);\n"
                "#ifdef __3DS__\n  n3ds_note_spell(d.MagicCaster, pcA3->incantation);\n#endif\n")


def patch_csbui(t):
    # Slow loading on hardware: the engine reads its data in small pieces and calls
    # fseek(0, SEEK_CUR) before every read, which throws away stdio's buffer, so each
    # read became a separate SD card access. Read-only files are served from RAM.
    return edit(t, "CSBUI fopen",
                "FILE *UI_fopen(const char *name, const char *flags)\n{\n  return fopen(name, flags);",
                "#ifdef __3DS__\nFILE *n3ds_fopen(const char *name, const char *flags);\n#endif\n"
                "FILE *UI_fopen(const char *name, const char *flags)\n{\n"
                "#ifdef __3DS__\n  return n3ds_fopen(name, flags);\n#endif\n  return fopen(name, flags);")


def patch_hint(t):
    for name, d in (("S20", S20_DEF), ("TEXT", TEXT_DEF), ("CLICKMAP", CLICKMAP_DEF)):
        t = edit(t, f"Hint {name} alignment", d, PACK_OPEN + d + PACK_CLOSE)
    return t


PATCHES = {
    "CSBTypes.h": patch_csbtypes,
    "Chaos.cpp": patch_chaos,
    "Hint.cpp": patch_hint,
    "Magic.cpp": patch_magic,
    "CSBlinux.cpp": patch_csblinux,
    "LinCSBUI.cpp": patch_lincsbui,
    "SoundMixer.cpp": patch_soundmixer,
    "CSBUI.cpp": patch_csbui,
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = pathlib.Path(sys.argv[1])
    for name, fn in PATCHES.items():
        path = src / name
        text = path.read_text(encoding="latin-1")
        if MARK in text:
            print(f"[csb-3ds] {name}: already patched")
            continue
        path.write_text(MARK + "\n" + fn(text), encoding="latin-1")
        print(f"[csb-3ds] {name}: patched")


if __name__ == "__main__":
    main()
