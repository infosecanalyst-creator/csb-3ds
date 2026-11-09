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
// Lighter versions for text on black (the game's red and blue are hard to read in the 8x8 font).
static const u16 CHAMP_TEXT[4] = {
  rgb565(70, 225, 70), rgb565(240, 225, 40), rgb565(255, 95, 95), rgb565(125, 165, 255) };

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
// Dungeon: the viewport fills the whole top screen (1.79x by 1.76x: the viewport's
// shape is within 2% of the screen's). Messages are drawn over the bottom of it
// and the party's vitals as slim bars along the left and right edges.
static const Region TOP_DUNGEON_VP  = { VP_X, VP_Y, VP_W, VP_H, 0, 0, TOP_W, TOP_H };
static const Region TOP_DUNGEON_TXT = { 0, TXT_Y, ST_W, TXT_H, (TOP_W - 320) / 2, TOP_H - TXT_H - 2, 320, TXT_H };
// Everything else (title, entrance, menus, inventory): whole screen at 1.2x, centred.
static const Region TOP_FULL        = { 0, 0, ST_W, ST_H, (TOP_W - 384) / 2, 0, 384, 240 };

// --- Bottom screen --------------------------------------------------------
// Dungeon, view on the bottom (the default): the screen you touch is the dungeon.
//   champion portraits/hands at 1:1 across the top,
//   the dungeon view at 1.3x under them (tap to grab, push, open, throw),
//   a row of finger-sized tabs along the bottom: FIGHT SPELL RECAST BAG SWAP.
// The attack menu and spell runes pop up over the right side of the view at 2x,
// only while wanted. Movement is on the D-pad / Circle Pad.
static const Region BOT_PORTRAITS = { 0, 0, ST_W, PORT_H, 0, 0, 320, PORT_H };
static const Region BOT_VIEW      = { VP_X, VP_Y, VP_W, VP_H, 0, 34, 320, 174 };   // edge to edge (1.43x by 1.28x)
// Pop-up sources measured from hardware photos: the panels start 8 px into the
// controls column; the spell panel is y 41..75, the action icons / attack menu y 77..123.
// Both sit in the lower right, just above the tabs, so the monster in front of you
// (the middle of the view) stays visible. Only one is shown at a time.
static const Region POP_FIGHT     = { 232, 77, 88, 47, 142, 113, 176, 94 };   // action icons / attack menu (2x)
static const Region POP_SPELL     = { 232, 41, 88, 35, 142, 137, 176, 70 };   // caster tabs, runes, cast (2x)
enum { TAB_Y = 210, TAB_H = 30, NUM_TABS = 6, TAB_W = BOT_W / NUM_TABS };
enum { TAB_FIGHT, TAB_SPELL, TAB_RECAST, TAB_BAG, TAB_SWAP, TAB_HELP };
static const char *const TAB_LABEL[NUM_TABS] = { "FIGHT", "SPELL", "RECAST", "BAG", "SWAP", "HELP" };
static bool manualRequested = false;   // opened from n3ds_poll, outside the touch handler
static int pressedTab = -1;            // drawn pressed while the finger is on it
static inline Rect tabRect(int i) { Rect r = { i * TAB_W, TAB_Y, TAB_W - 2, TAB_H }; return r; }
// With SWAP, the big view goes back to the top screen (with 3D depth) and the
// auto-map takes its place on the bottom.
static const Rect MAP_ON_BOTTOM = { 0, 34, BOT_W, 178 };
static bool viewOnBottom = true, showFight = false, showSpell = false;
static bool logView = false;   // ZR held: the top screen shows the whole message log
// Inventory: portraits 1:1 + the inventory panel at 1.4x, so slots are finger-sized.
static const Region BOT_INVENTORY[] = {
  { 0,    0,    ST_W, PORT_H, 0, 0,  320, PORT_H },
  { VP_X, VP_Y, VP_W, VP_H,   3, 37, 314, 190    },
};
// Everything else: the whole screen at 1:1 (exact mouse mapping), or zoomed in
// on the screen's buttons so they are finger-sized (see updateFullZoom()).
static const Region BOT_FULL_1X = { 0, 0, ST_W, ST_H, 0, (BOT_H - ST_H) / 2, ST_W, ST_H };
static Region BOT_FULL[1] = { BOT_FULL_1X };

enum Layout { LAYOUT_FULL = 0, LAYOUT_DUNGEON = 1, LAYOUT_INVENTORY = 2 };
static Layout layout = LAYOUT_FULL;
static bool presentPending = false;   // something on screen changed; draw at the next chance
static bool topDirty = true;          // the top screen needs redrawing too (not just the bottom)

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

// Text: libctru's 8x8 console font (256 glyphs, 8 bytes each, top row first,
// bit 7 = leftmost pixel). Optional integer scale.
extern "C" const u8 default_font_bin[];
static int drawText(u16 *fb, int w, int x, int y, const char *s, u16 c, int scale = 1)
{
  for (; *s; s++, x += 8 * scale) {
    const u8 *g = default_font_bin + 8 * (unsigned char)*s;
    for (int r = 0; r < 8; r++)
      for (int b = 0; b < 8; b++)
        if (g[r] & (0x80 >> b)) {
          if (scale == 1) { int px = x + b, py = y + r; if (px >= 0 && px < w && py >= 0 && py < 240) putpx(fb, px, py, c); }
          else fillRect(fb, w, x + b * scale, y + r * scale, scale, scale, c);
        }
  }
  return x;
}

// ---------------------------------------------------------------------------
// Touch-button click: a very quiet, short tick mixed into the game's own audio
// (the patched SoundMixer.cpp calls n3ds_mix_click from its SDL audio callback).
// ---------------------------------------------------------------------------
static volatile int clickSamplesLeft = 0;   // set by the game thread, consumed by the audio thread
enum { CLICK_MS = 6, CLICK_PEAK = 900 };     // about -31 dB: just audible
static void playClick(void) { clickSamplesLeft = -1; }   // the mixer converts -1 into a fresh click
void n3ds_mix_click(short *stream, int frames, int channels, int freq)
{
  int left = clickSamplesLeft;
  if (left == 0 || channels < 1 || freq <= 0) return;
  int total = freq * CLICK_MS / 1000;
  if (left < 0) left = total;
  for (int i = 0; i < frames && left > 0; i++, left--) {
    int pos = total - left;                      // 0 .. total-1
    int env = CLICK_PEAK * left / total;         // linear decay
    int s = ((pos / (freq / 2000 + 1)) & 1) ? env : -env;   // ~1 kHz square burst
    for (int c = 0; c < channels; c++) {
      int v = stream[i * channels + c] + s;
      stream[i * channels + c] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
  }
  clickSamplesLeft = left;
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
struct Action { int kind, a, b; u64 dueTicks; };   // kind 0 = key(sym=a, scan=b), 1 = right-click, 2 = left-click at (a,b)
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
static inline void queueClick(int x, int y, int delayMs) { queueAction(2, x, y, delayMs); }
// A scripted sequence is queued whole or not at all, so a busy queue never cuts
// one short (a recast without its final "cast" would leave stray runes behind).
static inline bool queueHasRoom(int n) { return actCount + n <= (int)(sizeof(actQueue) / sizeof(actQueue[0])); }

static bool touching = false, touchRight = false, touchOnButton = false;
static int lastSTX = 0, lastSTY = 0;   // where the last tap put the game cursor

static u64 parkCursorAt = 0;   // when to move the arrow out of the way (0 = no)
// The message-text rows (y 169..199) have no buttons and are not shown on the bottom
// screen. (The old spot right of the portraits is the party-formation box once four
// champions have joined, where the cursor turns into a hand.)
static const int PARK_X = 318, PARK_Y = 198;

static bool clickPending = false, clickRight = false;
static int clickX, clickY;

static void runActions(void)
{
  if (parkCursorAt && svcGetSystemTick() >= parkCursorAt) {
    parkCursorAt = 0;
    if (!touching && n3ds_engine_cursor_is_arrow()) {
      n3ds_send_mouse_move(PARK_X, PARK_Y);
      lastSTX = PARK_X; lastSTY = PARK_Y;
    }
  }
  if (clickPending) {   // release the button one poll after pressing it
    n3ds_send_mouse_button(clickRight, false, clickX, clickY);
    n3ds_send_mouse_move(lastSTX, lastSTY);   // put the cursor back where the player left it
    clickPending = false;
    return;   // one click per poll: the engine sees press and release separately
  }
  u64 now = svcGetSystemTick();
  while (actCount && actQueue[actHead].dueTicks <= now) {
    Action &a = actQueue[actHead];
    if (a.kind == 0) {
      n3ds_send_key(a.a, a.b);
    } else {
      clickRight = a.kind == 1;
      n3ds_send_mouse_move(a.a, a.b);
      n3ds_send_mouse_button(clickRight, true, a.a, a.b);
      clickX = a.a; clickY = a.b; clickPending = true;
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
  if (champ >= n3ds_engine_party_size() || !queueHasRoom(3)) return;
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

// Replay a champion's last spell: select their caster tab (F1..F4), remove any runes
// already typed (the arrow beside the rune line), enter the runes (1..6), cast (space).
static const int RUNE_DELETE_X = 0x138, RUNE_DELETE_Y = 0x44;   // the panel's back-arrow
static void recastSpell(int caster)
{
  if (caster < 0 || caster >= n3ds_engine_party_size() || !lastSpell[caster][0]) return;
  const signed char *inc = lastSpell[caster];
  int typed = n3ds_engine_rune_count(caster);
  if (!queueHasRoom(1 + typed + 4 + 1)) return;
  queueKey(0x4000003a + caster, SC_F1 + caster, 0);                 // SDLK_F1.. / caster tab
  for (int k = 0; k < typed; k++) queueClick(RUNE_DELETE_X, RUNE_DELETE_Y, 110);
  for (int i = 0; i < 4 && inc[i]; i++) {
    int button = inc[i] - 96 - 6 * i;                               // see Magic.cpp / CSBCode.cpp
    if (button < 0 || button > 5) break;
    queueKey('1' + button, SC_1 + button, 110);
  }
  queueKey(' ', SC_SPACE, 140);
  DBG("[n3ds] recast: caster %d (cleared %d runes)\n", caster, typed);
}
static void recastLastSpell(void) { recastSpell(lastCaster); }

// Right-click in the viewport toggles the leader's inventory (DM's own mouse convention).
static void toggleLeaderInventory(void) { queueAction(1, 100, 100, 0); }

// STOW (the BAG tab while something is in your hand): open the leader's
// inventory, drop the item in their first free backpack slot, close it again.
static void stowHeldItem(void)
{
  int leader = n3ds_engine_leader();
  int slot = leader >= 0 ? n3ds_engine_free_backpack_slot(leader) : -1;
  int x, y;
  if (slot < 0 || !n3ds_engine_slot_pos(slot, &x, &y) || !queueHasRoom(3)) {
    n3ds_note_message(15, "\nNO ROOM IN THE BACKPACK.");
    return;
  }
  queueAction(1, 100, 100, 0);   // open the leader's inventory
  queueClick(x, y, 160);         // put the item in the free slot
  queueAction(1, 100, 100, 160); // close it
  DBG("[n3ds] stow into slot %d at (%d,%d)\n", slot, x, y);
}

// Eat or drink with a double-tap in the inventory: the first tap picked the item
// up, so feed it to the mouth, then tap the same slot again, which puts back
// whatever is left (an empty flask) or does nothing if it was eaten.
static void eatFromSlot(int x, int y)
{
  int mx, my;
  if (!n3ds_engine_mouth_pos(&mx, &my) || !queueHasRoom(2)) return;
  queueClick(mx, my, 0);
  queueClick(x, y, 160);
  DBG("[n3ds] eat/drink from (%d,%d) via mouth (%d,%d)\n", x, y, mx, my);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Viewport at 1.5x with per-pixel horizontal disparity from the depth map. The source
// column for every output pixel only depends on the slider, so it is cached per eye
// and rebuilt when the slider moves (no float math per frame).
enum { SV_W = TOP_W, SV_H = TOP_H };
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

// Party vitals over the edges of the view: champions 1-2 down the left edge, 3-4 down
// the right; three slim vertical bars each (health, stamina, mana) in the champion's
// colour on a dark backing; the health bar's backing turns red below a quarter.
static void drawVitals(u16 *fb)
{
  int n = n3ds_engine_party_size();
  const u16 back = rgb565(24, 24, 24), danger = rgb565(150, 0, 0);
  const int barW = 3, step = 4, boxW = 3 * step + 1;
  for (int i = 0; i < n && i < 4; i++) {
    int stats[6];
    n3ds_engine_champion_stats(i, stats);   // hp, maxHp, stamina, maxStamina, mana, maxMana
    int x0 = (i < 2) ? 0 : TOP_W - boxW;
    int y0 = (i % 2) * 118 + 4;
    const int barH = 96;
    fillRect(fb, TOP_W, x0, y0, boxW, barH + 8, back);                 // backing
    fillRect(fb, TOP_W, x0 + 1, y0 + 1, boxW - 2, 3, CHAMP_COLOR[i]);  // colour tab = party slot
    for (int b = 0; b < 3; b++) {
      int cur = stats[b * 2], max = stats[b * 2 + 1];
      int bx = x0 + 1 + b * step, by = y0 + 6;
      bool low = (b == 0 && max > 0 && cur * 4 < max && cur > 0);
      if (low) fillRect(fb, TOP_W, bx, by, barW, barH, danger);
      if (max > 0 && cur > 0) {
        int h = cur >= max ? barH : cur * barH / max;
        if (h < 1) h = 1;
        fillRect(fb, TOP_W, bx, by + barH - h, barW, h, CHAMP_COLOR[i]);
      }
    }
  }
}

// The game's message lines (y 169..199, coloured text on black) drawn over the bottom
// of the view: only the text pixels, each with a dark outline so it stays readable
// on any wall or floor. Nothing is drawn when there are no messages.
static void drawMessages(u16 *fb)
{
  const Region &r = TOP_DUNGEON_TXT;
  static unsigned char mask[TXT_H + 2][ST_W + 2];
  bool any = false;
  memset(mask, 0, sizeof mask);
  for (int y = 0; y < TXT_H; y++)
    for (int x = 0; x < ST_W; x++)
      if (n3ds_st_buffer[(r.sy + y) * ST_W + x] != 0) { mask[y + 1][x + 1] = 1; any = true; }
  if (!any) return;
  const u16 shadow = rgb565(0, 0, 0);
  for (int y = 0; y < TXT_H + 2; y++)
    for (int x = 0; x < ST_W + 2; x++) {
      int dx = r.dx + x - 1, dy = r.dy + y - 1;
      if (dx < 0 || dx >= TOP_W || dy < 0 || dy >= TOP_H) continue;
      if (mask[y][x]) putpx(fb, dx, dy, n3ds_st_buffer[(r.sy + y - 1) * ST_W + x - 1]);
      else if ((y > 0 && mask[y - 1][x]) || (y < TXT_H + 1 && mask[y + 1][x]) ||
               (x > 0 && mask[y][x - 1]) || (x < ST_W + 1 && mask[y][x + 1]))
        putpx(fb, dx, dy, shadow);
    }
}

// ---------------------------------------------------------------------------
// Message log: every line the game prints (hooked in CSBCode.cpp PrintLines).
// The game itself shows four lines that fade; the log keeps the last sixteen.
// ---------------------------------------------------------------------------
enum { LOG_LINES = 32, LOG_COLS = 49 };
static char logText[LOG_LINES][LOG_COLS + 1];
static u16 logColor[LOG_LINES];
static int logHead = 0, logCount = 0;

static u16 messageColor(int idx)
{
  for (int i = 0; i < 4; i++) if (idx == n3ds_engine_char_color(i)) return CHAMP_TEXT[i];
  if (idx == 15) return rgb565(255, 255, 255);
  if (idx == 4)  return rgb565(0, 200, 210);
  return rgb565(200, 200, 200);
}

static void logNewLine(u16 c)
{
  logHead = (logHead + 1) % LOG_LINES;
  logText[logHead][0] = 0;
  logColor[logHead] = c;
  if (logCount < LOG_LINES) logCount++;
}

void n3ds_note_message(int color, const char *text)
{
  if (!text) return;
  u16 c = messageColor(color);
  if (logCount == 0) logNewLine(c);
  for (; *text; text++) {
    char ch = *text;
    char *line = logText[logHead];
    int len = (int)strlen(line);
    if (ch == '\n') { if (len) logNewLine(c); continue; }
    if (len == 0) { logColor[logHead] = c; if (ch == ' ') continue; }
    if (len >= LOG_COLS) {   // wrap at the last space; continuation lines are indented
      char carry[LOG_COLS + 1] = "";
      char *sp = strrchr(line, ' ');
      if (ch != ' ' && sp && sp - line > 2) { strcpy(carry, sp + 1); *sp = 0; }
      u16 lc = logColor[logHead];
      logNewLine(lc);
      line = logText[logHead];
      snprintf(line, LOG_COLS + 1, "  %s", carry);
      len = (int)strlen(line);
      if (ch == ' ') continue;
    }
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if ((unsigned char)ch < 32) ch = ' ';
    line[len] = ch; line[len + 1] = 0;
  }
  presentPending = topDirty = true;
}

static void drawLog(u16 *fb, int w, int x, int y, int lines, int lineH)
{
  int shown = 0;
  for (int k = 0; k < logCount && shown < lines; k++) {   // newest at the bottom
    int idx = (logHead - k + LOG_LINES) % LOG_LINES;
    if (!logText[idx][0] && k == 0) continue;
    u16 c = logColor[idx];
    if (shown >= 3) {   // older lines dimmer
      int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      c = (u16)(((r * 3 / 4) << 11) | ((g * 3 / 4) << 5) | (b * 3 / 4));
    }
    drawText(fb, w, x, y + (lines - 1 - shown) * lineH, logText[idx], c);
    shown++;
  }
}

// ---------------------------------------------------------------------------
// Auto-map: squares the party has seen, per level. Dungeon Master has no map of
// its own, so this is the biggest addition. Marked from the engine's cell data
// (what the view shows: up to three squares ahead, and one either side).
// ---------------------------------------------------------------------------
enum { MAP_LEVELS = 64, MAP_DIM = 32 };
static unsigned char seen[MAP_LEVELS][MAP_DIM][MAP_DIM];
static int lastMapPos[4] = { -1, -1, -1, -1 };

enum { ROOM_STONE = 0, ROOM_OPEN = 1, ROOM_PIT = 2, ROOM_STAIRS = 3, ROOM_DOOR = 4,
       ROOM_TELEPORTER = 5, ROOM_FALSEWALL = 6 };

static void markSeen(int l, int x, int y, int w, int h)
{
  if (l >= 0 && l < MAP_LEVELS && x >= 0 && y >= 0 && x < w && y < h && x < MAP_DIM && y < MAP_DIM) seen[l][x][y] = 1;
}

static bool blocksView(int cf)
{
  if (cf < 0) return true;
  int type = cf >> 5;
  if (type == ROOM_STONE || type == ROOM_FALSEWALL) return true;
  if (type == ROOM_DOOR) { int s = cf & 7; return s != 0 && s != 5; }   // closed (5 = broken)
  return false;
}

static void updateAutomap(void)
{
  int p[4];
  n3ds_engine_party(p);
  if (memcmp(p, lastMapPos, sizeof p) == 0) return;
  memcpy(lastMapPos, p, sizeof p);
  int l = p[0], x = p[1], y = p[2], f = p[3], w, h;
  if (!n3ds_engine_level_size(l, &w, &h)) return;
  int fx, fy, rx, ry;
  n3ds_engine_delta(f, &fx, &fy);
  n3ds_engine_delta(f + 1, &rx, &ry);
  for (int d = 0; d <= 3; d++) {
    int cx = x + fx * d, cy = y + fy * d;
    for (int lat = -1; lat <= 1; lat++) markSeen(l, cx + rx * lat, cy + ry * lat, w, h);
    if (d > 0 && blocksView(n3ds_engine_cell(l, cx, cy))) break;
  }
  presentPending = topDirty = true;
}

static void drawAutomap(u16 *fb, int fbW, const Rect &area, int shift, int arrowShift)
{
  const u16 bg = rgb565(10, 10, 14), grid = rgb565(70, 72, 80), floorC = rgb565(118, 116, 108);
  const u16 wallLine = rgb565(245, 235, 205);
  const u16 wallC = rgb565(40, 34, 28), pitC = rgb565(16, 16, 16), stairC = rgb565(214, 190, 90);
  const u16 doorC = rgb565(205, 120, 40), partyC = rgb565(255, 255, 255);
  fillRect(fb, fbW, area.x, area.y, area.w, area.h, bg);
  int p[4];
  n3ds_engine_party(p);
  int l = p[0], px = p[1], py = p[2], f = p[3], w, h;
  if (!n3ds_engine_level_size(l, &w, &h) || l < 0 || l >= MAP_LEVELS) return;
  const int C = 10;   // pixels per square
  int cols = area.w / C, rows = area.h / C;
  int ox = area.x + (area.w - cols * C) / 2 + shift, oy = area.y + (area.h - rows * C) / 2;
  int x0 = px - cols / 2, y0 = py - rows / 2;   // keep the party in the middle
  for (int gy = 0; gy < rows; gy++)
    for (int gx = 0; gx < cols; gx++) {
      int mx = x0 + gx, my = y0 + gy;
      int sx = ox + gx * C, sy = oy + gy * C;
      if (mx < 0 || my < 0 || mx >= w || my >= h || mx >= MAP_DIM || my >= MAP_DIM) continue;
      if (!seen[l][mx][my]) continue;
      int cf = n3ds_engine_cell(l, mx, my);
      int type = cf < 0 ? 0 : cf >> 5;
      if (type == ROOM_STONE || type == ROOM_FALSEWALL) {   // false walls look like walls
        fillRect(fb, fbW, sx, sy, C, C, wallC);
        continue;
      }
      fillRect(fb, fbW, sx, sy, C, C, floorC);
      // Edges, graph-paper style: solid where a wall is, dotted where the way is open.
      for (int side = 0; side < 4; side++) {
        static const int NX[4] = { 0, 1, 0, -1 }, NY[4] = { -1, 0, 1, 0 };   // up right down left
        int ncf = n3ds_engine_cell(l, mx + NX[side], my + NY[side]);
        int nt = ncf < 0 ? ROOM_STONE : ncf >> 5;
        bool wall = nt == ROOM_STONE || nt == ROOM_FALSEWALL;
        bool horiz = side == 0 || side == 2;
        int ex = side == 1 ? sx + C - (wall ? 2 : 1) : sx;
        int ey = side == 2 ? sy + C - (wall ? 2 : 1) : sy;
        if (wall) {
          if (horiz) fillRect(fb, fbW, ex, ey, C, 2, wallLine);
          else fillRect(fb, fbW, ex, ey, 2, C, wallLine);
        } else if (side == 1 || side == 2) {   // open: one dotted line per shared edge
          for (int k = 0; k < C; k += 2) {
            int X = horiz ? ex + k : ex, Y = horiz ? ey : ey + k;
            if (X >= 0 && X < fbW && Y >= 0 && Y < 240) putpx(fb, X, Y, grid);
          }
        }
      }
      if (type == ROOM_PIT) fillRect(fb, fbW, sx + 3, sy + 3, C - 6, C - 6, pitC);
      else if (type == ROOM_STAIRS) for (int k = 2; k < C - 2; k += 2) fillRect(fb, fbW, sx + 2, sy + k, C - 4, 1, stairC);
      else if (type == ROOM_DOOR) {   // a bar across the passage
        int left = n3ds_engine_cell(l, mx - 1, my), right = n3ds_engine_cell(l, mx + 1, my);
        bool nsPassage = (left < 0 || (left >> 5) == ROOM_STONE) && (right < 0 || (right >> 5) == ROOM_STONE);
        bool closed = (cf & 7) != 0 && (cf & 7) != 5;
        u16 dc = closed ? doorC : rgb565(120, 80, 40);
        if (nsPassage) fillRect(fb, fbW, sx + 1, sy + C / 2 - 1, C - 1, 2, dc);
        else fillRect(fb, fbW, sx + C / 2 - 1, sy + 1, 2, C - 1, dc);
      }
    }
  // The party: an arrow filling its square, pointing the way it faces. The tip is
  // red and the body white with a dark outline, so the direction reads at a glance.
  int fx, fy;
  n3ds_engine_delta(f, &fx, &fy);
  int cx = ox + (px - x0) * C + C / 2 + arrowShift, cy = oy + (py - y0) * C + C / 2;
  const u16 tipC = rgb565(255, 50, 30), outline = rgb565(0, 0, 0);
  for (int pass = 0; pass < 2; pass++)       // 0: outline (one pixel larger), 1: fill
    for (int a = -6; a <= 6; a++)            // along the facing (+ = forward)
      for (int b = -6; b <= 6; b++) {        // across it
        int grow = pass == 0 ? 1 : 0;
        // A triangle from the tail (a = -3, half-width 4) to the tip (a = +4); front half red.
        if (a < -3 - grow || a > 4 + grow) continue;
        int half = (4 - a) * 4 / 7 + grow;
        if (abs(b) > half) continue;
        int dx = fx * a - fy * b, dy = fy * a + fx * b;
        int X = cx + dx, Y = cy + dy;
        if (X < area.x || X >= area.x + area.w || Y < area.y || Y >= area.y + area.h) continue;
        putpx(fb, X, Y, pass == 0 ? outline : (a >= 0 ? tipC : partyC));
      }
  char label[24];
  snprintf(label, sizeof label, "LEVEL %d", l + 1);
  drawText(fb, fbW, area.x + 3, area.y + 3, label, rgb565(220, 210, 180));
}

// ---------------------------------------------------------------------------
// Top screen information panel (while the view is on the bottom, and with the
// inventory open): party, auto-map, compass and light, message log.
// ---------------------------------------------------------------------------
static const Rect INFO_MAP = { 160, 0, 240, 176 };
enum { INFO_PARTY_W = 156, INFO_BLOCK_H = 44, INFO_LOG_Y = 178, INFO_LOG_LINES = 7, INFO_LOG_H = 9 };

static void drawBar(u16 *fb, int x, int y, int w, int h, int cur, int max, u16 c, u16 back)
{
  fillRect(fb, TOP_W, x, y, w, h, back);
  if (max > 0 && cur > 0) {
    int len = cur >= max ? w : cur * w / max;
    if (len < 1) len = 1;
    fillRect(fb, TOP_W, x, y, len, h, c);
  }
}

static bool leftyNow();   // left-handed mode (defined with the remap below)
static void drawParty(u16 *fb)
{
  int n = n3ds_engine_party_size();
  const u16 back = rgb565(36, 36, 40), text = rgb565(225, 220, 205);
  if (n == 0) {
    drawText(fb, TOP_W, 4, 8, "NO PARTY YET", text);
    drawText(fb, TOP_W, 4, 22, "TAP A MIRROR", rgb565(150, 145, 130));
    drawText(fb, TOP_W, 4, 32, "TO MEET A", rgb565(150, 145, 130));
    drawText(fb, TOP_W, 4, 42, "CHAMPION", rgb565(150, 145, 130));
    return;
  }
  // Each champion: name in their party colour, then labelled bars with current/max:
  //   HP  health (green; red below a quarter)   STA stamina (amber)   MP mana (blue)
  //   FOOD and WATER bars (turn yellow, then red, when running low).
  const u16 label = rgb565(165, 158, 140);
  const u16 hpC = rgb565(60, 200, 70), hpLow = rgb565(230, 40, 30);
  const u16 staC = rgb565(235, 170, 40), mpC = rgb565(70, 130, 255);
  static const char *const NAMES[3] = { "HP", "STA", "MP" };
  for (int i = 0; i < n && i < 4; i++) {
    int y0 = i * INFO_BLOCK_H + 1;
    int s[6], e[4];
    n3ds_engine_champion_stats(i, s);
    n3ds_engine_champion_extra(i, e);   // food, water, poison, busy
    bool alive = s[0] > 0;
    if (i > 0) fillRect(fb, TOP_W, 0, y0 - 1, INFO_PARTY_W, 1, rgb565(60, 54, 44));   // divider
    // The button that attacks with this champion: lit in their colour when the
    // weapon is ready, grey while it recovers.
    // Left-handed mode: the D-pad direction that now attacks (A=right, B=down, X=left, Y=up).
    static const char *const BUTTON[4] = { "A", "B", "X", "Y" }, *const LBUTTON[4] = { ">", "v", "<", "^" };
    bool ready = alive && !e[3];
    fillRect(fb, TOP_W, 1, y0 - 1, 10, 10, ready ? CHAMP_COLOR[i] : rgb565(70, 70, 70));
    drawText(fb, TOP_W, 2, y0, leftyNow() ? LBUTTON[i] : BUTTON[i], ready ? rgb565(0, 0, 0) : rgb565(150, 150, 150));
    drawText(fb, TOP_W, 14, y0, n3ds_engine_champion_name(i), alive ? CHAMP_TEXT[i] : rgb565(110, 100, 100));
    if (i == n3ds_engine_leader()) drawText(fb, TOP_W, 76, y0, "LEAD", rgb565(245, 200, 90));
    if (!alive) drawText(fb, TOP_W, INFO_PARTY_W - 32, y0, "DEAD", rgb565(230, 70, 70));
    else if (e[2]) drawText(fb, TOP_W, INFO_PARTY_W - 48, y0, "POISON", rgb565(60, 220, 60));
    for (int b = 0; b < 3; b++) {
      int by = y0 + 10 + b * 8, cur = s[b * 2], max = s[b * 2 + 1];
      if (b == 1) { cur /= 10; max /= 10; }   // the game shows stamina divided by ten
      bool low = b == 0 && max > 0 && cur * 4 < max;
      u16 c = b == 0 ? (low ? hpLow : hpC) : b == 1 ? staC : mpC;
      drawText(fb, TOP_W, 2, by, NAMES[b], label);
      if (max <= 0) { drawText(fb, TOP_W, 30, by, "-", label); continue; }   // no mana: not a caster yet
      drawBar(fb, 30, by + 1, 52, 6, cur, max, c, back);
      char num[16];
      snprintf(num, sizeof num, "%d/%d", cur, max);
      drawText(fb, TOP_W, 86, by, num, text);
    }
    // Food and water: the game's scale runs -1024..2048; yellow below 0, red below -512.
    static const char *const FW[2] = { "FOOD", "WATER" };
    int fx = 2;
    for (int k = 0; k < 2; k++) {
      int v = e[k];
      u16 c = v < -512 ? rgb565(230, 40, 30) : v < 0 ? rgb565(235, 210, 0) : (k ? rgb565(80, 150, 255) : rgb565(190, 130, 60));
      fx = drawText(fb, TOP_W, fx, y0 + 34, FW[k], label) + 2;
      drawBar(fb, fx, y0 + 35, 30, 6, v + 1024, 3072, c, back);
      fx += 36;
    }
  }
}

static void drawCompassAndLight(u16 *fb, const Rect &m)
{
  int p[4];
  n3ds_engine_party(p);
  static const char *const DIRS[4] = { "N", "E", "S", "W" };
  char buf[16];
  snprintf(buf, sizeof buf, "FACING %s", DIRS[p[3] & 3]);
  drawText(fb, TOP_W, m.x + m.w - 8 * 8 - 3, m.y + 3, buf, rgb565(220, 210, 180));
  // Light: 0 (dark) .. 5 (bright) from the palette the engine picked.
  int light = 5 - n3ds_engine_light_level();
  if (light < 0) light = 0;
  if (light > 5) light = 5;
  drawText(fb, TOP_W, m.x + 3, m.y + m.h - 11, "LIGHT", rgb565(220, 210, 180));
  for (int k = 0; k < 5; k++)
    fillRect(fb, TOP_W, m.x + 46 + k * 9, m.y + m.h - 10, 7, 7,
             k < light ? rgb565(250, 200, 60) : rgb565(50, 46, 40));
}

static void drawInfo(u16 *fb, float eye, float slider)
{
  const u16 rule = rgb565(70, 62, 50);
  drawParty(fb);
  fillRect(fb, TOP_W, INFO_PARTY_W + 1, 0, 1, INFO_LOG_Y - 2, rule);
  fillRect(fb, TOP_W, 0, INFO_LOG_Y - 2, TOP_W, 1, rule);
  // 3D: the map sits slightly behind the screen and the party arrow in front of it.
  int shift = (int)floorf(-eye * slider * 2.0f + 0.5f);
  int arrow = (int)floorf(eye * slider * 1.5f + 0.5f);
  drawAutomap(fb, TOP_W, INFO_MAP, shift, arrow - shift);
  drawCompassAndLight(fb, INFO_MAP);
  drawLog(fb, TOP_W, 4, INFO_LOG_Y, INFO_LOG_LINES, INFO_LOG_H);
}

static void drawTop(u16 *fb, float eye /* -1 left, +1 right */, float slider)
{
  memset(fb, 0, TOP_W * TOP_H * 2);
  if (logView) {
    fillRect(fb, TOP_W, 0, 0, TOP_W, 12, rgb565(70, 52, 30));
    drawText(fb, TOP_W, 4, 2, "MESSAGES  (RELEASE ZL TO GO BACK)", rgb565(245, 200, 90));
    drawLog(fb, TOP_W, 4, 16, 22, 10);
  } else if ((layout == LAYOUT_DUNGEON && viewOnBottom) || layout == LAYOUT_INVENTORY) {
    drawInfo(fb, eye, slider);
  } else if (layout == LAYOUT_DUNGEON) {
    blitViewportStereo(fb, TOP_DUNGEON_VP, eye, slider);
    drawMessages(fb);
    drawVitals(fb);
  } else {
    blitRegion(fb, TOP_FULL);
  }
}

static bool fightShown(void) { return showFight || n3ds_engine_attack_menu_owner() != 0; }

static const Region *bottomRegions(int *count)
{
  if (layout == LAYOUT_DUNGEON) {   // pop-ups first: they sit on top of the view
    static Region list[4];
    int n = 0;
    if (fightShown()) list[n++] = POP_FIGHT;
    else if (showSpell) list[n++] = POP_SPELL;
    list[n++] = BOT_PORTRAITS;
    if (viewOnBottom) list[n++] = BOT_VIEW;
    *count = n;
    return list;
  }
  if (layout == LAYOUT_INVENTORY) { *count = 2; return BOT_INVENTORY; }
  *count = 1;
  return BOT_FULL;
}

static bool handFull(void) { return layout == LAYOUT_DUNGEON && n3ds_engine_hand_full(); }

// RECAST held: one button per champion with a remembered spell, above the tabs.
static bool recastHeld = false, recastChooser = false;
static u64 recastSince = 0;
static Rect chooserButton(int c) { Rect r = { c * 80 + 1, 168, 78, 38 }; return r; }

static void drawRecastChooser(u16 *fb)
{
  fillRect(fb, BOT_W, 0, 164, BOT_W, 46, rgb565(0, 0, 0));
  for (int c = 0; c < 4; c++) {
    Rect r = chooserButton(c);
    bool has = c < n3ds_engine_party_size() && lastSpell[c][0];
    fillRect(fb, BOT_W, r.x, r.y, r.w, r.h, has ? rgb565(52, 44, 36) : rgb565(26, 24, 22));
    frameRect(fb, BOT_W, r.x, r.y, r.w, r.h, has ? CHAMP_COLOR[c] : rgb565(60, 60, 60));
    if (c >= n3ds_engine_party_size()) continue;
    char name[9];
    strncpy(name, n3ds_engine_champion_name(c), 8); name[8] = 0;
    drawText(fb, BOT_W, r.x + (r.w - (int)strlen(name) * 8) / 2, r.y + 6, name, has ? CHAMP_TEXT[c] : rgb565(90, 90, 90));
    for (int k = 0; k < 4 && lastSpell[c][k]; k++)
      fillRect(fb, BOT_W, r.x + (r.w - 4 * 10) / 2 + k * 10 + 1, r.y + 22, 7, 7, CHAMP_COLOR[c]);
  }
}

static void drawTabs(u16 *fb)
{
  const u16 face = rgb565(52, 44, 36), on = rgb565(110, 84, 40), edge = rgb565(150, 130, 100);
  const u16 text = rgb565(235, 225, 195), dim = rgb565(110, 100, 90);
  bool party = n3ds_engine_party_size() > 0;
  bool haveSpell = lastCaster >= 0 && lastCaster < n3ds_engine_party_size();
  if (recastChooser) drawRecastChooser(fb);
  for (int i = 0; i < NUM_TABS; i++) {
    Rect r = tabRect(i);
    bool lit = (i == TAB_FIGHT && fightShown()) || (i == TAB_SPELL && showSpell && !fightShown()) ||
               (i == TAB_RECAST && recastChooser);
    bool live = i == TAB_SWAP || i == TAB_HELP || (i == TAB_RECAST ? haveSpell : party);
    bool down = i == pressedTab;    // pressed: darker, sunk by a pixel, light edge on the bottom
    fillRect(fb, BOT_W, r.x, r.y, r.w, r.h, down ? rgb565(30, 25, 20) : lit ? on : face);
    frameRect(fb, BOT_W, r.x, r.y, r.w, r.h, down ? rgb565(90, 78, 60) : edge);
    if (down) fillRect(fb, BOT_W, r.x + 1, r.y + r.h - 2, r.w - 2, 1, edge);
    const char *label = (i == TAB_BAG && handFull()) ? "STOW" : TAB_LABEL[i];
    int len = (int)strlen(label);
    drawText(fb, BOT_W, r.x + (r.w - len * 8) / 2, r.y + 8 + (down ? 1 : 0), label, live ? text : dim);
    if (i == TAB_RECAST && haveSpell) {   // whose spell, and how many runes
      fillRect(fb, BOT_W, r.x + 2, r.y + 2, r.w - 4, 2, CHAMP_COLOR[lastCaster]);   // caster colour along the top
      for (int k = 0; k < 4 && lastSpell[lastCaster][k]; k++)
        fillRect(fb, BOT_W, r.x + (r.w - 4 * 8) / 2 + k * 8 + 1, r.y + 20, 5, 4, CHAMP_COLOR[lastCaster]);
    }
  }
}

static void framePopup(u16 *fb, const Region &r)
{
  frameRect(fb, BOT_W, r.dx - 2, r.dy - 2, r.dw + 4, r.dh + 4, rgb565(0, 0, 0));
  frameRect(fb, BOT_W, r.dx - 1, r.dy - 1, r.dw + 2, r.dh + 2, rgb565(200, 170, 110));
}

static void drawAutomap(u16 *fb, int fbW, const Rect &area, int shift, int arrowShift);

static void drawBottom(u16 *fb)
{
  memset(fb, 0, BOT_W * BOT_H * 2);
  int n;
  const Region *regs = bottomRegions(&n);
  if (layout == LAYOUT_DUNGEON && !viewOnBottom) drawAutomap(fb, BOT_W, MAP_ON_BOTTOM, 0, 0);
  for (int i = n - 1; i >= 0; i--) blitRegion(fb, regs[i]);   // back to front
  if (layout == LAYOUT_DUNGEON) {
    if (fightShown()) framePopup(fb, POP_FIGHT);
    else if (showSpell) framePopup(fb, POP_SPELL);
    drawTabs(fb);
  }
  if (dbgEnabled && dbgTX >= 0) {   // debug: magenta crosshair where the last tap landed
    for (int d = -4; d <= 4; d++) {
      if (dbgTX + d >= 0 && dbgTX + d < BOT_W) putpx(fb, dbgTX + d, dbgTY, 0xF81F);
      if (dbgTY + d >= 0 && dbgTY + d < BOT_H) putpx(fb, dbgTX, dbgTY + d, 0xF81F);
    }
  }
}

// The portrait strip is drawn 1:1, where the hands are 16 px and the name strip
// 7 px: stylus targets. Snap a tap anywhere in a champion's 69 px box to the
// nearest hotspot, so each one is a finger-sized area. From the game's own button
// table (graphic 561): left-click the name (y 0..6) = make leader, left-click a
// hand (16x16 at x+4 / x+24, y 10) = take or place, and a right-click anywhere in
// the box opens that champion's inventory. So the bars area becomes a right-click.
static bool portraitWantsRight = false;   // set by touchToST for the bars zone
static void snapPortraitTap(int *stx, int *sty)
{
  int champ = *stx / 69;
  if (champ >= n3ds_engine_party_size() || champ > 3) return;
  int lx = *stx - champ * 69, base = champ * 69;
  if (*sty < 9)        { *stx = base + 20; *sty = 3; }
  else if (lx < 22)    { *stx = base + 12; *sty = 18; }
  else if (lx < 43)    { *stx = base + 32; *sty = 18; }
  else                 { *stx = base + 54; *sty = 14; portraitWantsRight = true; }
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
  portraitWantsRight = false;
  for (int i = 0; i < n; i++) {
    const Region &r = regs[i];
    if (px < r.dx || px >= r.dx + r.dw || py < r.dy || py >= r.dy + r.dh) continue;
    *stx = r.sx + (px - r.dx) * r.sw / r.dw;
    *sty = r.sy + (py - r.dy) * r.sh / r.dh;
    if (r.sy == 0 && r.sh == PORT_H && layout != LAYOUT_FULL) snapPortraitTap(stx, sty);
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
static float lastSlider = -1.0f;
// The slider reading can flicker between neighbouring values; 32 steps is plenty.
static inline int sliderStep(float s) { return s < 0 ? -1 : (int)(s * 32.0f + 0.5f); }
static unsigned lastVitalsHash = 0;

static volatile u32 vblankCount = 0;
static void onVBlank(void *) { vblankCount++; }

static void flushPresent(bool force)
{
  // At most one present per display frame. A swap takes effect at the next
  // vertical blank; until then the "back" buffer is still the one on screen, so
  // drawing into it flickers. A VBlank counter (bumped by a GSP event callback)
  // tells us whether the last swap has happened, without making the engine wait
  // for the display (waiting slowed the game down whenever it redrew often).
  // A frame that arrives too early stays pending and n3ds_poll() presents it.
  static u32 lastSwapVBlank = 0xffffffff;
  static u64 lastSwapTick = 0;
  u64 now = svcGetSystemTick();
  bool landed = vblankCount != lastSwapVBlank ||
                now - lastSwapTick > (u64)SYSCLOCK_ARM11 / 20;   // 50 ms: safe even if no callback ever arrives
  if (!landed) {
    if (!force) return;
    gspWaitForVBlank();   // forced: wait for the previous swap to land
  }
  presentPending = false;
  dbgPresents++;

  drawBottom((u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
  if (topDirty) {
    topDirty = false;
    float slider = sliderStep(osGet3DSliderState()) / 32.0f;
    lastSlider = slider;
    drawTop((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), -1.0f, slider);
    drawTop((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL), +1.0f, slider);
    gfxFlushBuffers();
    gfxSwapBuffers();
  } else {
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
  }
  lastSwapVBlank = vblankCount;
  lastSwapTick = svcGetSystemTick();
}

// The engine changed its picture. The top screen only shows it in SWAP mode and on
// menus; the dashboard (party, map, log) has its own change tracking, so a moving
// cursor or an animation redraws just the bottom screen.
void n3ds_present(void)
{
  presentPending = true;
  if (layout == LAYOUT_FULL || (layout == LAYOUT_DUNGEON && !viewOnBottom)) topDirty = true;
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
    presentPending = topDirty = true;
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

static void saveSettings(void);
static void pressTab(int i)
{
  DBG("[n3ds] tab %s\n", TAB_LABEL[i]);
  playClick();
  pressedTab = i;
  switch (i) {
  case TAB_FIGHT:
    if (fightShown() && n3ds_engine_attack_menu_owner() != 0) queueKey('\t', SC_TAB, 0);   // close the open menu
    showFight = !fightShown();
    if (showFight) showSpell = false;   // one pop-up at a time
    break;
  case TAB_SPELL:
    if (fightShown()) { if (n3ds_engine_attack_menu_owner() != 0) queueKey('\t', SC_TAB, 0); showFight = false; showSpell = true; }
    else showSpell = !showSpell;
    break;
  case TAB_RECAST: recastHeld = true; recastSince = svcGetSystemTick(); break;   // acts on release
  case TAB_BAG:    if (handFull()) stowHeldItem(); else toggleLeaderInventory(); break;
  case TAB_SWAP:   viewOnBottom = !viewOnBottom; break;
  case TAB_HELP:   manualRequested = true; break;
  }
  if (i == TAB_SPELL || i == TAB_SWAP || i == TAB_FIGHT) saveSettings();
  presentPending = topDirty = true;
}

static int recastTouchX, recastTouchY, lastTapX, lastTapY;
static u64 lastTapUpTick = 0;

static void handleTouch(u32 held)
{
  if (held & KEY_TOUCH) {
    touchPosition t;
    hidTouchRead(&t);
    if (!touching) {
      touching = true;
      dbgTX = t.px; dbgTY = t.py; presentPending = topDirty = true;
      touchOnButton = false;
      if (layout == LAYOUT_DUNGEON) {   // 3DS-only tabs fire on press
        for (int i = 0; i < NUM_TABS; i++) {
          if (!inRect(tabRect(i), t.px, t.py)) continue;
          touchOnButton = true;
          pressTab(i);
          return;
        }
      }
      int x, y;
      if (!touchToST(t.px, t.py, &x, &y)) { touchOnButton = true; return; }   // gap: swallow this touch
      if (layout == LAYOUT_INVENTORY && !(held & KEY_ZR) && svcGetSystemTick() - lastTapUpTick < 350 * MS &&
          abs(x - lastTapX) < 8 && abs(y - lastTapY) < 8) {   // double-tap: eat or drink it
        touchOnButton = true;
        lastTapUpTick = 0;
        eatFromSlot(lastTapX, lastTapY);
        return;
      }
      touchRight = (held & KEY_ZR) != 0 || portraitWantsRight;   // ZR + tap = right mouse button
      DBG("[n3ds] touch DOWN px=(%d,%d) st=(%d,%d) layout=%d right=%d\n",
          t.px, t.py, x, y, (int)layout, touchRight);
      n3ds_send_mouse_move(x, y);
      n3ds_send_mouse_button(touchRight, true, x, y);
      lastSTX = x; lastSTY = y;
    } else if (recastHeld) {   // RECAST held: after a moment, offer every champion's spell
      if (!recastChooser && svcGetSystemTick() - recastSince > 300 * MS) { recastChooser = true; presentPending = topDirty = true; }
      recastTouchX = t.px; recastTouchY = t.py;
    } else if (!touchOnButton) {
      int x, y;
      if (touchToST(t.px, t.py, &x, &y) && (x != lastSTX || y != lastSTY)) {
        n3ds_send_mouse_move(x, y);               // drag
        lastSTX = x; lastSTY = y;
      }
    }
  } else if (touching) {
    touching = false;
    if (pressedTab >= 0) { pressedTab = -1; presentPending = true; }
    if (recastHeld) {   // a quick tap recasts the last spell; after a hold, the champion under the finger
      if (!recastChooser) recastLastSpell();
      else for (int c = 0; c < 4; c++) if (inRect(chooserButton(c), recastTouchX, recastTouchY)) { playClick(); recastSpell(c); }
      recastHeld = recastChooser = false;
      presentPending = topDirty = true;
      return;
    }
    if (!touchOnButton) {
      lastTapUpTick = svcGetSystemTick(); lastTapX = lastSTX; lastTapY = lastSTY;
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

static void showManual(void);

// Left-handed play: the D-pad and A/B/X/Y swap jobs (Y<->up, A<->right, B<->down, X<->left).
// Toggled from the manual's bottom screen; remembered in csb3ds.ini.
static bool leftyOn = false;
static bool leftyNow() { return leftyOn; }
static u32 remapKeys(u32 m)
{
  static const u32 pr[4][2] = { { KEY_Y, KEY_DUP }, { KEY_A, KEY_DRIGHT }, { KEY_B, KEY_DDOWN }, { KEY_X, KEY_DLEFT } };
  if (!leftyOn) return m;
  u32 r = m;
  for (int i = 0; i < 4; i++) r &= ~(pr[i][0] | pr[i][1]);
  for (int i = 0; i < 4; i++) {
    if (m & pr[i][0]) r |= pr[i][1];
    if (m & pr[i][1]) r |= pr[i][0];
  }
  return r;
}
static void saveSettings(void);

// Buttons still held when a modal (manual, message box) closes must not leak into
// the game: B that closed the manual would otherwise attack with champion 2.
static bool armSuppress = false;
static u32 suppressMask = 0;
static const u32 GAME_KEYS = KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_ZL | KEY_ZR |
                             KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN |
                             KEY_CPAD_LEFT | KEY_CPAD_RIGHT | KEY_START | KEY_SELECT;

bool n3ds_poll(void)
{
  if (!aptMainLoop()) return false;
  hidScanInput();
  u32 down = remapKeys(hidKeysDown()), held = remapKeys(hidKeysHeld());
  if (armSuppress) { suppressMask = held & GAME_KEYS; armSuppress = false; }
  suppressMask &= held;               // released buttons work again
  down &= ~suppressMask; held &= ~suppressMask;
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
      presentPending = topDirty = true;
    }
    if (layout == LAYOUT_FULL) updateFullZoom((held & KEY_ZR) != 0);
  }

  if ((held & KEY_START) && (held & KEY_SELECT) && (down & (KEY_START | KEY_SELECT))) {
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
  if ((down & KEY_SELECT) && !(held & KEY_START) && layout == LAYOUT_FULL && !touching)
    manualRequested = true;                                                       // SELECT on menus: manual
  // ZL swaps the big view between the screens (the SWAP tab), in the dungeon.
  if ((down & KEY_ZL) && layout == LAYOUT_DUNGEON) {
    viewOnBottom = !viewOnBottom;
    saveSettings();
    presentPending = topDirty = true;
  }
  // Champion attacks: A/B/X/Y = champion 1/2/3/4, one press per attack.
  // Plain = first attack, with ZR = second (the third is on the FIGHT panel).
  // Holding the button keeps attacking: the attack is re-issued every 0.6 s, and
  // the engine itself ignores it until the champion's weapon has recovered.
  static u64 nextAutoAttack[4];
  if (layout == LAYOUT_DUNGEON) {
    u64 t = svcGetSystemTick();
    for (int c = 0; c < 4; c++) {
      int row = (held & KEY_ZR) ? 1 : 0;
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
  if (manualRequested) { manualRequested = false; showManual(); armSuppress = true; }
  runActions();

  if (layout == LAYOUT_DUNGEON || layout == LAYOUT_INVENTORY) updateAutomap();
  {   // the FIGHT pop-up closes by itself once an attack has been chosen
    static int prevOwner = 0;
    int owner = layout == LAYOUT_DUNGEON ? n3ds_engine_attack_menu_owner() : 0;
    if (prevOwner != 0 && owner == 0) { showFight = false; presentPending = topDirty = true; }
    if (owner != prevOwner) presentPending = topDirty = true;
    prevOwner = owner;
  }
  {   // hold ZR on its own for a moment: the whole top screen shows the message log
      // (the delay keeps it from flashing up during ZR + A/B/X/Y attacks)
    static u64 zlSince = 0;
    bool zlAlone = (held & KEY_ZR) && !(held & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_TOUCH));
    if (!zlAlone) zlSince = 0;
    else if (!zlSince) zlSince = svcGetSystemTick();
    bool want = zlAlone && svcGetSystemTick() - zlSince > 300 * MS &&
                (layout == LAYOUT_INVENTORY || (layout == LAYOUT_DUNGEON && viewOnBottom));
    if (want != logView) { logView = want; presentPending = topDirty = true; }
  }
  if (layout == LAYOUT_DUNGEON || layout == LAYOUT_INVENTORY) {   // vitals and the RECAST button change without engine redraws
    static int lastShownCaster = -2;
    unsigned h = vitalsHash();
    if (h != lastVitalsHash || lastShownCaster != lastCaster) {
      lastVitalsHash = h; lastShownCaster = lastCaster; presentPending = topDirty = true;
    }
  }
  if (sliderStep(osGet3DSliderState()) != sliderStep(lastSlider)) presentPending = topDirty = true;
  if (presentPending) flushPresent(false);
  return true;
}

// ---------------------------------------------------------------------------
// Instruction manual, readable in the game (HELP tab, or SELECT on menu screens).
// The top screen is for reading: the section's text, scrolled with the Circle Pad
// or D-pad. The bottom screen is for choosing: a big button per section, and Close.
// L / R step through sections, B or START closes; it reopens where it was left.
// The game waits while it is open (the engine only advances when we return).
// Text: built in from manual.txt (n3ds_manual.h, generated by build.sh); a
// manual.txt in sdmc:/3ds/csb3ds/ replaces it. Format: "# " section, "## " heading,
// "- " list item, "1. " numbered step, "| a | b" two-column table row.
// ---------------------------------------------------------------------------
#include "n3ds_manual.h"

enum { MAN_SECTIONS = 24, MAN_LINES = 400, MAN_COLS = 48, MAN_COL1 = 15 };
enum { ML_BODY, ML_HEAD, ML_ITEM, ML_TABLE, ML_BLANK };
struct ManLine { unsigned char kind, indent; char a[MAN_COLS + 1]; char b[MAN_COLS + 1]; };
static char *manBuf = NULL;
static int manSecCount = 0;
static const char *manSecTitle[MAN_SECTIONS];
static int manSecTitleLen[MAN_SECTIONS];
static const char *manSecBody[MAN_SECTIONS];   // text after the "# " line
static const char *manSecEnd[MAN_SECTIONS];
static ManLine manLines[MAN_LINES];
static int manLineCount = 0, manSection = 0, manScroll = 0;

static void manualLoad(void)
{
  if (manBuf) return;
  FILE *f = fopen(DATA_DIR "/manual.txt", "rb");
  long size = 0;
  if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size < 512 * 1024) {
    rewind(f);
    manBuf = (char *)malloc(size + 1);
    if (manBuf && fread(manBuf, 1, size, f) == (size_t)size) manBuf[size] = 0;
    else { free(manBuf); manBuf = NULL; }
  }
  if (f) fclose(f);
  if (!manBuf) manBuf = strdup(N3DS_MANUAL);
  if (!manBuf) return;
  for (char *p = manBuf; *p; ) {   // sections begin at lines starting with "# "
    char *eol = strchr(p, '\n');
    char *next = eol ? eol + 1 : p + strlen(p);
    if (p[0] == '#' && p[1] == ' ' && manSecCount < MAN_SECTIONS) {
      if (manSecCount) manSecEnd[manSecCount - 1] = p;
      manSecTitle[manSecCount] = p + 2;
      int len = (int)((eol ? eol : next) - (p + 2));
      while (len > 0 && (p[2 + len - 1] == '\r' || p[2 + len - 1] == ' ')) len--;
      manSecTitleLen[manSecCount] = len;
      manSecBody[manSecCount] = next;
      manSecCount++;
    }
    p = next;
  }
  if (manSecCount) manSecEnd[manSecCount - 1] = manBuf + strlen(manBuf);
}

static ManLine *manAdd(int kind, int indent)
{
  if (manLineCount >= MAN_LINES) return NULL;
  ManLine *l = &manLines[manLineCount++];
  l->kind = (unsigned char)kind; l->indent = (unsigned char)indent; l->a[0] = l->b[0] = 0;
  return l;
}

// Word-wrap text[0..len) to `width` columns; returns the number of characters that fit
// on this line (breaking at a space when possible) and skips the space.
static int manWrap(const char *s, int len, int width, int *consumed)
{
  if (len <= width) { *consumed = len; return len; }
  int cut = width;
  while (cut > 0 && s[cut] != ' ') cut--;
  if (cut == 0) { *consumed = width; return width; }
  *consumed = cut + 1;
  return cut;
}

static void manLayout(int sec)
{
  manLineCount = 0;
  if (sec < 0 || sec >= manSecCount) return;
  const char *p = manSecBody[sec], *end = manSecEnd[sec];
  while (p < end) {
    const char *eol = (const char *)memchr(p, '\n', end - p);
    if (!eol) eol = end;
    int len = (int)(eol - p);
    while (len > 0 && p[len - 1] == '\r') len--;
    const char *s = p;
    p = eol + 1;
    if (len == 0) { if (manLineCount && manLines[manLineCount - 1].kind != ML_BLANK) manAdd(ML_BLANK, 0); continue; }
    if (s[0] == '#' && s[1] == '#' && s[2] == ' ') {
      if (manLineCount && manLines[manLineCount - 1].kind != ML_BLANK) manAdd(ML_BLANK, 0);
      ManLine *l = manAdd(ML_HEAD, 0);
      if (l) { int n = len - 3 > MAN_COLS ? MAN_COLS : len - 3; memcpy(l->a, s + 3, n); l->a[n] = 0; }
      continue;
    }
    if (s[0] == '|') {   // "| a | b": first column, then the rest wrapped beside it
      const char *a = s + 1, *bar = (const char *)memchr(a, '|', len - 1);
      const char *b = bar ? bar + 1 : s + len;
      int alen = (int)((bar ? bar : s + len) - a), blen = (int)(s + len - b);
      while (alen > 0 && a[0] == ' ') { a++; alen--; }
      while (alen > 0 && a[alen - 1] == ' ') alen--;
      while (blen > 0 && b[0] == ' ') { b++; blen--; }
      if (alen > MAN_COL1 - 1) alen = MAN_COL1 - 1;
      bool first = true;
      do {
        ManLine *l = manAdd(ML_TABLE, 0);
        if (!l) break;
        if (first) { memcpy(l->a, a, alen); l->a[alen] = 0; first = false; }
        int used, n = manWrap(b, blen, MAN_COLS - MAN_COL1, &used);
        memcpy(l->b, b, n); l->b[n] = 0;
        b += used; blen -= used;
      } while (blen > 0);
      continue;
    }
    int kind = ML_BODY, indent = 0;
    char marker[6] = "";
    if (s[0] == '-' && s[1] == ' ') { kind = ML_ITEM; indent = 2; strcpy(marker, "-"); s += 2; len -= 2; }
    else if (s[0] >= '1' && s[0] <= '9' && s[1] == '.' && s[2] == ' ') {
      kind = ML_ITEM; indent = 3; marker[0] = s[0]; marker[1] = '.'; marker[2] = 0; s += 3; len -= 3;
    }
    bool first = true;
    while (len > 0) {
      ManLine *l = manAdd(kind, indent);
      if (!l) break;
      if (first && marker[0]) strcpy(l->b, marker);   // the bullet / number, drawn in the margin
      first = false;
      int used, n = manWrap(s, len, MAN_COLS - indent, &used);
      memcpy(l->a, s, n); l->a[n] = 0;
      s += used; len -= used;
    }
  }
  while (manLineCount && manLines[manLineCount - 1].kind == ML_BLANK) manLineCount--;
}

enum { MAN_HEAD_H = 20, MAN_LINE_H = 10, MAN_TEXT_Y = 26, MAN_VISIBLE = 20, MAN_X = 6 };

static void manDrawTop(u16 *fb)
{
  const u16 bg = rgb565(16, 14, 12), band = rgb565(70, 52, 30), gold = rgb565(245, 200, 90);
  const u16 text = rgb565(225, 220, 205), dim = rgb565(150, 140, 120), head = rgb565(130, 200, 255);
  fillRect(fb, TOP_W, 0, 0, TOP_W, TOP_H, bg);
  fillRect(fb, TOP_W, 0, 0, TOP_W, MAN_HEAD_H, band);
  char title[64];
  int tl = manSecTitleLen[manSection] < 40 ? manSecTitleLen[manSection] : 40;
  memcpy(title, manSecTitle[manSection], tl); title[tl] = 0;
  for (char *c = title; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
  drawText(fb, TOP_W, MAN_X, 6, title, gold);
  char pos[16];
  snprintf(pos, sizeof pos, "%d/%d", manSection + 1, manSecCount);
  drawText(fb, TOP_W, TOP_W - 6 - (int)strlen(pos) * 8, 6, pos, rgb565(230, 210, 170));
  for (int i = 0; i < MAN_VISIBLE; i++) {
    int li = manScroll + i;
    if (li >= manLineCount) break;
    const ManLine &l = manLines[li];
    int y = MAN_TEXT_Y + i * MAN_LINE_H;
    switch (l.kind) {
    case ML_HEAD:  drawText(fb, TOP_W, MAN_X, y, l.a, head); break;
    case ML_TABLE: drawText(fb, TOP_W, MAN_X, y, l.a, gold); drawText(fb, TOP_W, MAN_X + MAN_COL1 * 8, y, l.b, text); break;
    case ML_ITEM:  if (l.b[0]) drawText(fb, TOP_W, MAN_X, y, l.b, gold);
                   drawText(fb, TOP_W, MAN_X + l.indent * 8, y, l.a, text); break;
    case ML_BODY:  drawText(fb, TOP_W, MAN_X, y, l.a, text); break;
    default: break;
    }
  }
  if (manLineCount > MAN_VISIBLE) {   // scroll bar
    int trackY = MAN_TEXT_Y, trackH = MAN_VISIBLE * MAN_LINE_H;
    int thumbH = trackH * MAN_VISIBLE / manLineCount; if (thumbH < 8) thumbH = 8;
    int thumbY = trackY + (trackH - thumbH) * manScroll / (manLineCount - MAN_VISIBLE);
    fillRect(fb, TOP_W, TOP_W - 4, trackY, 2, trackH, rgb565(50, 46, 40));
    fillRect(fb, TOP_W, TOP_W - 5, thumbY, 4, thumbH, gold);
  }
  drawText(fb, TOP_W, MAN_X, TOP_H - 11, leftyOn ? "STICKS SCROLL ABXY PICK L/R D-DOWN CLOSE" : "STICKS SCROLL  D-PAD PICK  L/R  B CLOSE", dim);
}

static Rect manButton(int i, int count)   // section buttons in two columns, Close last
{
  int rows = (count + 1) / 2;
  int rowH = (BOT_H - 4) / rows;
  Rect r = { 4 + (i % 2) * 158, 4 + (i / 2) * rowH, 154, rowH - 4 };
  return r;
}

static int manFocus = 0;      // D-pad focus: a section button, or Close (= manSecCount)
static int manPressed = -1;   // button under the finger, drawn pressed

static void manDrawBottom(u16 *fb)
{
  const u16 face = rgb565(52, 44, 36), on = rgb565(120, 90, 40), edge = rgb565(150, 130, 100);
  const u16 text = rgb565(235, 225, 195), focusC = rgb565(250, 215, 110);
  fillRect(fb, BOT_W, 0, 0, BOT_W, BOT_H, rgb565(10, 10, 12));
  int count = manSecCount + 2;   // sections, CLOSE, LEFT HAND
  for (int i = 0; i < count; i++) {
    Rect r = manButton(i, count);
    bool close = i == manSecCount, down = i == manPressed;
    u16 fill = close ? rgb565(120, 30, 24) : (i == manSecCount + 1 ? (leftyOn ? on : face) : (i == manSection ? on : face));
    if (down) fill = close ? rgb565(70, 16, 12) : rgb565(30, 25, 20);
    fillRect(fb, BOT_W, r.x, r.y, r.w, r.h, fill);
    frameRect(fb, BOT_W, r.x, r.y, r.w, r.h, close ? rgb565(220, 90, 70) : edge);
    if (i == manFocus) {   // D-pad focus ring
      frameRect(fb, BOT_W, r.x - 2, r.y - 2, r.w + 4, r.h + 4, focusC);
      frameRect(fb, BOT_W, r.x - 1, r.y - 1, r.w + 2, r.h + 2, focusC);
    }
    if (down) r.y++;
    char label[24];
    if (close) strcpy(label, "CLOSE");
    else if (i == manSecCount + 1) strcpy(label, leftyOn ? "LEFT HAND: ON" : "LEFT HAND: OFF");
    else {
      int n = manSecTitleLen[i] < 18 ? manSecTitleLen[i] : 18;
      memcpy(label, manSecTitle[i], n); label[n] = 0;
      for (char *c = label; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    }
    drawText(fb, BOT_W, r.x + (r.w - (int)strlen(label) * 8) / 2, r.y + (r.h - 8) / 2, label, text);
  }
}

static void manPresent(void)
{
  manDrawTop((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL));
  manDrawTop((u16 *)gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL));
  manDrawBottom((u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}

// Controls (the same in every one of these 3DS ports):
//   Circle Pad / C-Stick: scroll the text (hold to keep scrolling)
//   D-pad: move between the section buttons (a section opens as it is picked;
//          on CLOSE, A closes)       L / R: previous / next section
//   tap a section button, or CLOSE   B or START: close
static void showManual(void)
{
  manualLoad();
  if (manSecCount == 0) return;
  if (manSection >= manSecCount) manSection = 0;
  manFocus = manSection;
  manLayout(manSection);
  if (manScroll > manLineCount - MAN_VISIBLE) manScroll = manLineCount > MAN_VISIBLE ? manLineCount - MAN_VISIBLE : 0;
  DBG("[n3ds] manual open: section %d\n", manSection);
  bool waitRelease = true;   // the tap that opened the manual is still down
  int repeat = 0;
  const int count = manSecCount + 2;   // sections, CLOSE, LEFT HAND
  const u32 SCROLL_DOWN = KEY_CPAD_DOWN | KEY_CSTICK_DOWN, SCROLL_UP = KEY_CPAD_UP | KEY_CSTICK_UP;
  while (aptMainLoop()) {
    hidScanInput();
    u32 kd = remapKeys(hidKeysDown()), kh = remapKeys(hidKeysHeld());
    if (kd & (KEY_B | KEY_START)) { playClick(); break; }
    if ((kd & KEY_A) && manFocus == manSecCount) { playClick(); break; }
    if ((kd & KEY_A) && manFocus == manSecCount + 1) { playClick(); leftyOn = !leftyOn; saveSettings(); }
    int sec = manSection, focus = manFocus;
    if (kd & KEY_R) focus = sec = (manSection + 1) % manSecCount;
    if (kd & KEY_L) focus = sec = (manSection + manSecCount - 1) % manSecCount;
    if (kd & KEY_DRIGHT && (focus % 2) == 0 && focus + 1 < count) focus++;
    if (kd & KEY_DLEFT && (focus % 2) == 1) focus--;
    if (kd & KEY_DDOWN && focus + 2 < count) focus += 2;
    else if (kd & KEY_DDOWN && focus + 1 < count && (focus % 2) == 0) focus++;   // odd count: last row
    if (kd & KEY_DUP && focus - 2 >= 0) focus -= 2;
    if (focus != manFocus) { manFocus = focus; playClick(); if (focus < manSecCount) sec = focus; }
    if (kh & KEY_TOUCH) {
      touchPosition t;
      hidTouchRead(&t);
      int hit = -1;
      for (int i = 0; i < count; i++) if (inRect(manButton(i, count), t.px, t.py)) hit = i;
      if (!waitRelease && hit >= 0) { playClick(); manFocus = hit; waitRelease = true; }
      manPressed = waitRelease ? (hit == manFocus ? hit : -1) : -1;
    } else {
      if (manPressed == manSecCount) { manPressed = -1; break; }   // CLOSE acts on release
      if (manPressed == manSecCount + 1) { leftyOn = !leftyOn; saveSettings(); }   // LEFT HAND flips, stays open
      else if (manPressed >= 0) sec = manPressed;
      manPressed = -1;
      waitRelease = false;
    }
    if (manPressed >= 0 && manPressed < manSecCount) sec = manPressed;
    if (sec != manSection) { manSection = sec; manScroll = 0; manLayout(sec); if (manFocus != manSecCount) manFocus = sec; }
    int dir = (kh & SCROLL_DOWN) ? 1 : (kh & SCROLL_UP) ? -1 : 0;
    if (dir) {
      if ((kd & (SCROLL_DOWN | SCROLL_UP)) || ++repeat >= 2) {   // first push, then every other frame
        repeat = 0;
        manScroll += dir;
        int maxScroll = manLineCount > MAN_VISIBLE ? manLineCount - MAN_VISIBLE : 0;
        if (manScroll < 0) manScroll = 0;
        if (manScroll > maxScroll) manScroll = maxScroll;
      }
    } else repeat = 0;
    manPresent();
  }
  manPressed = -1;
  while (aptMainLoop()) {   // let go of the screen before the game sees touches again
    hidScanInput();
    if (!(hidKeysHeld() & KEY_TOUCH)) break;
    gspWaitForVBlank();
  }
  DBG("[n3ds] manual closed\n");
  presentPending = topDirty = true;
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
  if (leftyOn) printf(yesno ? "D-Pad right = Yes    D-Pad down = No\n" : "Press D-Pad right to continue\n");
  else printf(yesno ? "A = Yes    B = No\n" : "Press A to continue\n");
  bool result = true;
  while (aptMainLoop()) {
    hidScanInput();
    u32 kd = remapKeys(hidKeysDown());
    if (kd & KEY_A) { result = true; break; }
    if (yesno && (kd & KEY_B)) { result = false; break; }
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);   // the top screen keeps the last game frame
    gspWaitForVBlank();
  }
  // Give the bottom screen back to the game.
  consoleClear();
  setupScreens();
  n3ds_force_redraw();
  presentPending = topDirty = true;
  armSuppress = true;
  return result;
}

// ---------------------------------------------------------------------------
// File access
// ---------------------------------------------------------------------------
// The engine reads its data a few bytes at a time and calls fseek(0, SEEK_CUR)
// before every read, which discards stdio's buffer; on the SD card each of those
// became a separate slow access and the first dungeon load took about a minute.
// Read-only files are read in one go and served from memory instead.
// The auto-map follows your saves (hooked in SaveGame.cpp): saving a game writes
// <save>.map beside it, loading a save reads it back, and a new game starts blank.
static const char MAP_MAGIC[8] = { 'C', 'S', 'B', '3', 'M', 'A', 'P', '1' };

void n3ds_automap_saved(const char *saveName, const char *backupName)
{
  if (!saveName) return;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s.map", saveName);
  if (backupName) {   // the engine just renamed the old save to its .BAK; keep the pair together
    char bak[PATH_MAX];
    snprintf(bak, sizeof bak, "%s.map", backupName);
    remove(bak);
    rename(path, bak);
  }
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(MAP_MAGIC, 1, sizeof MAP_MAGIC, f);
  fwrite(seen, 1, sizeof seen, f);
  fclose(f);
  DBG("[n3ds] auto-map saved to %s\n", path);
}

void n3ds_automap_loaded(const char *saveName)   // NULL = new game
{
  memset(seen, 0, sizeof seen);
  lastMapPos[0] = -1;
  if (!saveName) return;
  char path[PATH_MAX], magic[8];
  snprintf(path, sizeof path, "%s.map", saveName);
  FILE *f = fopen(path, "rb");
  if (!f) return;   // a save from before maps were kept: start blank
  if (fread(magic, 1, sizeof magic, f) != sizeof magic || memcmp(magic, MAP_MAGIC, sizeof magic) != 0 ||
      fread(seen, 1, sizeof seen, f) != sizeof seen)
    memset(seen, 0, sizeof seen);
  fclose(f);
  DBG("[n3ds] auto-map loaded from %s\n", path);
}

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

// Remembered between sessions: which screen has the view, and open pop-ups.
static void saveSettings(void)
{
  FILE *f = fopen(DATA_DIR "/csb3ds.ini", "w");
  if (!f) return;
  fprintf(f, "view_on_bottom=%d\nshow_spell=%d\nlefty=%d\n", viewOnBottom ? 1 : 0, showSpell ? 1 : 0, leftyOn ? 1 : 0);
  fclose(f);
}

static void loadSettings(void)
{
  FILE *f = fopen(DATA_DIR "/csb3ds.ini", "r");
  if (!f) return;
  char line[64];
  int v;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "view_on_bottom=%d", &v) == 1) viewOnBottom = v != 0;
    if (sscanf(line, "show_spell=%d", &v) == 1) showSpell = v != 0;
    if (sscanf(line, "lefty=%d", &v) == 1) leftyOn = v != 0;
  }
  fclose(f);
}

void n3ds_init(void)
{
  osSetSpeedupEnable(true);   // 804 MHz + L2 cache on New 3DS
  gfxInitDefault();
  setupScreens();
  gspSetEventCallback(GSPGPU_EVENT_VBlank0, onVBlank, NULL, false);
  buildDepthMap();
  memset(n3ds_st_buffer, 0, sizeof(n3ds_st_buffer));

  mkdir(DATA_DIR, 0777);
  (void)chdir(DATA_DIR);
  loadSettings();
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
