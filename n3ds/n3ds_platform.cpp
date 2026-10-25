// csb-3ds: Nintendo 3DS platform layer for CSBwin. See n3ds_platform.h for the overview.
#ifdef __3DS__

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include "n3ds_platform.h"

#define DATA_DIR "sdmc:/3ds/csb3ds"

unsigned short n3ds_st_buffer[320 * 200];

// Debug mode: create an empty file named debug.txt next to csb3ds.3dsx to get
// csb3ds.log (engine output + input trace) and a crosshair where each tap lands.
static bool dbgEnabled = false;
#define DBG(...) do { if (dbgEnabled) printf(__VA_ARGS__); } while (0)
static unsigned dbgPolls = 0, dbgPresents = 0;
static int dbgTX = -1, dbgTY = -1;   // last tap, bottom-screen pixels

// ---------------------------------------------------------------------------
// Screen geometry
// ---------------------------------------------------------------------------
// The engine's own region split of the 320x200 ST screen (see WinScreen.cpp display()):
//   portraits y 0..32 | viewport x 0..223, y 33..168 | controls x 224..319, y 33..168
//   | message text y 169..199
enum {
  ST_W = 320, ST_H = 200,
  VP_X = 0, VP_Y = 0x21, VP_W = 0xe0, VP_H = 0xa9 - 0x21,   // 224 x 136
  CTL_X = 0xe0, CTL_W = 320 - 0xe0,                          // 96 x 136 (spells, actions, arrows)
  PORT_H = 0x21,                                             // 33 rows of champion status
  TXT_Y = 0xa9, TXT_H = 0xc8 - 0xa9,                         // 31 rows of messages
  TOP_W = 400, TOP_H = 240, BOT_W = 320, BOT_H = 240,
};

// A rectangle of the ST screen drawn (nearest-neighbour scaled) into a rectangle
// of a 3DS screen. Touch input is mapped back through the same rectangles.
struct Region { int sx, sy, sw, sh, dx, dy, dw, dh; };

// --- Top screen -----------------------------------------------------------
// Dungeon: viewport at 1.5x (336x204) with the message strip underneath.
static const Region TOP_DUNGEON_VP  = { VP_X, VP_Y, VP_W, VP_H, (TOP_W - 336) / 2, 0, 336, 204 };
static const Region TOP_DUNGEON_TXT = { 0, TXT_Y, ST_W, TXT_H, (TOP_W - 320) / 2, 207, 320, TXT_H };
// Everything else (title, entrance, menus, inventory): whole screen at 1.2x, centred.
static const Region TOP_FULL        = { 0, 0, ST_W, ST_H, (TOP_W - 384) / 2, 0, 384, 240 };

// --- Bottom screen --------------------------------------------------------
// Dungeon: controls only, sized for fingers.
//   champion portraits/hands at 1:1 across the top,
//   spell runes + attack menu + movement arrows at 1.5x on the right,
//   a small copy of the viewport on the left for grabbing items, wall buttons, doors.
static const Region BOT_DUNGEON[] = {
  { 0,     0,    ST_W,  PORT_H, 0,   0,  320, PORT_H },        // portraits
  { CTL_X, VP_Y, CTL_W, VP_H,   176, 35, 144, 204    },        // controls 1.5x
  { VP_X,  VP_Y, VP_W,  VP_H,   0,   84, 176, 107    },        // mini viewport (0.79x)
};
// Everything else: the whole screen at 1:1 (exact mouse mapping, inventory at full size).
static const Region BOT_FULL[] = {
  { 0, 0, ST_W, ST_H, 0, (BOT_H - ST_H) / 2, ST_W, ST_H },
};

enum Layout { LAYOUT_FULL = 0, LAYOUT_DUNGEON = 1 };
static Layout layout = LAYOUT_FULL;

// Stereoscopic depth -------------------------------------------------------
// DM's viewport is a one-point perspective: walls at each distance form nested
// rectangles around the vanishing point. A rectangular (Chebyshev) distance from
// that point is a decent depth estimate for corridors and walls: the centre is
// far away (pushed into the screen), the edges sit at the screen plane.
static const float VP_HORIZON_X = 112.0f;  // tune on hardware if the corridor looks bent
static const float VP_HORIZON_Y = 68.0f;
static const float MAX_DISPARITY = 7.0f;   // top-screen pixels at full slider
static unsigned char depthMap[VP_H][VP_W];  // 0 = screen plane .. 255 = farthest
static bool depthEnabled = true;

static void buildDepthMap(void)
{
  for (int y = 0; y < VP_H; y++)
    for (int x = 0; x < VP_W; x++) {
      float dx = fabsf(x - VP_HORIZON_X) / VP_HORIZON_X;
      float dy = fabsf(y - VP_HORIZON_Y) / VP_HORIZON_Y;
      float d = dx > dy ? dx : dy;
      if (d > 1.0f) d = 1.0f;
      depthMap[y][x] = (unsigned char)((1.0f - d) * 255.0f);
    }
}

// Framebuffers are rotated: pixel (x, y) lives at x*240 + (239 - y).
static inline void putpx(u16 *fb, int x, int y, u16 c) { fb[x * 240 + (239 - y)] = c; }

static void blitRegion(u16 *fb, const Region &r)
{
  for (int y = 0; y < r.dh; y++) {
    const u16 *srow = n3ds_st_buffer + (r.sy + y * r.sh / r.dh) * ST_W + r.sx;
    for (int x = 0; x < r.dw; x++) putpx(fb, r.dx + x, r.dy + y, srow[x * r.sw / r.dw]);
  }
}

static void frameRegion(u16 *fb, const Region &r, u16 c)   // 1px border just outside
{
  for (int x = r.dx - 1; x <= r.dx + r.dw; x++) {
    if (x < 0 || x >= BOT_W) continue;
    if (r.dy - 1 >= 0) putpx(fb, x, r.dy - 1, c);
    if (r.dy + r.dh < BOT_H) putpx(fb, x, r.dy + r.dh, c);
  }
  for (int y = r.dy; y < r.dy + r.dh; y++) {
    if (r.dx - 1 >= 0) putpx(fb, r.dx - 1, y, c);
    if (r.dx + r.dw < BOT_W) putpx(fb, r.dx + r.dw, y, c);
  }
}

// Viewport at 1.5x with per-pixel horizontal disparity from the depth map.
static void blitViewportStereo(u16 *fb, const Region &r, float eye, float slider)
{
  const float k = (depthEnabled ? slider : 0.0f) * MAX_DISPARITY * 0.5f / 255.0f;
  for (int y = 0; y < r.dh; y++) {
    int sy = y * r.sh / r.dh;
    const u16 *srow = n3ds_st_buffer + (r.sy + sy) * ST_W + r.sx;
    const unsigned char *drow = depthMap[sy];
    for (int x = 0; x < r.dw; x++) {
      int sx = x * r.sw / r.dw;
      if (k != 0.0f) {
        // Left eye samples to the right and vice versa -> far content sits behind the screen.
        float shift = -eye * k * drow[sx];
        int sx2 = (int)((x + shift) * r.sw / (float)r.dw + 0.5f);
        sx = sx2 < 0 ? 0 : (sx2 >= r.sw ? r.sw - 1 : sx2);
      }
      putpx(fb, r.dx + x, r.dy + y, srow[sx]);
    }
  }
}

static void drawTop(u16 *fb, float eye /* -1 left, +1 right */, float slider)
{
  memset(fb, 0, TOP_W * TOP_H * 2);
  if (layout == LAYOUT_DUNGEON) {
    blitViewportStereo(fb, TOP_DUNGEON_VP, eye, slider);
    blitRegion(fb, TOP_DUNGEON_TXT);
  } else {
    blitRegion(fb, TOP_FULL);
  }
}

static const Region *bottomRegions(int *count)
{
  if (layout == LAYOUT_DUNGEON) { *count = 3; return BOT_DUNGEON; }
  *count = 1;
  return BOT_FULL;
}

static void drawBottom(u16 *fb)
{
  memset(fb, 0, BOT_W * BOT_H * 2);
  int n;
  const Region *regs = bottomRegions(&n);
  for (int i = 0; i < n; i++) blitRegion(fb, regs[i]);
  if (layout == LAYOUT_DUNGEON) frameRegion(fb, BOT_DUNGEON[2], 0x632C);  // grey frame round mini view
  if (dbgEnabled && dbgTX >= 0) {   // debug: magenta crosshair where the last tap landed
    for (int d = -4; d <= 4; d++) {
      if (dbgTX + d >= 0 && dbgTX + d < BOT_W) putpx(fb, dbgTX + d, dbgTY, 0xF81F);
      if (dbgTY + d >= 0 && dbgTY + d < BOT_H) putpx(fb, dbgTX, dbgTY + d, 0xF81F);
    }
  }
}

// Bottom-screen pixel -> ST coordinate, through whichever region contains it.
static bool touchToST(int px, int py, int *stx, int *sty)
{
  int n;
  const Region *regs = bottomRegions(&n);
  if (layout == LAYOUT_FULL) {   // single region: snap taps in the black borders to the edge
    const Region &r = regs[0];
    if (py < r.dy) py = r.dy;
    if (py >= r.dy + r.dh) py = r.dy + r.dh - 1;
  }
  for (int i = 0; i < n; i++) {
    const Region &r = regs[i];
    if (px < r.dx || px >= r.dx + r.dw || py < r.dy || py >= r.dy + r.dh) continue;
    *stx = r.sx + (px - r.dx) * r.sw / r.dw;
    *sty = r.sy + (py - r.dy) * r.sh / r.dh;
    return true;
  }
  return false;   // gap between regions: not a click
}

static void setupScreens(void)
{
  gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
  gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
  gfxSetDoubleBuffering(GFX_TOP, true);
  gfxSetDoubleBuffering(GFX_BOTTOM, true);
  gfxSet3D(true);
}

// The engine presents only when the screen changed. Presents are capped at ~60 Hz,
// and a frame that arrives too soon is kept pending (never dropped) so the final
// frame of an animation always reaches the screen; n3ds_poll() flushes it.
static bool presentPending = false;
static float lastSlider = -1.0f;

static void flushPresent(bool force)
{
  static u64 lastTick = 0;
  u64 now = svcGetSystemTick();
  if (!force && now - lastTick < SYSCLOCK_ARM11 / 62) return;
  lastTick = now;
  presentPending = false;
  dbgPresents++;

  float slider = osGet3DSliderState();
  lastSlider = slider;
  drawTop((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), -1.0f, slider);
  drawTop((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL), +1.0f, slider);
  drawBottom((u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
  gfxFlushBuffers();
  gfxSwapBuffers();
}

void n3ds_present(void)
{
  presentPending = true;
  flushPresent(false);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
// Scancodes match the [keys] section of the shipped csb.cfg (SDL2 scancodes).
struct KeyMap { u32 button; int sym; int scan; bool repeat; };
static const KeyMap keyMap[] = {
  { KEY_DUP    | KEY_CPAD_UP,    0x40000060, 0x60, true  },  // keypad 8: forward
  { KEY_DDOWN  | KEY_CPAD_DOWN,  0x4000005d, 0x5d, true  },  // keypad 5: back
  { KEY_DLEFT  | KEY_CPAD_LEFT,  0x4000005f, 0x5f, true  },  // keypad 7: turn left
  { KEY_DRIGHT | KEY_CPAD_RIGHT, 0x40000061, 0x61, true  },  // keypad 9: turn right
  { KEY_L,                       0x4000005c, 0x5c, true  },  // keypad 4: strafe left
  { KEY_R,                       0x4000005e, 0x5e, true  },  // keypad 6: strafe right
  { KEY_A,                       'q',        0x14, false },  // champion 1 attack
  { KEY_B,                       'w',        0x1a, false },  // champion 2 attack
  { KEY_X,                       'e',        0x08, false },  // champion 3 attack
  { KEY_Y,                       'r',        0x15, false },  // champion 4 attack
  { KEY_START,                   0x1b,       0x29, false },  // Esc: freeze game
};
static const u64 REPEAT_DELAY = SYSCLOCK_ARM11 / 4;   // 250 ms
static const u64 REPEAT_RATE  = SYSCLOCK_ARM11 / 6;   // ~170 ms between repeated steps

static bool touching = false, touchRight = false;
static int lastSTX = 0, lastSTY = 0;

static void handleTouch(u32 held)
{
  if (held & KEY_TOUCH) {
    touchPosition t;
    hidTouchRead(&t);
    int x, y;
    bool inside = touchToST(t.px, t.py, &x, &y);
    if (!touching) {
      if (!inside) return;                        // tap in a gap: ignore until released
      touching = true;
      touchRight = (held & KEY_ZR) != 0;          // ZR + tap = right mouse button
      dbgTX = t.px; dbgTY = t.py; presentPending = true;
      DBG("[n3ds] touch DOWN px=(%d,%d) st=(%d,%d) layout=%d right=%d\n",
          t.px, t.py, x, y, (int)layout, touchRight);
      n3ds_send_mouse_move(x, y);
      n3ds_send_mouse_button(touchRight, true, x, y);
      lastSTX = x; lastSTY = y;
    } else if (inside && (x != lastSTX || y != lastSTY)) {
      n3ds_send_mouse_move(x, y);                 // drag
      lastSTX = x; lastSTY = y;
    }
  } else if (touching) {
    touching = false;
    DBG("[n3ds] touch UP st=(%d,%d)\n", lastSTX, lastSTY);
    n3ds_send_mouse_button(touchRight, false, lastSTX, lastSTY);
  }
}

bool n3ds_poll(void)
{
  if (!aptMainLoop()) return false;
  hidScanInput();
  u32 down = hidKeysDown(), held = hidKeysHeld();
  dbgPolls++;
  if (dbgEnabled) {
    static u64 nextBeat = 0;
    u64 t = svcGetSystemTick();
    if (t >= nextBeat) {
      nextBeat = t + SYSCLOCK_ARM11 * 5;
      printf("[n3ds] beat polls=%u presents=%u layout=%d\n", dbgPolls, dbgPresents, (int)layout);
    }
  }

  // Pick the layout from the engine's state. Never switch in the middle of a tap,
  // so a press and its release are always mapped through the same regions.
  if (!touching) {
    Layout want = n3ds_engine_in_dungeon_view() ? LAYOUT_DUNGEON : LAYOUT_FULL;
    if (want != layout) {
      DBG("[n3ds] layout %d -> %d\n", (int)layout, (int)want);
      layout = want;
      presentPending = true;
    }
  }

  if ((held & KEY_START) && (held & KEY_SELECT)) {
    if (n3ds_message_box("Quit to the Homebrew Launcher?\nUnsaved progress will be lost.",
                         "Quit", true))
      return false;
    return true;
  }
  if ((down & KEY_SELECT) && !(held & KEY_START)) { depthEnabled = !depthEnabled; presentPending = true; }

  static u64 nextRepeat[sizeof(keyMap) / sizeof(keyMap[0])];
  u64 now = svcGetSystemTick();
  for (size_t i = 0; i < sizeof(keyMap) / sizeof(keyMap[0]); i++) {
    const KeyMap &k = keyMap[i];
    if (k.button == KEY_START && (held & KEY_SELECT)) continue;
    if (down & k.button) {
      n3ds_send_key(k.sym, k.scan);
      nextRepeat[i] = now + REPEAT_DELAY;
    } else if (k.repeat && (held & k.button) && now >= nextRepeat[i]) {
      n3ds_send_key(k.sym, k.scan);
      nextRepeat[i] = now + REPEAT_RATE;
    }
  }
  handleTouch(held);
  if (osGet3DSliderState() != lastSlider) presentPending = true;
  if (presentPending) flushPresent(false);
  return true;
}

// ---------------------------------------------------------------------------
// Message box (bottom-screen console, A = OK/Yes, B = No)
// ---------------------------------------------------------------------------
bool n3ds_message_box(const char *msg, const char *title, bool yesno)
{
  static PrintConsole con;
  consoleInit(GFX_BOTTOM, &con);
  consoleSelect(&con);
  consoleClear();
  printf("\x1b[1;1H\x1b[33m%s\x1b[0m\n\n%s\n\n", title ? title : "CSB", msg ? msg : "");
  printf(yesno ? "A = Yes    B = No\n" : "Press A to continue\n");
  bool result = true;
  while (aptMainLoop()) {
    hidScanInput();
    u32 kd = hidKeysDown();
    if (kd & KEY_A) { result = true; break; }
    if (yesno && (kd & KEY_B)) { result = false; break; }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  // Give the bottom screen back to the game.
  consoleClear();
  setupScreens();
  n3ds_force_redraw();
  presentPending = true;
  return result;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
static bool fileExists(const char *name)
{
  struct stat st;
  return stat(name, &st) == 0;
}

void n3ds_init(void)
{
  osSetSpeedupEnable(true);   // 804 MHz + L2 cache on New 3DS
  gfxInitDefault();
  setupScreens();
  buildDepthMap();
  memset(n3ds_st_buffer, 0, sizeof(n3ds_st_buffer));

  mkdir(DATA_DIR, 0777);
  (void)chdir(DATA_DIR);
  if (fileExists("debug.txt") && freopen(DATA_DIR "/csb3ds.log", "w", stdout)) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), fileno(stderr));
    dbgEnabled = true;
    printf("[n3ds] csb-3ds debug log start\n");
  }
  if (!fileExists("graphics.dat") || !fileExists("dungeon.dat")) {
    n3ds_message_box("Game data not found.\n\n"
                     "Copy GRAPHICS.DAT and DUNGEON.DAT (and the csb.cfg\n"
                     "from csb-3ds) into:\n\n  " DATA_DIR "/\n\n"
                     "See the README for where to get them.",
                     "Dungeon Master 3DS", false);
    gfxExit();
    exit(0);
  }
}

void n3ds_shutdown(void)
{
  gfxExit();
}

// newlib on devkitARM may not provide realpath(); imguifilebrowser references it.
// It is never called on 3DS (the ImGui front end is compiled but not run).
extern "C" __attribute__((weak)) char *realpath(const char *path, char *resolved)
{
  if (!resolved) resolved = (char *)malloc(PATH_MAX);
  if (resolved) strncpy(resolved, path, PATH_MAX - 1), resolved[PATH_MAX - 1] = 0;
  return resolved;
}

#endif // __3DS__
