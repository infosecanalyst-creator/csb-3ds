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

static inline u16 rgb565(int r, int g, int b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

// Dungeon Master's champion colours, by party slot.
static const u16 CHAMP_COLOR[4] = {
  rgb565(0, 204, 0), rgb565(238, 221, 0), rgb565(238, 0, 0), rgb565(51, 102, 255) };

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
struct Rect { int x, y, w, h; };
static inline bool inRect(const Rect &r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// --- Top screen -----------------------------------------------------------
// Dungeon: viewport at 1.5x (336x204) with the message strip underneath, party
// vitals (health / stamina / mana) in the side margins where the eyes already are.
static const Region TOP_DUNGEON_VP  = { VP_X, VP_Y, VP_W, VP_H, (TOP_W - 336) / 2, 0, 336, 204 };
static const Region TOP_DUNGEON_TXT = { 0, TXT_Y, ST_W, TXT_H, (TOP_W - 320) / 2, 207, 320, TXT_H };
// Everything else (title, entrance, menus, inventory): whole screen at 1.2x, centred.
static const Region TOP_FULL        = { 0, 0, ST_W, ST_H, (TOP_W - 384) / 2, 0, 384, 240 };

// --- Bottom screen --------------------------------------------------------
// Dungeon: controls only, sized for fingers.
//   champion portraits/hands at 1:1 across the top,
//   spell runes + attack menu + movement arrows at 1.5x on the right,
//   a small copy of the viewport on the left for grabbing items, wall buttons, doors,
//   and two 3DS-only shortcut buttons in the spare space: RECAST and BAG.
static const Region BOT_DUNGEON[] = {
  { 0,     0,    ST_W,  PORT_H, 0,   0,  320, PORT_H },        // portraits
  { CTL_X, VP_Y, CTL_W, VP_H,   176, 35, 144, 204    },        // controls 1.5x
  { VP_X,  VP_Y, VP_W,  VP_H,   0,   84, 176, 107    },        // mini viewport (0.79x)
};
static const Rect BTN_RECAST = { 4, 38, 168, 42 };              // above the mini viewport
static const Rect BTN_BAG    = { 4, 195, 168, 42 };             // below the mini viewport
// Inventory: portraits 1:1 + the inventory panel at 1.4x, so slots are finger-sized.
static const Region BOT_INVENTORY[] = {
  { 0,    0,    ST_W, PORT_H, 0, 0,  320, PORT_H },
  { VP_X, VP_Y, VP_W, VP_H,   3, 37, 314, 190    },
};
// Dungeon before anyone has joined the party (the portrait row, runes and attack
// menu are all empty): the dungeon view at 1:1 and the movement arrows at 2x.
static const Region BOT_DUNGEON_EMPTY[] = {
  { VP_X, VP_Y, VP_W, VP_H, (BOT_W - 224) / 2, 4,   224, 136 },  // viewport 1:1
  { 232,  122,  88,   47,   (BOT_W - 176) / 2, 146, 176, 94  },  // movement arrows 2x
};
static bool partyEmpty = false;   // chosen with the layout, never mid-tap
// Everything else: the whole screen at 1:1 (exact mouse mapping), or zoomed in
// on the screen's buttons so they are finger-sized (see updateFullZoom()).
static const Region BOT_FULL_1X = { 0, 0, ST_W, ST_H, 0, (BOT_H - ST_H) / 2, ST_W, ST_H };
static Region BOT_FULL[1] = { BOT_FULL_1X };

enum Layout { LAYOUT_FULL = 0, LAYOUT_DUNGEON = 1, LAYOUT_INVENTORY = 2 };
static Layout layout = LAYOUT_FULL;

// Stereoscopic depth -------------------------------------------------------
// DM's viewport is a one-point perspective: walls at each distance form nested
// rectangles around the vanishing point. A rectangular (Chebyshev) distance from
// that point is a decent depth estimate for corridors and walls: the centre is
// far away (pushed into the screen), the edges sit at the screen plane.
// The 3D slider is the only control (slide it down to turn depth off).
static const float VP_HORIZON_X = 112.0f;  // tune on hardware if the corridor looks bent
static const float VP_HORIZON_Y = 68.0f;
static const float MAX_DISPARITY = 7.0f;   // top-screen pixels at full slider
static unsigned char depthMap[VP_H][VP_W];  // 0 = screen plane .. 255 = farthest

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

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
// Framebuffers are rotated: pixel (x, y) lives at x*240 + (239 - y).
static inline void putpx(u16 *fb, int x, int y, u16 c) { fb[x * 240 + (239 - y)] = c; }

static void fillRect(u16 *fb, int w, int x0, int y0, int rw, int rh, u16 c)
{
  for (int x = x0; x < x0 + rw; x++) {
    if (x < 0 || x >= w) continue;
    for (int y = y0; y < y0 + rh; y++)
      if (y >= 0 && y < 240) putpx(fb, x, y, c);
  }
}

// Nearest-neighbour scale, written column by column because the framebuffer is
// stored rotated (each screen column is contiguous). No divisions in the inner loop.
static void blitRegion(u16 *fb, const Region &r)
{
  static int srcRow[240];
  for (int y = 0; y < r.dh; y++) srcRow[y] = (r.sy + y * r.sh / r.dh) * ST_W;
  for (int x = 0; x < r.dw; x++) {
    const u16 *scol = n3ds_st_buffer + r.sx + x * r.sw / r.dw;
    u16 *dcol = fb + (r.dx + x) * 240 + (239 - r.dy);
    for (int y = 0; y < r.dh; y++) dcol[-y] = scol[srcRow[y]];
  }
}

static void frameRect(u16 *fb, int w, int x0, int y0, int rw, int rh, u16 c)   // 1px outline
{
  fillRect(fb, w, x0, y0, rw, 1, c);
  fillRect(fb, w, x0, y0 + rh - 1, rw, 1, c);
  fillRect(fb, w, x0, y0, 1, rh, c);
  fillRect(fb, w, x0 + rw - 1, y0, 1, rh, c);
}

// Tiny 5x7 font, just the characters the 3DS-only buttons use.
struct Glyph { char c; unsigned char rows[7]; };
static const Glyph FONT[] = {
  {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
  {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
  {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
  {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {' ',{0,0,0,0,0,0,0}},
};

static void drawText(u16 *fb, int w, int x, int y, const char *s, u16 c, int scale)
{
  for (; *s; s++, x += 6 * scale) {
    const Glyph *g = NULL;
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) if (FONT[i].c == *s) g = &FONT[i];
    if (!g) continue;
    for (int r = 0; r < 7; r++)
      for (int b = 0; b < 5; b++)
        if (g->rows[r] & (0x10 >> b)) fillRect(fb, w, x + b * scale, y + r * scale, scale, scale, c);
  }
}

// ---------------------------------------------------------------------------
// RECAST: remember each champion's last spell (hooked in Magic.cpp) and replay it
// ---------------------------------------------------------------------------
static signed char lastSpell[4][4];
static int lastCaster = -1;

void n3ds_note_spell(int caster, const signed char *incantation)
{
  if (caster < 0 || caster > 3 || incantation[0] == 0) return;
  memcpy(lastSpell[caster], incantation, 4);
  lastCaster = caster;
  DBG("[n3ds] spell noted: caster %d runes %d %d %d %d\n", caster,
      incantation[0], incantation[1], incantation[2], incantation[3]);
}

// ---------------------------------------------------------------------------
// Scripted input: short sequences of key presses / clicks, spaced out so the
// engine's small mouse queue (4 entries) never overflows.
// ---------------------------------------------------------------------------
struct Action { int kind, a, b; u64 dueTicks; };   // kind 0 = key(sym=a, scan=b), 1 = right-click at (a,b)
static Action actQueue[24];
static int actHead = 0, actCount = 0;
static const u64 MS = SYSCLOCK_ARM11 / 1000;

static void queueAction(int kind, int a, int b, int delayMs)
{
  if (actCount == (int)(sizeof(actQueue) / sizeof(actQueue[0]))) return;
  u64 base = svcGetSystemTick();
  if (actCount) {   // schedule relative to the last queued action
    const Action &last = actQueue[(actHead + actCount - 1) % 24];
    if (last.dueTicks > base) base = last.dueTicks;
  }
  Action &n = actQueue[(actHead + actCount) % 24];
  n.kind = kind; n.a = a; n.b = b; n.dueTicks = base + delayMs * MS;
  actCount++;
}
static inline void queueKey(int sym, int scan, int delayMs) { queueAction(0, sym, scan, delayMs); }

static bool touching = false, touchRight = false, touchOnButton = false;
static int lastSTX = 0, lastSTY = 0;   // where the last tap put the game cursor

static u64 parkCursorAt = 0;   // when to move the arrow out of the way (0 = no)
static const int PARK_X = 300, PARK_Y = 8;   // empty space right of the 4th portrait

static bool rightClickPending = false;
static int rightClickX, rightClickY;

static void runActions(void)
{
  if (parkCursorAt && svcGetSystemTick() >= parkCursorAt) {
    parkCursorAt = 0;
    if (!touching && n3ds_engine_cursor_is_arrow()) {
      n3ds_send_mouse_move(PARK_X, PARK_Y);
      lastSTX = PARK_X; lastSTY = PARK_Y;
    }
  }
  if (rightClickPending) {   // release the right button one poll after pressing it
    n3ds_send_mouse_button(true, false, rightClickX, rightClickY);
    n3ds_send_mouse_move(lastSTX, lastSTY);   // put the cursor back where the player left it
    rightClickPending = false;
  }
  u64 now = svcGetSystemTick();
  while (actCount && actQueue[actHead].dueTicks <= now) {
    Action &a = actQueue[actHead];
    if (a.kind == 0) {
      n3ds_send_key(a.a, a.b);
    } else {
      n3ds_send_mouse_move(a.a, a.b);
      n3ds_send_mouse_button(true, true, a.a, a.b);
      rightClickX = a.a; rightClickY = a.b; rightClickPending = true;
      actHead = (actHead + 1) % 24; actCount--;
      break;   // the release goes out on the next poll
    }
    actHead = (actHead + 1) % 24; actCount--;
  }
}

// Keyboard shortcuts from csb.cfg ([keys] mscan entries), as SDL scancodes.
static const int ATTACK_KEY_SYM[3][4]  = { {'q','w','e','r'}, {'a','s','d','f'}, {'z','x','c','v'} };
static const int ATTACK_KEY_SCAN[3][4] = { {0x14,0x1a,0x08,0x15}, {0x04,0x16,0x07,0x09}, {0x1d,0x1b,0x06,0x19} };
enum { SC_TAB = 0x2b, SC_SPACE = 0x2c, SC_F1 = 0x3a, SC_1 = 0x1e };

// One press = one attack: open the champion's attack menu (closing someone else's
// first) and pick attack row 0..2. Clicking a hand that is still recovering does
// nothing, and the follow-up click then lands on that same hand, so nothing stray happens.
static void quickAttack(int champ, int row)
{
  if (champ >= n3ds_engine_party_size()) return;
  int owner = n3ds_engine_attack_menu_owner();   // 0 = no menu, else champion ordinal
  if (owner == champ + 1) {
    queueKey(ATTACK_KEY_SYM[row][champ], ATTACK_KEY_SCAN[row][champ], 0);
    return;
  }
  if (owner != 0) queueKey('\t', SC_TAB, 0);                       // "pass" closes the other menu
  queueKey(ATTACK_KEY_SYM[0][champ], ATTACK_KEY_SCAN[0][champ], owner ? 90 : 0);   // open menu
  queueKey(ATTACK_KEY_SYM[row][champ], ATTACK_KEY_SCAN[row][champ], 140);          // choose attack
  DBG("[n3ds] quick attack champ %d row %d (menu owner %d)\n", champ, row, owner);
}

// Replay the last spell: select the caster's tab (F1..F4), enter the runes (1..6), cast (space).
static void recastLastSpell(void)
{
  if (lastCaster < 0 || lastCaster >= n3ds_engine_party_size()) return;
  const signed char *inc = lastSpell[lastCaster];
  queueKey(0x4000003a + lastCaster, SC_F1 + lastCaster, 0);        // SDLK_F1.. / caster tab
  for (int i = 0; i < 4 && inc[i]; i++) {
    int button = inc[i] - 96 - 6 * i;                               // see Magic.cpp / CSBCode.cpp
    if (button < 0 || button > 5) break;
    queueKey('1' + button, SC_1 + button, 110);
  }
  queueKey(' ', SC_SPACE, 140);
  DBG("[n3ds] recast: caster %d\n", lastCaster);
}

// Right-click in the viewport toggles the leader's inventory (DM's own mouse convention).
static void toggleLeaderInventory(void) { queueAction(1, 100, 100, 0); }

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Viewport at 1.5x with per-pixel horizontal disparity from the depth map. The source
// column for every output pixel only depends on the slider, so it is cached per eye
// and rebuilt when the slider moves (no float math per frame).
enum { SV_W = 336, SV_H = 204 };
static unsigned char stereoCol[2][SV_H][SV_W];
static float stereoCachedSlider = -1.0f;

static void buildStereoTables(const Region &r, float slider)
{
  const float k = slider * MAX_DISPARITY * 0.5f / 255.0f;
  for (int e = 0; e < 2; e++) {
    float eye = e ? +1.0f : -1.0f;
    for (int y = 0; y < r.dh; y++) {
      const unsigned char *drow = depthMap[y * r.sh / r.dh];
      for (int x = 0; x < r.dw; x++) {
        int sx = x * r.sw / r.dw;
        if (k != 0.0f) {
          // Left eye samples to the right and vice versa -> far content sits behind the screen.
          float shift = -eye * k * drow[sx];
          int sx2 = (int)((x + shift) * r.sw / (float)r.dw + 0.5f);
          sx = sx2 < 0 ? 0 : (sx2 >= r.sw ? r.sw - 1 : sx2);
        }
        stereoCol[e][y][x] = (unsigned char)sx;
      }
    }
  }
  stereoCachedSlider = slider;
}

static void blitViewportStereo(u16 *fb, const Region &r, float eye, float slider)
{
  if (slider != stereoCachedSlider) buildStereoTables(r, slider);
  const int e = eye > 0 ? 1 : 0;
  for (int y = 0; y < r.dh; y++) {
    const u16 *srow = n3ds_st_buffer + (r.sy + y * r.sh / r.dh) * ST_W + r.sx;
    const unsigned char *cols = stereoCol[e][y];
    for (int x = 0; x < r.dw; x++) putpx(fb, r.dx + x, r.dy + y, srow[cols[x]]);
  }
}

// Party vitals in the top screen's side margins: champions 1-2 on the left, 3-4 on
// the right; three vertical bars each (health, stamina, mana) in the champion's colour,
// health bar background turns red below a quarter.
static void drawVitals(u16 *fb)
{
  int n = n3ds_engine_party_size();
  const u16 back = rgb565(40, 40, 40), danger = rgb565(140, 0, 0);
  for (int i = 0; i < n && i < 4; i++) {
    int stats[6];
    n3ds_engine_champion_stats(i, stats);   // hp, maxHp, stamina, maxStamina, mana, maxMana
    int x0 = (i < 2) ? 4 : TOP_W - 28;
    int y0 = (i % 2) * 120 + 6;
    const int barH = 104;
    fillRect(fb, TOP_W, x0, y0, 24, 4, CHAMP_COLOR[i]);              // colour tab = party slot
    for (int b = 0; b < 3; b++) {
      int cur = stats[b * 2], max = stats[b * 2 + 1];
      int bx = x0 + b * 9, by = y0 + 8;
      bool low = (b == 0 && max > 0 && cur * 4 < max && cur > 0);
      fillRect(fb, TOP_W, bx, by, 6, barH, low ? danger : back);
      if (max > 0 && cur > 0) {
        int h = cur >= max ? barH : cur * barH / max;
        if (h < 1) h = 1;
        fillRect(fb, TOP_W, bx, by + barH - h, 6, h, CHAMP_COLOR[i]);
      }
    }
  }
}

static void drawTop(u16 *fb, float eye /* -1 left, +1 right */, float slider)
{
  memset(fb, 0, TOP_W * TOP_H * 2);
  if (layout == LAYOUT_DUNGEON) {
    blitViewportStereo(fb, TOP_DUNGEON_VP, eye, slider);
    blitRegion(fb, TOP_DUNGEON_TXT);
    drawVitals(fb);
  } else {
    blitRegion(fb, TOP_FULL);
  }
}

static const Region *bottomRegions(int *count)
{
  if (layout == LAYOUT_DUNGEON && partyEmpty) { *count = 2; return BOT_DUNGEON_EMPTY; }
  if (layout == LAYOUT_DUNGEON)   { *count = 3; return BOT_DUNGEON; }
  if (layout == LAYOUT_INVENTORY) { *count = 2; return BOT_INVENTORY; }
  *count = 1;
  return BOT_FULL;
}

static void drawButtons(u16 *fb)
{
  const u16 face = rgb565(52, 44, 36), edge = rgb565(150, 130, 100), text = rgb565(230, 220, 190);
  // RECAST: shows whose spell it replays (colour) and how many runes.
  fillRect(fb, BOT_W, BTN_RECAST.x, BTN_RECAST.y, BTN_RECAST.w, BTN_RECAST.h, face);
  frameRect(fb, BOT_W, BTN_RECAST.x, BTN_RECAST.y, BTN_RECAST.w, BTN_RECAST.h, edge);
  bool haveSpell = lastCaster >= 0 && lastCaster < n3ds_engine_party_size();
  if (haveSpell) {
    fillRect(fb, BOT_W, BTN_RECAST.x + 3, BTN_RECAST.y + 3, 8, BTN_RECAST.h - 6, CHAMP_COLOR[lastCaster]);
    drawText(fb, BOT_W, BTN_RECAST.x + 20, BTN_RECAST.y + 7, "RECAST", text, 2);
    for (int i = 0; i < 4 && lastSpell[lastCaster][i]; i++)
      fillRect(fb, BOT_W, BTN_RECAST.x + 20 + i * 12, BTN_RECAST.y + 28, 8, 8, CHAMP_COLOR[lastCaster]);
  } else {
    drawText(fb, BOT_W, BTN_RECAST.x + 20, BTN_RECAST.y + 14, "NO SPELL", rgb565(110, 100, 90), 2);
  }
  // BAG: the leader's inventory.
  fillRect(fb, BOT_W, BTN_BAG.x, BTN_BAG.y, BTN_BAG.w, BTN_BAG.h, face);
  frameRect(fb, BOT_W, BTN_BAG.x, BTN_BAG.y, BTN_BAG.w, BTN_BAG.h, edge);
  drawText(fb, BOT_W, BTN_BAG.x + 20, BTN_BAG.y + 14, "BAG", text, 2);
}

static void drawBottom(u16 *fb)
{
  memset(fb, 0, BOT_W * BOT_H * 2);
  int n;
  const Region *regs = bottomRegions(&n);
  for (int i = 0; i < n; i++) blitRegion(fb, regs[i]);
  if (layout == LAYOUT_DUNGEON) {
    const Region &mv = partyEmpty ? BOT_DUNGEON_EMPTY[0] : BOT_DUNGEON[2];
    frameRect(fb, BOT_W, mv.dx - 1, mv.dy - 1, mv.dw + 2, mv.dh + 2, rgb565(100, 100, 100));
    if (!partyEmpty) drawButtons(fb);
  }
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
static unsigned lastVitalsHash = 0;

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
  // Wait for the swap to take effect. Without this, two presents inside one
  // display frame meant the next frame was drawn into the buffer still being
  // shown, which flickered and tore while walking.
  gspWaitForVBlank();
}

void n3ds_present(void)
{
  presentPending = true;
  flushPresent(false);
}

// Menus, the entrance, the champion mirror, freeze, game over: zoom the bottom
// screen in on the buttons the engine is hit-testing, at 2x or 1.5x when they fit,
// so they are finger-sized (Nintendo's guidance is about 1 cm). The whole screen
// stays on the top screen. Hold ZL to see the bottom at 1:1.
static void updateFullZoom(bool forceOneToOne)
{
  Region r = BOT_FULL_1X;
  int x0, y0, x1, y1;
  if (!forceOneToOne && n3ds_engine_button_bounds(&x0, &y0, &x1, &y1)) {
    const int M = 8;   // margin around the buttons
    x0 = x0 - M < 0 ? 0 : x0 - M;            y0 = y0 - M < 0 ? 0 : y0 - M;
    x1 = x1 + M > ST_W - 1 ? ST_W - 1 : x1 + M; y1 = y1 + M > ST_H - 1 ? ST_H - 1 : y1 + M;
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    int sw = 0, sh = 0;
    if (bw <= 160 && bh <= 120)      { sw = 160; sh = 120; }   // 2x
    else if (bw <= 213 && bh <= 160) { sw = 213; sh = 160; }   // 1.5x
    if (sw) {
      int sx = (x0 + x1 + 1) / 2 - sw / 2, sy = (y0 + y1 + 1) / 2 - sh / 2;
      if (sx < 0) sx = 0;
      if (sx > ST_W - sw) sx = ST_W - sw;
      if (sy < 0) sy = 0;
      if (sy > ST_H - sh) sy = ST_H - sh;
      Region z = { sx, sy, sw, sh, 0, 0, BOT_W, BOT_H };
      r = z;
    }
  }
  if (memcmp(&r, &BOT_FULL[0], sizeof r) != 0) {
    DBG("[n3ds] full zoom src=(%d,%d %dx%d)\n", r.sx, r.sy, r.sw, r.sh);
    BOT_FULL[0] = r;
    presentPending = true;
  }
}

// Party stats change without the engine redrawing the viewport (e.g. stamina
// regenerating), so redraw when they change.
static unsigned vitalsHash(void)
{
  unsigned h = (unsigned)n3ds_engine_party_size() * 2654435761u;
  for (int i = 0; i < n3ds_engine_party_size() && i < 4; i++) {
    int s[6];
    n3ds_engine_champion_stats(i, s);
    for (int k = 0; k < 6; k++) h = h * 31u + (unsigned)s[k];
  }
  return h;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
// Movement: D-pad/Circle Pad up/down = step, left/right = turn, L/R = strafe
// (Etrian Odyssey / Legend of Grimrock conventions). Keys repeat while held.
struct KeyMap { u32 button; int sym; int scan; };
static const KeyMap moveMap[] = {
  { KEY_DUP    | KEY_CPAD_UP,    0x40000060, 0x60 },  // keypad 8: forward
  { KEY_DDOWN  | KEY_CPAD_DOWN,  0x4000005d, 0x5d },  // keypad 5: back
  { KEY_DLEFT  | KEY_CPAD_LEFT,  0x4000005f, 0x5f },  // keypad 7: turn left
  { KEY_DRIGHT | KEY_CPAD_RIGHT, 0x40000061, 0x61 },  // keypad 9: turn right
  { KEY_L,                       0x4000005c, 0x5c },  // keypad 4: strafe left
  { KEY_R,                       0x4000005e, 0x5e },  // keypad 6: strafe right
};
static const u32 FACE_BUTTON[4] = { KEY_A, KEY_B, KEY_X, KEY_Y };   // champions 1..4
static const u64 REPEAT_DELAY = SYSCLOCK_ARM11 / 4;   // 250 ms
static const u64 REPEAT_RATE  = SYSCLOCK_ARM11 / 6;   // ~170 ms between repeated steps

static void handleTouch(u32 held)
{
  if (held & KEY_TOUCH) {
    touchPosition t;
    hidTouchRead(&t);
    if (!touching) {
      touching = true;
      dbgTX = t.px; dbgTY = t.py; presentPending = true;
      touchOnButton = false;
      if (layout == LAYOUT_DUNGEON && !partyEmpty) {   // 3DS-only shortcut buttons fire on press
        if (inRect(BTN_RECAST, t.px, t.py)) { touchOnButton = true; recastLastSpell(); return; }
        if (inRect(BTN_BAG, t.px, t.py))    { touchOnButton = true; toggleLeaderInventory(); return; }
      }
      int x, y;
      if (!touchToST(t.px, t.py, &x, &y)) { touchOnButton = true; return; }   // gap: swallow this touch
      touchRight = (held & KEY_ZR) != 0;          // ZR + tap = right mouse button
      DBG("[n3ds] touch DOWN px=(%d,%d) st=(%d,%d) layout=%d right=%d\n",
          t.px, t.py, x, y, (int)layout, touchRight);
      n3ds_send_mouse_move(x, y);
      n3ds_send_mouse_button(touchRight, true, x, y);
      lastSTX = x; lastSTY = y;
    } else if (!touchOnButton) {
      int x, y;
      if (touchToST(t.px, t.py, &x, &y) && (x != lastSTX || y != lastSTY)) {
        n3ds_send_mouse_move(x, y);               // drag
        lastSTX = x; lastSTY = y;
      }
    }
  } else if (touching) {
    touching = false;
    if (!touchOnButton) {
      DBG("[n3ds] touch UP st=(%d,%d)\n", lastSTX, lastSTY);
      n3ds_send_mouse_button(touchRight, false, lastSTX, lastSTY);
      // The engine draws its own cursor into the picture, so after a tap in the dungeon
      // the arrow would sit in the big view on the top screen. Park the plain arrow in
      // the empty end of the portrait strip (bottom screen only). A held item keeps
      // following the finger: its picture is how DM shows what is in your hand.
      if (layout == LAYOUT_DUNGEON && n3ds_engine_cursor_is_arrow()) parkCursorAt = svcGetSystemTick() + 120 * MS;
    }
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
    Layout want = LAYOUT_FULL;
    if (n3ds_engine_in_dungeon_view()) want = LAYOUT_DUNGEON;
    else if (n3ds_engine_inventory_open() && !n3ds_engine_candidate_shown()) want = LAYOUT_INVENTORY;
    if (want != layout) {
      DBG("[n3ds] layout %d -> %d\n", (int)layout, (int)want);
      layout = want;
      presentPending = true;
    }
    bool empty = n3ds_engine_party_size() == 0;
    if (empty != partyEmpty) { partyEmpty = empty; presentPending = true; }
    if (layout == LAYOUT_FULL) updateFullZoom((held & KEY_ZL) != 0);
  }

  if ((held & KEY_START) && (held & KEY_SELECT)) {
    if (n3ds_message_box("Quit to the Homebrew Launcher?\nUnsaved progress will be lost.",
                         "Quit", true))
      return false;
    return true;
  }
  if ((down & KEY_START) && !(held & KEY_SELECT)) n3ds_send_key(0x1b, 0x29);      // Esc: freeze
  // These shortcuts replay clicks at fixed game-screen spots, so they only run where
  // those spots mean what we expect: the dungeon view (and the inventory for SELECT).
  if ((down & KEY_SELECT) && !(held & KEY_START) && layout != LAYOUT_FULL)
    toggleLeaderInventory();                                                      // SELECT: bag
  // Champion attacks: A/B/X/Y = champion 1/2/3/4, one press per attack.
  // Plain = first attack, with ZL = second, with ZR = third (the attack menu's three rows).
  // Holding the button keeps attacking: the attack is re-issued every 0.6 s, and
  // the engine itself ignores it until the champion's weapon has recovered.
  static u64 nextAutoAttack[4];
  if (layout == LAYOUT_DUNGEON) {
    u64 t = svcGetSystemTick();
    for (int c = 0; c < 4; c++) {
      int row = (held & KEY_ZL) ? 1 : (held & KEY_ZR) ? 2 : 0;
      if (down & FACE_BUTTON[c]) {
        quickAttack(c, row);
        nextAutoAttack[c] = t + 600 * MS;
      } else if ((held & FACE_BUTTON[c]) && t >= nextAutoAttack[c] && actCount == 0) {
        quickAttack(c, row);
        nextAutoAttack[c] = t + 600 * MS;
      }
    }
  }

  static u64 nextRepeat[sizeof(moveMap) / sizeof(moveMap[0])];
  u64 now = svcGetSystemTick();
  for (size_t i = 0; i < sizeof(moveMap) / sizeof(moveMap[0]); i++) {
    const KeyMap &k = moveMap[i];
    if (down & k.button) {
      n3ds_send_key(k.sym, k.scan);
      nextRepeat[i] = now + REPEAT_DELAY;
    } else if ((held & k.button) && now >= nextRepeat[i]) {
      n3ds_send_key(k.sym, k.scan);
      nextRepeat[i] = now + REPEAT_RATE;
    }
  }
  handleTouch(held);
  runActions();

  if (layout == LAYOUT_DUNGEON) {   // vitals and the RECAST button change without engine redraws
    static int lastShownCaster = -2;
    unsigned h = vitalsHash();
    if (h != lastVitalsHash || lastShownCaster != lastCaster) {
      lastVitalsHash = h; lastShownCaster = lastCaster; presentPending = true;
    }
  }
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
// File access
// ---------------------------------------------------------------------------
// The engine reads its data a few bytes at a time and calls fseek(0, SEEK_CUR)
// before every read, which discards stdio's buffer; on the SD card each of those
// became a separate slow access and the first dungeon load took about a minute.
// Read-only files are read in one go and served from memory instead.
extern "C" FILE *fmemopen(void *buf, size_t size, const char *mode);
FILE *n3ds_fopen(const char *name, const char *flags)
{
  bool readOnly = flags[0] == 'r' && !strchr(flags, '+');
  if (!readOnly) return fopen(name, flags);
  FILE *f = fopen(name, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { rewind(f); return f; }
  long size = ftell(f);
  rewind(f);
  if (size <= 0 || size > 16 * 1024 * 1024) return f;   // empty or huge: plain file
  // The memory stream owns its buffer (freed by fclose). One spare byte keeps
  // newlib from overwriting the last data byte with its terminating NUL.
  FILE *m = fmemopen(NULL, (size_t)size + 1, "w+b");
  if (!m) return f;
  static unsigned char chunk[64 * 1024];
  size_t total = 0, n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
    if (fwrite(chunk, 1, n, m) != n) break;
    total += n;
  }
  fclose(f);
  if (total != (size_t)size) {   // short read: fall back to the file itself
    fclose(m);
    return fopen(name, flags);
  }
  rewind(m);
  DBG("[n3ds] %s loaded into memory (%ld bytes)\n", name, size);
  return m;
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
