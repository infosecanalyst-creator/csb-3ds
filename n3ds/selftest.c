// selftest.c - run a port's test script on a real 3DS, without anyone holding it.
//
// Linked into a port with the --wrap flags in selftest.mk. When the port is
// started with the argument "--selftest=<script>" (the test agent does this),
// the script presses buttons, taps, drags and moves the sticks by replacing what
// the HID functions return, and saves both screens as BMP files. At the end it
// tells the Homebrew loader to start the test agent again and exits, so the
// next test can run. Without the argument nothing changes: no thread, no file,
// no SD access, every wrapped call goes straight to libctru.
//
// THIS FILE IS SHARED: every port carries an identical copy (master in
// 3ds-test-suite hwtest/selftest). Port-specific behaviour goes in an optional
// "selftest_port.h" next to this file (see "Port glue" below), never in here.
//
// Script (one command per line, '#' starts a comment). Full list with examples in
// README.md next to this file.
//   wait N                    N frames (one frame = one call of hidScanInput)
//   waitms N                  N milliseconds of wall-clock time
//   press BTN [frames]        press and release (default 6 frames)
//   chord HELD BTN [frames]   two buttons together (default 6 frames)
//   hold BTN | release BTN    keep a button down / let it go
//   tap X Y [frames]          touch the bottom screen (default 6 frames)
//   drag X1 Y1 X2 Y2 [frames] touch at X1,Y1 and move in a straight line to X2,Y2 (default 30 frames)
//   stick L|C X Y [frames]    Circle Pad (L) or C-Stick (C) (default 30 frames; 0 = keep it there)
//   minpress MS               every press/chord/tap/drag/stick lasts at least MS ms (default 150)
//   minrelease MS             after one of those, wait at least MS ms before the next command (default 50)
//   shot NAME                 save both screens (NAME_top.bmp, NAME_bottom.bmp)
//   waitpix top|bottom X Y RRGGBB N   until that pixel has that colour (at most N frames and N/60 s)
//   hang N                    the game may draw nothing for N seconds before the watchdog acts (min 60)
//   end
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---- Port glue ---------------------------------------------------------------
// A port that needs something different puts a selftest_port.h next to this file.
// It is included twice: first with ST_PORT_CONFIG defined (settings, before any code
// here), then with ST_PORT_CODE defined (extra code at the end, e.g. more --wrap
// functions or #include "restartguard.c"). Settings it may define:
//   ST_INJECT 0             run the script and take shots, but leave the real input alone
//   ST_ONE_THREAD 1         writer and watchdog share one thread (FS2: near the 3DS thread limit)
//   ST_WRITER_CORE n        core for the shot writer (default 2, the New 3DS's extra core; -2 = game's)
//   ST_WRITER_PRIO_OFFSET n writer priority = game's + n (default 2; one-thread mode default -1)
//   ST_DONE_GRACE_MS n      after the script, let the game quit by itself for n ms before ending it
//   ST_APT_QUIT 1           after the script, aptMainLoop() returns false (the game quits normally)
//   ST_PORT_QUIT()          called when the script is done (ask the game to quit its own way)
//   ST_TRACE(...)           printf-style trace into the port's own log
//   ST_EMU_SCRIPT "path"    script to use when there is no --selftest argument but this file exists
#if defined(__has_include)
#if __has_include("selftest_port.h")
#define ST_PORT_CONFIG 1
#include "selftest_port.h"
#undef ST_PORT_CONFIG
#define ST_HAVE_PORT_GLUE 1
#endif
#endif
#ifndef ST_INJECT
#define ST_INJECT 1
#endif
#ifndef ST_ONE_THREAD
#define ST_ONE_THREAD 0
#endif
#ifndef ST_WRITER_CORE
#define ST_WRITER_CORE (ST_ONE_THREAD ? -2 : 2)
#endif
#ifndef ST_WRITER_PRIO_OFFSET
#define ST_WRITER_PRIO_OFFSET (ST_ONE_THREAD ? -1 : 2)
#endif
#ifndef ST_DONE_GRACE_MS
#define ST_DONE_GRACE_MS 0
#endif
#ifndef ST_APT_QUIT
#define ST_APT_QUIT 0
#endif
#ifndef ST_PORT_QUIT
#define ST_PORT_QUIT() ((void)0)
#endif
#ifndef ST_TRACE
#define ST_TRACE(...) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {   // the same file builds as C or C++ (ScummVM's build only compiles C++)
#endif

extern int __system_argc;
extern char** __system_argv;

void __real_hidScanInput(void);
u32 __real_hidKeysHeld(void);
u32 __real_hidKeysDown(void);
u32 __real_hidKeysDownRepeat(void);
u32 __real_hidKeysUp(void);
void __real_hidTouchRead(touchPosition* pos);
void __real_hidCircleRead(circlePosition* pos);
void __real_irrstCstickRead(circlePosition* pos);
bool __real_aptMainLoop(void);

#define ST_LIMIT_MS (30u * 60u * 1000u)   // a whole test may take this long; then the script stops cleanly
#define ST_BOOT_MS 180000u                 // a starting game may draw nothing this long (FS2 loads ~40 s+)
#define ST_BOOT_FRAMES 30                  // ...until it has run this many input passes

static volatile int st_done;      // script finished: the game is ended (or asked to quit)
static int st_on = -1;            // -1 not checked yet, 0 off, 1 running
static char st_dir[256];          // results folder
static char* st_lines[4096];
static int st_n, st_pc;
static FILE* st_log;
static volatile u64 st_frame;
static u64 st_now;                // osGetTime() of this frame
// the current step: lasts st_wait more frames AND until st_until (ms, 0 = no time limit)
static long st_wait;
static u64 st_until;
static int st_pressing;           // the step is an input (press, tap, drag, stick): undo it when it ends
static u64 st_gapUntil;           // after an input step, no next command before this time
static u32 st_minpress = 150, st_minrelease = 50;
static u32 st_held, st_prev, st_hold;
static int st_touch, st_tx, st_ty;
static int st_drag, st_dx1, st_dy1, st_dx2, st_dy2;
static long st_dragFrames;
static u64 st_dragStart;
static u32 st_dragMs;
static int st_stick, st_sx, st_sy;   // st_stick: 0 none, 1 Circle Pad, 2 C-Stick
static int st_hx[3], st_hy[3];       // sticks kept in place ("stick L|C X Y 0"), [1] Circle Pad, [2] C-Stick
static u32 st_hbits[3];
static volatile int st_limit;        // the watchdog saw the 30-minute limit: stop the script

static void st_logf(const char* fmt, ...) {
    if (!st_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(st_log, fmt, ap);
    va_end(ap);
    fflush(st_log);
}

static u32 st_button(const char* b) {
    static const struct { const char* n; u32 k; } m[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
        {"ZL", KEY_ZL}, {"ZR", KEY_ZR}, {"START", KEY_START}, {"SELECT", KEY_SELECT},
        {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT},
        {"CUP", KEY_CSTICK_UP}, {"CDOWN", KEY_CSTICK_DOWN}, {"CLEFT", KEY_CSTICK_LEFT}, {"CRIGHT", KEY_CSTICK_RIGHT}};
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strcmp(m[i].n, b)) return m[i].k;
    st_logf("unknown button %s\n", b);
    return 0;
}

// ---- Screenshots ---------------------------------------------------------------
// The screens are copied to memory at once (fast, on the game's thread) and written
// to the SD card by a separate thread, so a slow SD card never holds the game up (a
// shot froze Gold Box on the real 3DS). One buffer, allocated at the first shot and
// reused for every later one (each shot used to allocate ~1.7 MB of the game's heap):
// [ write chunk | top screen pixels | bottom screen pixels ]. The BMP is converted
// from the pixel copy into the chunk and written a chunk at a time (few, large SD
// writes: 240 small row writes queued behind Gold Box's own SD reads took minutes).
#define ST_CHUNK (48 * 1024)
static u8* st_shotBuf;
static size_t st_shotCap;
typedef struct { char path[300]; const u8* px; int fmt, w, stride; } StShotJob;
static StShotJob st_jobs[2];          // the two screens of one shot
static volatile int st_jobHead, st_jobTail;
static LightEvent st_jobEvent;
// What the writer is doing, read by the game's thread for the log (the writer
// itself never logs, so the two threads never share the log file).
static const char* volatile st_writing;
static volatile u32 st_wroteN, st_wroteMs, st_wroteMaxMs;
static int st_writerCore = -2;

static void st_bmp(const char* path, const u8* fb, int fmt, int w, int h, int stride) {
    u8* out = st_shotBuf;               // the chunk at the start of the shot buffer
    int row = (w * 3 + 3) & ~3;
    u32 size = 54 + row * h;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    setvbuf(f, NULL, _IONBF, 0);       // straight to the card, one request per chunk
    memset(out, 0, 54);
    out[0] = 'B'; out[1] = 'M';
    memcpy(out + 2, &size, 4);
    u32 v = 54; memcpy(out + 10, &v, 4);
    v = 40; memcpy(out + 14, &v, 4);
    memcpy(out + 18, &w, 4);
    memcpy(out + 22, &h, 4);
    out[26] = 1; out[28] = 24;
    size_t used = 54;
    int bpp = fmt == GSP_RGBA8_OES ? 4 : fmt == GSP_BGR8_OES ? 3 : 2;
    for (int y = h - 1; y >= 0; y--) {           // BMP rows go bottom-up
        if (used + row > ST_CHUNK) { fwrite(out, 1, used, f); used = 0; }
        u8* line = out + used;
        memset(line, 0, row);
        for (int x = 0; x < w; x++) {
            // 3DS framebuffers are rotated: column x is stored as a run of 240 pixels, y from the bottom.
            const u8* p = fb + x * stride + (239 - y) * bpp;
            u8 r = 0, g = 0, b = 0;
            if (fmt == GSP_RGBA8_OES) { r = p[3]; g = p[2]; b = p[1]; }
            else if (fmt == GSP_BGR8_OES) { r = p[2]; g = p[1]; b = p[0]; }
            else {
                u16 c = p[0] | (p[1] << 8);
                if (fmt == GSP_RGB565_OES) { r = (c >> 11) << 3; g = ((c >> 5) & 63) << 2; b = (c & 31) << 3; }
                else if (fmt == GSP_RGB5_A1_OES) { r = (c >> 11) << 3; g = ((c >> 6) & 31) << 3; b = ((c >> 1) & 31) << 3; }
                else { r = (c >> 12) << 4; g = ((c >> 8) & 15) << 4; b = ((c >> 4) & 15) << 4; }
            }
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        used += row;
    }
    if (used) fwrite(out, 1, used, f);
    fclose(f);
}

// One pixel of what a screen shows now, as 0xRRGGBB (x, y in 400x240 / 320x240 layout pixels).
static long st_pixel(int top, int x, int y) {
    GSPGPU_CaptureInfo ci;
    if (R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&ci))) return -1;
    GSPGPU_CaptureInfoEntry* e = &ci.screencapture[top ? 0 : 1];
    int fmt = e->format & 7;
    if (top && (e->format & 0x60) == 0) x *= 2;   // 800-pixel mode
    int bpp = fmt == GSP_RGBA8_OES ? 4 : fmt == GSP_BGR8_OES ? 3 : 2;
    const u8* p = (const u8*)e->framebuf0_vaddr + x * e->framebuf_widthbytesize + (239 - y) * bpp;
    if (fmt == GSP_RGBA8_OES) return (p[3] << 16) | (p[2] << 8) | p[1];
    if (fmt == GSP_BGR8_OES) return (p[2] << 16) | (p[1] << 8) | p[0];
    u16 c = p[0] | (p[1] << 8);
    if (fmt == GSP_RGB565_OES) return ((c >> 11) << 19) | (((c >> 5) & 63) << 10) | ((c & 31) << 3);
    return ((c >> 11) << 19) | (((c >> 6) & 31) << 11) | (((c >> 1) & 31) << 3);
}

static int st_waitTop, st_waitX, st_waitY;
static long st_waitCol, st_waitMax, st_waitDone;
static u64 st_waitPixUntil;

// ---- Is the game still drawing? -------------------------------------------------
// The watchdog normally goes by input passes (hidScanInput calls). A game can still be
// presenting frames without reading the buttons (a cutscene, a loading animation), and
// such a game must never be cold-restarted. When the passes stop, the writer thread looks
// at what the screens show once in a while; any change counts as a frame. It runs on the
// writer thread, not the watchdog: if that system call ever blocked, the watchdog would
// still act. One-thread mode (FS2) goes by input passes only.
static volatile u64 st_beat;     // input passes, as seen by the watchdog
static volatile u32 st_present;  // screen changes seen while the passes stood still
static void st_check_present(void) {
    static u64 lastBeat, still, lastSig;
    u64 now = osGetTime();
    if (st_beat != lastBeat) { lastBeat = st_beat; still = now; return; }
    if (now - still < 1000) return;
    GSPGPU_CaptureInfo ci;
    if (R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&ci))) return;
    u64 sig = 1469598103934665603ULL;
    for (int s = 0; s < 2; s++) {
        GSPGPU_CaptureInfoEntry* e = &ci.screencapture[s];
        const u32* fb = (const u32*)e->framebuf0_vaddr;
        sig = (sig ^ (u32)(uintptr_t)fb) * 1099511628211ULL;
        if (!fb) continue;
        u32 words = (u32)(s == 0 ? 400 : 320) * e->framebuf_widthbytesize / 4;
        for (u32 i = 0; i < 64; i++) sig = (sig ^ fb[(i * 2654435761u) % words]) * 1099511628211ULL;
    }
    if (lastSig && sig != lastSig) st_present = st_present + 1;
    lastSig = sig;
}

static void st_watchdog_tick(void);
static void st_writer(void* arg) {
    (void)arg;
    u64 lastTick = 0;
    for (;;) {
        LightEvent_WaitTimeout(&st_jobEvent, 250000000LL);
        while (st_jobTail != st_jobHead) {
            StShotJob* j = &st_jobs[st_jobTail % 2];
            st_writing = j->path;
            u64 t0 = osGetTime();
            st_bmp(j->path, j->px, j->fmt, j->w, 240, j->stride);
            u32 ms = (u32)(osGetTime() - t0);
            st_writing = NULL;
            st_wroteN = st_wroteN + 1;
            st_wroteMs = st_wroteMs + ms;
            if (ms > st_wroteMaxMs) st_wroteMaxMs = ms;
            st_jobTail = st_jobTail + 1;
        }
        if (ST_ONE_THREAD) {
            u64 now = osGetTime();
            if (now - lastTick >= 1000) { lastTick = now; st_watchdog_tick(); }
        } else if (!st_done) {
            st_check_present();
        }
    }
}

static void st_shot(const char* name) {
    if (st_jobTail != st_jobHead) {   // the previous shot is still being written: wait for it (10 s at most)
        u64 t0 = osGetTime();
        while (st_jobTail != st_jobHead && osGetTime() - t0 < 10000) svcSleepThread(10000000LL);
        if (st_jobTail != st_jobHead) { st_logf("shot %s: writer behind, skipped\n", name); return; }
        st_logf("shot %s: waited %lu ms for the previous shot\n", name, (unsigned long)(osGetTime() - t0));
    }
    GSPGPU_CaptureInfo ci;
    if (R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&ci))) { st_logf("shot %s: capture failed\n", name); return; }
    int w[2], stride[2];
    size_t n[2], need = ST_CHUNK;
    for (int s = 0; s < 2; s++) {
        GSPGPU_CaptureInfoEntry* e = &ci.screencapture[s];
        // the top screen in the 800-pixel mode: neither the 2D (bit 6) nor the 3D (bit 5) flag
        w[s] = s == 0 ? ((e->format & 0x60) == 0 ? 800 : 400) : 320;
        stride[s] = e->framebuf_widthbytesize;
        n[s] = (size_t)w[s] * stride[s];
        need += n[s];
    }
    if (need > st_shotCap) {
        free(st_shotBuf);
        st_shotBuf = (u8*)malloc(need);
        st_shotCap = st_shotBuf ? need : 0;
        if (!st_shotBuf) { st_logf("shot %s: no memory (%lu bytes)\n", name, (unsigned long)need); return; }
    }
    u8* px = st_shotBuf + ST_CHUNK;
    for (int s = 0; s < 2; s++) {
        GSPGPU_CaptureInfoEntry* e = &ci.screencapture[s];
        StShotJob* j = &st_jobs[(st_jobHead + s) % 2];
        j->fmt = e->format & 7;
        j->w = w[s];
        j->stride = stride[s];
        memcpy(px, e->framebuf0_vaddr, n[s]);
        j->px = px;
        px += n[s];
        snprintf(j->path, sizeof j->path, "%s/%s_%s.bmp", st_dir, name, s == 0 ? "top" : "bottom");
    }
    st_jobHead = st_jobHead + 2;
    LightEvent_Signal(&st_jobEvent);
    st_logf("shot %s (frame %llu)\n", name, (unsigned long long)st_frame);
}

// Point the Homebrew loader at the test agent: it starts when this process ends.
static void st_set_target(void) {
    Handle h;
    if (R_FAILED(svcConnectToPort(&h, "hb:ldr"))) return;
    static const char path[] = "/3ds/testagent/testagent.3dsx";
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(sizeof path, 0);
    cmd[2] = (u32)path;
    svcSendSyncRequest(h);
    static u32 argv[0x100];
    argv[0] = 1;
    strcpy((char*)&argv[1], "sdmc:/3ds/testagent/testagent.3dsx");
    cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(3, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(sizeof argv, 1);
    cmd[2] = (u32)argv;
    svcSendSyncRequest(h);
    svcCloseHandle(h);
}

// The script ended (or was stopped at the time limit). The watchdog then ends the game
// the normal way (after ST_DONE_GRACE_MS, for ports that quit by themselves).
static void st_finish(const char* why) {
    for (int i = 0; i < 200 && st_jobTail != st_jobHead; i++) svcSleepThread(100000000LL);   // let the last shots reach the SD card
    const char* w = st_writing;
    st_logf("writer: core %d, %lu files, %lu ms in all, slowest %lu ms\n", st_writerCore,
            (unsigned long)st_wroteN, (unsigned long)st_wroteMs, (unsigned long)st_wroteMaxMs);
    if (st_jobTail != st_jobHead)
        st_logf("writer: %d files never written, stuck on %s\n", st_jobHead - st_jobTail, w ? w : "(waiting for the CPU)");
    st_logf("%s\ndone frames=%llu\n", why, (unsigned long long)st_frame);
    st_set_target();
    st_held = st_prev = st_hold = 0;
    st_touch = st_stick = st_drag = 0;
    memset(st_hx, 0, sizeof st_hx); memset(st_hy, 0, sizeof st_hy);
    st_done = 1;
}

// ---- Watchdog -------------------------------------------------------------------
// Nobody has to look after the 3DS. Once a second:
// - script done: end the game (after ST_DONE_GRACE_MS) and return to the agent;
// - nothing drawn and no input pass for the hang limit (3 min while starting): the game
//   hung, so cold-restart the console (ending a process stuck on the GPU left the GPU hung);
// - 30 minutes in all: stop the script cleanly ("limit reached"); the game is then ended
//   like after a normal end of script, never by a cold restart while it still draws.
static volatile u32 st_hang_ms = 60000;   // watchdog limit for a frame gap (script: hang SECS)

static void st_end_game(void) {   // the normal end: back to the Homebrew loader, which starts the agent
    if (st_log) { fclose(st_log); st_log = NULL; }
    st_set_target();
    aptSetChainloaderToSelf();
    aptExit();
    svcExitProcess();
}

static void st_cold_restart(const char* why) {   // the game is frozen: restart the console
    st_logf("%s\ndone frames=0 (stopped by the watchdog)\n", why);
    if (st_log) { fclose(st_log); st_log = NULL; }
    st_set_target();
    // A hung game is usually stuck waiting on the GPU. Ending the process left the GPU hung
    // (every later launch froze until a power-off), so restart the whole console instead.
    // Cold restart through the MCU first (Nov 16): Luma treats it as a power-on and
    // autoboots the test agent, so nobody has to touch the 3DS. APT restart as the fallback
    // (lands on the HOME Menu). ptm:sysm RebootAsync crashed the ptm process (kernel panic,
    // Nov 15), so it is not used.
    svcSleepThread(500000000LL);
    if (R_SUCCEEDED(mcuHwcInit())) {
        u8 v = 1 << 2;
        MCUHWC_WriteRegister(0x20, &v, 1);
        mcuHwcExit();
        svcSleepThread(3000000000LL);
    }
    APT_HardwareResetAsync();
    svcSleepThread(2000000000LL);
    svcExitProcess();
}

static void st_watchdog_tick(void) {
    static u64 start, still, doneAt, limitAt, lastBeat;
    static u32 lastPresent;
    static int drawingLogged;
    u64 now = osGetTime();
    if (!start) start = still = now;
    if (st_done) {
        if (!doneAt) { doneAt = now; ST_PORT_QUIT(); }
#if ST_DONE_GRACE_MS > 0
        if (now - doneAt < (u64)ST_DONE_GRACE_MS) return;
#endif
        st_end_game();
    }
    u64 beat = st_beat;
    u32 present = st_present;
    if (beat != lastBeat) { lastBeat = beat; lastPresent = present; still = now; drawingLogged = 0; }
    else if (present != lastPresent) {
        lastPresent = present; still = now;
        if (!drawingLogged) { st_logf("watchdog: no input passes, but the screen still changes (frame %llu)\n", (unsigned long long)beat); drawingLogged = 1; }
    } else {
        u64 limit = st_hang_ms;
        int starting = beat < ST_BOOT_FRAMES;
        if (starting && limit < ST_BOOT_MS) limit = ST_BOOT_MS;   // a slow-booting game gets 3 minutes
        if (now - still > limit)
            st_cold_restart(starting ? "watchdog: nothing drawn within 3 min of starting (the game hung while starting)"
                                     : "watchdog: no frames for the hang limit (the game hung)");
    }
    if (now - start > ST_LIMIT_MS && !st_limit) { st_limit = 1; limitAt = now; }
    // The game's thread stops the script at its next input pass. A game that still draws but
    // reads no input for 30 s more is stopped from here, the same clean way.
    if (st_limit && !st_done && now - limitAt > 30000)
        st_finish("limit reached (30 minutes): script stopped by the watchdog");
}

static void st_watchdog(void* arg) {
    (void)arg;
    for (;;) {
        svcSleepThread(1000000000LL);
        st_watchdog_tick();
    }
}

static void st_load(const char* script) {
    FILE* f = fopen(script, "r");
    if (!f) return;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    for (char* l = strtok(buf, "\n"); l && st_n < 4096; l = strtok(NULL, "\n")) {
        char* h = strchr(l, '#');
        if (h) *h = 0;
        while (*l == ' ' || *l == '\t') l++;
        size_t k = strlen(l);
        while (k && (l[k - 1] == ' ' || l[k - 1] == '\r' || l[k - 1] == '\t')) l[--k] = 0;
        if (*l) st_lines[st_n++] = l;
    }
}

// Runs before the game's main() (libctru has set up argv by then): take
// --selftest=<script> out of argv, so no port's own option parser ever sees it
// (ScummVM and JA2 quit on unknown options).
static const char* st_scriptArg;
__attribute__((constructor)) static void st_hide_arg(void) {
    int n = 0;
    for (int i = 0; i < __system_argc; i++) {
        char* a = __system_argv[i];
        if (i > 0 && a && !strncmp(a, "--selftest=", 11)) { st_scriptArg = a + 11; continue; }
        __system_argv[n++] = a;
    }
    if (n < __system_argc) __system_argv[n] = NULL;
    __system_argc = n;
}

// The game quit by itself (Lantern: START in its picker) while the writer was
// still saving a shot: exit() then freed the heap under it and the writer
// crashed (data abort in st_bmp, Nov 19 2026). Let the writes finish first.
static void st_drain_at_exit(void) {
    for (int i = 0; i < 100 && st_jobTail != st_jobHead; i++) svcSleepThread(100000000LL);
    if (st_log) {
        st_logf("game exited by itself at frame %llu\n", (unsigned long long)st_frame);
        fclose(st_log); st_log = NULL;
    }
}

static void st_init(void) {
    st_on = 0;
    const char* script = st_scriptArg;
    for (int i = 0; !script && i < __system_argc; i++)   // a port may have changed argv: skip empty entries
        if (__system_argv[i] && !strncmp(__system_argv[i], "--selftest=", 11)) script = __system_argv[i] + 11;
#ifdef ST_EMU_SCRIPT
    if (!script) { FILE* ef = fopen(ST_EMU_SCRIPT, "r"); if (ef) { fclose(ef); script = ST_EMU_SCRIPT; } }   // lets the emulator suite run the hooks too
#endif
    if (!script) return;
    ST_TRACE("st: init (script %s)\n", script);
    snprintf(st_dir, sizeof st_dir, "sdmc:/3ds/testagent/results");
    mkdir("sdmc:/3ds/testagent", 0777);
    mkdir(st_dir, 0777);
    char lp[300];
    snprintf(lp, sizeof lp, "%s/run.log", st_dir);
    st_log = fopen(lp, "w");
    ST_TRACE("st: run.log %s\n", st_log ? "open" : "FAILED");
    st_load(script);
    st_logf("selftest: %d steps from %s\n", st_n, script);
    st_set_target();   // if the game exits by itself mid-test, the loader starts the agent, not the Homebrew Menu
    st_on = 1;
    atexit(st_drain_at_exit);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightEvent_Init(&st_jobEvent, RESET_ONESHOT);
    if (!ST_ONE_THREAD) threadCreate(st_watchdog, NULL, 16 * 1024, prio - 1, -2, true);
    // The writer runs on the New 3DS's extra core (2) by default, so a busy game or sound
    // thread on the game's core can never starve it (it stalled in Gold Box once the demo's
    // sound started). Old 3DS: the game's core. In one-thread mode it is the watchdog too.
    st_writerCore = ST_WRITER_CORE;
    if (!threadCreate(st_writer, NULL, 32 * 1024, prio + ST_WRITER_PRIO_OFFSET, st_writerCore, true)) {
        st_writerCore = -2;
        if (!threadCreate(st_writer, NULL, 32 * 1024, prio + ST_WRITER_PRIO_OFFSET, -2, true)) {
            st_logf("selftest: could not create the writer thread\n");
            ST_TRACE("st: COULD NOT CREATE THREAD\n");
        }
    }
    ST_TRACE("st: writer on core %d, init done\n", st_writerCore);
}

// Circle Pad / C-Stick values: -156..156 are the 3DS's own (X right, Y up). Larger values
// are the emulator suite's -32767..32767 (Y down), scaled so one script runs in both places.
static int st_axis(int v, int isY) {
    if (v > 156 || v < -156) { v = v * 156 / 32767; if (isY) v = -v; }
    return v < -156 ? -156 : v > 156 ? 156 : v;
}

static void st_input_step(long frames) {   // a press-like step: lasts the frames and at least minpress ms
    st_wait = frames > 0 ? frames : 1;
    st_until = st_now + st_minpress;
    st_pressing = 1;
}

static void st_drag_pos(void) {
    long done = st_dragFrames - st_wait;
    // the last held frame is at the end point (the frame after it lets go)
    float pf = st_dragFrames > 1 ? (float)done / (float)(st_dragFrames - 1) : 1.0f;
    float pt = st_dragMs ? (float)(st_now - st_dragStart) / (float)st_dragMs : 1.0f;
    float p = pf < pt ? pf : pt;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    st_tx = st_dx1 + (int)((st_dx2 - st_dx1) * p + (st_dx2 >= st_dx1 ? 0.5f : -0.5f));
    st_ty = st_dy1 + (int)((st_dy2 - st_dy1) * p + (st_dy2 >= st_dy1 ? 0.5f : -0.5f));
}

// One step of the script per frame; returns when the step needs more frames or time.
static void st_step(void) {
    st_prev = st_held;
    if (st_waitMax > 0) {   // waitpix: until the pixel matches, or the limit (frames and time)
        long c = st_pixel(st_waitTop, st_waitX, st_waitY);
        int dr = (int)((c >> 16) & 255) - (int)((st_waitCol >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((st_waitCol >> 8) & 255),
            db = (int)(c & 255) - (int)(st_waitCol & 255);
        int hit = c >= 0 && abs(dr) < 48 && abs(dg) < 48 && abs(db) < 48;
        ++st_waitDone;
        if (!hit && (st_waitDone < st_waitMax || st_now < st_waitPixUntil)) return;
        st_logf("waitpix: %s after %ld frames\n", hit ? "found" : "NOT FOUND", st_waitDone);
        st_waitMax = 0;
    }
    if (st_wait > 0 || st_until) {
        if (st_wait > 0) st_wait--;
        if (st_drag) st_drag_pos();
        if (st_wait > 0 || st_now < st_until) return;
        st_until = 0;
        if (st_pressing) {   // a press ends; held buttons stay
            st_pressing = 0;
            st_held &= st_hold;
            st_touch = st_drag = st_stick = 0;
            st_gapUntil = st_now + st_minrelease;
        }
        return;
    }
    if (st_now < st_gapUntil) return;
    while (st_pc < st_n) {
        char cmd[32] = "", a1[64] = "", a2[64] = "", a3[64] = "", a4[64] = "", a5[64] = "";
        sscanf(st_lines[st_pc++], "%31s %63s %63s %63s %63s %63s", cmd, a1, a2, a3, a4, a5);
        st_logf("> %s %s %s %s %s %s\n", cmd, a1, a2, a3, a4, a5);
        if (!strcmp(cmd, "wait")) { st_wait = atol(a1); if (st_wait > 0) return; continue; }
        if (!strcmp(cmd, "waitms")) { long ms = atol(a1); if (ms > 0) { st_until = st_now + (u64)ms; return; } continue; }
        if (!strcmp(cmd, "minpress")) { st_minpress = (u32)atol(a1); continue; }
        if (!strcmp(cmd, "minrelease")) { st_minrelease = (u32)atol(a1); continue; }
        if (!strcmp(cmd, "press")) { st_held |= st_button(a1); st_input_step(*a2 ? atol(a2) : 6); return; }
        if (!strcmp(cmd, "hold")) { st_hold |= st_button(a1); st_held |= st_button(a1); continue; }
        if (!strcmp(cmd, "release")) { st_hold &= ~st_button(a1); st_held &= ~st_button(a1); continue; }
        if (!strcmp(cmd, "chord")) { st_held |= st_button(a1) | st_button(a2); st_input_step(*a3 ? atol(a3) : 6); return; }
        if (!strcmp(cmd, "tap")) {
            st_tx = atoi(a1); st_ty = atoi(a2); st_touch = 1; st_held |= KEY_TOUCH;
            st_input_step(*a3 ? atol(a3) : 6);
            return;
        }
        if (!strcmp(cmd, "drag")) {   // drag X1 Y1 X2 Y2 [frames]
            st_dx1 = atoi(a1); st_dy1 = atoi(a2); st_dx2 = atoi(a3); st_dy2 = atoi(a4);
            st_dragFrames = *a5 ? atol(a5) : 30;
            if (st_dragFrames < 2) st_dragFrames = 2;
            st_input_step(st_dragFrames);
            // the move takes the frames and at least that long at 60 fps (and minpress), so a
            // game whose loop runs much faster than 60 times a second still sees a drag
            st_dragMs = (u32)(st_dragFrames * 1000 / 60);
            if (st_dragMs < st_minpress) st_dragMs = st_minpress;
            st_until = st_now + st_dragMs + 40;   // then a moment at the end point before letting go
            st_dragStart = st_now;
            st_drag = 1; st_touch = 1; st_held |= KEY_TOUCH;
            st_tx = st_dx1; st_ty = st_dy1;
            return;
        }
        if (!strcmp(cmd, "stick")) {   // stick L|C X Y [frames]
            st_stick = (a1[0] == 'C' || a1[0] == 'c') ? 2 : 1;
            int k = st_stick, x = st_axis(atoi(a2), 0), y = st_axis(atoi(a3), 1);
            const int th = 40;   // past this the direction bits are set, as the system does
            u32 bits = 0;
            if (x > th) bits |= k == 2 ? (u32)KEY_CSTICK_RIGHT : (u32)KEY_CPAD_RIGHT;
            if (x < -th) bits |= k == 2 ? (u32)KEY_CSTICK_LEFT : (u32)KEY_CPAD_LEFT;
            if (y > th) bits |= k == 2 ? (u32)KEY_CSTICK_UP : (u32)KEY_CPAD_UP;
            if (y < -th) bits |= k == 2 ? (u32)KEY_CSTICK_DOWN : (u32)KEY_CPAD_DOWN;
            if (*a4 && atol(a4) == 0) {   // frames 0: keep it there (like hold) until the next "stick L|C X Y 0"
                st_stick = 0;
                st_hold &= ~st_hbits[k]; st_held &= ~st_hbits[k];
                st_hx[k] = x; st_hy[k] = y; st_hbits[k] = bits;
                st_hold |= bits; st_held |= bits;
                continue;
            }
            st_sx = x; st_sy = y; st_held |= bits;
            st_input_step(*a4 ? atol(a4) : 30);
            return;
        }
        if (!strcmp(cmd, "shot")) { st_shot(a1); continue; }
        if (!strcmp(cmd, "waitpix")) {   // waitpix top|bottom X Y RRGGBB MAXFRAMES
            st_waitTop = !strcmp(a1, "top"); st_waitX = atoi(a2); st_waitY = atoi(a3);
            long mx = atol(a5);
            st_waitCol = strtol(a4, NULL, 16); st_waitMax = mx > 0 ? mx : 1; st_waitDone = 0;
            st_waitPixUntil = st_now + (u64)(mx > 0 ? mx : 1) * 1000 / 60;   // and at least as long as at 60 fps
            return;
        }
        if (!strcmp(cmd, "hang")) {   // hang SECS: the game may draw nothing this long (loading); at least 60 s
            int sec = atoi(a1); st_hang_ms = (u32)(sec > 60 ? sec : 60) * 1000; continue;
        }
        if (!strcmp(cmd, "end")) break;
        st_logf("unknown command %s\n", cmd);
    }
    st_finish("end of script");
}

void __wrap_hidScanInput(void) {
    __real_hidScanInput();
    if (st_on < 0) st_init();
    if (st_on != 1) return;
    st_frame = st_frame + 1;
    st_beat = st_frame;
    if (st_frame <= 3 || st_frame % 300 == 0) ST_TRACE("st: frame %lu\n", (unsigned long)st_frame);
    if (st_done) return;
    st_now = osGetTime();
    if (st_limit) { st_finish("limit reached (30 minutes): script stopped"); return; }
    st_step();
}
bool __wrap_aptMainLoop(void) {
#if ST_APT_QUIT
    if (st_done) { ST_PORT_QUIT(); return false; }   // this port quits by itself after the script
#endif
    return __real_aptMainLoop();
}
u32 __wrap_hidKeysHeld(void) { return ST_INJECT && st_on == 1 ? st_held : __real_hidKeysHeld(); }
u32 __wrap_hidKeysDown(void) { return ST_INJECT && st_on == 1 ? (st_held & ~st_prev) : __real_hidKeysDown(); }
u32 __wrap_hidKeysDownRepeat(void) { return ST_INJECT && st_on == 1 ? (st_held & ~st_prev) : __real_hidKeysDownRepeat(); }
u32 __wrap_hidKeysUp(void) { return ST_INJECT && st_on == 1 ? (st_prev & ~st_held) : __real_hidKeysUp(); }
void __wrap_hidTouchRead(touchPosition* pos) {
    if (!ST_INJECT || st_on != 1) { __real_hidTouchRead(pos); return; }
    pos->px = st_touch ? st_tx : 0;
    pos->py = st_touch ? st_ty : 0;
}
void __wrap_hidCircleRead(circlePosition* pos) {
    if (!ST_INJECT || st_on != 1) { __real_hidCircleRead(pos); return; }
    pos->dx = st_stick == 1 ? st_sx : st_hx[1];
    pos->dy = st_stick == 1 ? st_sy : st_hy[1];
}
void __wrap_irrstCstickRead(circlePosition* pos) {
    if (!ST_INJECT || st_on != 1) { __real_irrstCstickRead(pos); return; }
    pos->dx = st_stick == 2 ? st_sx : st_hx[2];
    pos->dy = st_stick == 2 ? st_sy : st_hy[2];
}

#ifdef __cplusplus
}
#endif

#ifdef ST_HAVE_PORT_GLUE
#define ST_PORT_CODE 1
#include "selftest_port.h"
#undef ST_PORT_CODE
#endif
