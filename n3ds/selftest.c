// selftest.c - run a port's test script on a real 3DS, without anyone holding it.
//
// Linked into a port with the --wrap flags in selftest.mk. When the port is
// started with the argument "--selftest=<script>" (the test agent does this),
// the script presses buttons, taps and moves the sticks by replacing what the
// HID functions return, and saves both screens as BMP files. At the end it
// tells the Homebrew loader to start the test agent again and exits, so the
// next test can run. Without the argument nothing changes.
//
// Script (same language as the emulator suite, scripts/<port>.txt):
//   wait N | press BTN [frames] | hold BTN | release BTN | chord HELD BTN [frames]
//   tap X Y [frames] | stick L|C X Y FRAMES | shot NAME | hang N | end
// One "frame" is one call of hidScanInput.
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static int st_done;               // script finished: the app is asked to quit
static u64 st_doneFrame;
static int st_on = -1;            // -1 not checked yet, 0 off, 1 running
static char st_dir[256];          // results folder
static char* st_lines[4096];
static int st_n, st_pc;
static FILE* st_log;
static u64 st_frame;
static long st_wait;              // frames left in the current step
static u32 st_held, st_prev, st_tapFrames, st_hold;
static int st_tx, st_ty;
static int st_stick, st_sx, st_sy, st_stickFrames;

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

// Both screens as 24-bit BMP files, read from what the screens are showing now.
// The whole file is built in memory and written with one call: the SD card
// is shared with the game, and on real hardware 240 small row writes queued
// behind Gold Box's own SD reads took minutes per shot.
static void st_bmp(const char* path, const u8* fb, int fmt, int w, int h, int stride) {
    int row = (w * 3 + 3) & ~3;
    u32 size = 54 + row * h;
    u8* buf = (u8*)calloc(1, size);
    if (!buf) return;
    u8* hd = buf;
    hd[0] = 'B'; hd[1] = 'M';
    memcpy(hd + 2, &size, 4);
    u32 v = 54; memcpy(hd + 10, &v, 4);
    v = 40; memcpy(hd + 14, &v, 4);
    memcpy(hd + 18, &w, 4);
    memcpy(hd + 22, &h, 4);
    hd[26] = 1; hd[28] = 24;
    int bpp = fmt == GSP_RGBA8_OES ? 4 : fmt == GSP_BGR8_OES ? 3 : 2;
    for (int y = h - 1; y >= 0; y--) {           // BMP rows go bottom-up
        u8* line = buf + 54 + (size_t)(h - 1 - y) * row;
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
    }
    FILE* f = fopen(path, "wb");
    if (f) {
        setvbuf(f, NULL, _IONBF, 0);             // straight to the card in one request
        fwrite(buf, 1, size, f);
        fclose(f);
    }
    free(buf);
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

// Screenshots: the screens are copied to memory at once (fast, on the game's
// thread) and written to the SD card by a separate thread, so a slow SD card
// never holds the game up (a shot froze Gold Box on the real 3DS).
typedef struct { char path[300]; u8* px; int fmt, w, stride; } StShotJob;
static StShotJob st_jobs[8];
static volatile int st_jobHead, st_jobTail;
static LightEvent st_jobEvent;
// What the writer is doing, read by the game's thread for the log (the writer
// itself never logs, so the two threads never share the log file).
static const char* volatile st_writing;
static volatile u32 st_wroteN, st_wroteMs, st_wroteMaxMs;
static int st_writerCore = -2;
static void st_writer(void* arg) {
    (void)arg;
    for (;;) {
        LightEvent_Wait(&st_jobEvent);
        while (st_jobTail != st_jobHead) {
            StShotJob* j = &st_jobs[st_jobTail % 8];
            st_writing = j->path;
            u64 t0 = osGetTime();
            st_bmp(j->path, j->px, j->fmt, j->w, 240, j->stride);
            u32 ms = (u32)(osGetTime() - t0);
            st_writing = NULL;
            st_wroteN++;
            st_wroteMs += ms;
            if (ms > st_wroteMaxMs) st_wroteMaxMs = ms;
            free(j->px);
            j->px = NULL;
            st_jobTail++;
        }
    }
}
static void st_shot(const char* name) {
    GSPGPU_CaptureInfo ci;
    if (R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&ci))) { st_logf("shot %s: capture failed\n", name); return; }
    for (int s = 0; s < 2; s++) {
        GSPGPU_CaptureInfoEntry* e = &ci.screencapture[s];
        if (st_jobHead - st_jobTail >= 8) { st_logf("shot %s: writer behind, skipped\n", name); return; }
        StShotJob* j = &st_jobs[st_jobHead % 8];
        j->fmt = e->format & 7;
        // the top screen in the 800-pixel mode: neither the 2D (bit 6) nor the 3D (bit 5) flag
        j->w = s == 0 ? ((e->format & 0x60) == 0 ? 800 : 400) : 320;
        j->stride = e->framebuf_widthbytesize;
        size_t n = (size_t)j->w * j->stride;
        j->px = (u8*)malloc(n);
        if (!j->px) { st_logf("shot %s: no memory\n", name); return; }
        memcpy(j->px, e->framebuf0_vaddr, n);
        snprintf(j->path, sizeof j->path, "%s/%s_%s.bmp", st_dir, name, s == 0 ? "top" : "bottom");
        st_jobHead++;
    }
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

// The script ended: quit the normal way. aptMainLoop() now returns false, so the
// game runs its own cleanup (calling exit() from inside a frame crashed the GSP
// thread). If the game does not get out by itself, the watchdog ends it.
static void st_finish(const char* why) {
    for (int i = 0; i < 200 && st_jobTail != st_jobHead; i++) svcSleepThread(100000000LL);   // let the last shots reach the SD card
    const char* w = st_writing;
    st_logf("writer: core %d, %lu files, %lu ms in all, slowest %lu ms\n", st_writerCore,
            (unsigned long)st_wroteN, (unsigned long)st_wroteMs, (unsigned long)st_wroteMaxMs);
    if (st_jobTail != st_jobHead)
        st_logf("writer: %d files never written, stuck on %s\n", st_jobHead - st_jobTail, w ? w : "(waiting for the CPU)");
    st_logf("%s\ndone frames=%llu\n", why, (unsigned long long)st_frame);
    st_set_target();
    st_done = 1;
    st_doneFrame = st_frame;
}

// Watchdog thread: nobody has to look after the 3DS. It ends the game and
// returns to the agent when the game does not quit within 15 s of the script's
// end, when frames stop for 60 s (a hang), or after 30 minutes in all.
static volatile u64 st_beat;
static void st_watchdog(void* arg) {
    (void)arg;
    u64 last = 0, still = 0, doneAt = 0, start = osGetTime();
    for (;;) {
        svcSleepThread(1000000000LL);
        u64 now = osGetTime();
        const char* why = NULL;
        if (st_done) {
            // End the game ourselves as soon as the script is done. Asking it to
            // quit (aptMainLoop false) crashed ScummVM on the real 3DS: its input
            // thread still read the buttons after HID was shut down (Nov 13 2026).
            if (!doneAt) doneAt = now;
            why = "";
        } else {
            if (st_beat != last) { last = st_beat; still = now; }
            else if (now - still > 60000) why = "watchdog: no frames for 60 s (the game hung)";
            if (now - start > 30 * 60 * 1000) why = "watchdog: the test ran longer than 30 minutes";
        }
        if (why) {
            if (*why) st_logf("%s\n%s\n", why, st_done ? "" : "done frames=0 (stopped by the watchdog)");
            if (st_log) { fclose(st_log); st_log = NULL; }
            st_set_target();
            aptSetChainloaderToSelf();   // relaunch the Homebrew loader, which starts the agent
            aptExit();
            svcExitProcess();
        }
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

static void st_init(void) {
    st_on = 0;
    const char* script = st_scriptArg;
    for (int i = 0; !script && i < __system_argc; i++)   // a port may have changed argv: skip empty entries
        if (__system_argv[i] && !strncmp(__system_argv[i], "--selftest=", 11)) script = __system_argv[i] + 11;
    if (!script) return;
    snprintf(st_dir, sizeof st_dir, "sdmc:/3ds/testagent/results");
    mkdir("sdmc:/3ds/testagent", 0777);
    mkdir(st_dir, 0777);
    char lp[300];
    snprintf(lp, sizeof lp, "%s/run.log", st_dir);
    st_log = fopen(lp, "w");
    st_load(script);
    st_logf("selftest: %d steps from %s\n", st_n, script);
    st_set_target();   // if the game exits by itself mid-test, the loader starts the agent, not the Homebrew Menu
    st_on = 1;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    threadCreate(st_watchdog, NULL, 16 * 1024, prio - 1, -2, true);
    LightEvent_Init(&st_jobEvent, RESET_ONESHOT);
    // The writer runs on the New 3DS's extra core (2), so a busy game or sound
    // thread on the game's core can never starve it (it stalled in Gold Box
    // once the demo's sound started). Old 3DS: the game's core, as before.
    st_writerCore = 2;
    if (!threadCreate(st_writer, NULL, 32 * 1024, prio + 2, 2, true)) {
        st_writerCore = -2;
        threadCreate(st_writer, NULL, 32 * 1024, prio + 2, -2, true);
    }
}

// One step of the script per frame; returns when the step needs more frames.
static void st_step(void) {
    st_prev = st_held;
    if (st_tapFrames) st_tapFrames--;
    if (st_stickFrames && !--st_stickFrames) st_stick = 0;
    if (st_waitMax > 0) {   // waitpix: until the pixel matches, or the limit
        long c = st_pixel(st_waitTop, st_waitX, st_waitY);
        int dr = (int)((c >> 16) & 255) - (int)((st_waitCol >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((st_waitCol >> 8) & 255),
            db = (int)(c & 255) - (int)(st_waitCol & 255);
        int hit = c >= 0 && abs(dr) < 48 && abs(dg) < 48 && abs(db) < 48;
        if (!hit && ++st_waitDone < st_waitMax) return;
        st_logf("waitpix: %s after %ld frames\n", hit ? "found" : "NOT FOUND", st_waitDone);
        st_waitMax = 0;
    }
    if (st_wait > 0) {
        if (--st_wait == 0) st_held &= st_hold;   // a press ends; held buttons stay
        return;
    }
    while (st_pc < st_n) {
        char cmd[32] = "", a1[64] = "", a2[64] = "", a3[64] = "", a4[64] = "";
        sscanf(st_lines[st_pc++], "%31s %63s %63s %63s %63s", cmd, a1, a2, a3, a4);
        st_logf("> %s %s %s %s %s\n", cmd, a1, a2, a3, a4);
        if (!strcmp(cmd, "wait")) { st_wait = atol(a1); return; }
        if (!strcmp(cmd, "press")) { st_held |= st_button(a1); st_wait = *a2 ? atol(a2) : 6; return; }
        if (!strcmp(cmd, "hold")) { st_hold |= st_button(a1); st_held |= st_button(a1); continue; }
        if (!strcmp(cmd, "release")) { st_hold &= ~st_button(a1); st_held &= ~st_button(a1); continue; }
        if (!strcmp(cmd, "chord")) { st_held |= st_button(a1) | st_button(a2); st_wait = *a3 ? atol(a3) : 6; return; }
        if (!strcmp(cmd, "tap")) { st_tx = atoi(a1); st_ty = atoi(a2); st_tapFrames = *a3 ? atoi(a3) : 6; st_held |= KEY_TOUCH; st_wait = st_tapFrames; return; }
        if (!strcmp(cmd, "stick")) { st_stick = a1[0] == 'C' ? 2 : 1; st_sx = atoi(a2); st_sy = atoi(a3); st_stickFrames = atoi(a4); continue; }
        if (!strcmp(cmd, "shot")) { st_shot(a1); continue; }
        if (!strcmp(cmd, "waitpix")) {   // waitpix top|bottom X Y RRGGBB MAXFRAMES
            st_waitTop = !strcmp(a1, "top"); st_waitX = atoi(a2); st_waitY = atoi(a3);
            char col[16] = ""; long mx = 0;
            sscanf(st_lines[st_pc - 1], "%*s %*s %*s %*s %15s %ld", col, &mx);
            st_waitCol = strtol(col, NULL, 16); st_waitMax = mx; st_waitDone = 0;
            return;
        }
        if (!strcmp(cmd, "hang")) continue;   // the emulator suite checks hangs; here the agent times out
        if (!strcmp(cmd, "end")) break;
    }
    st_finish("end of script");
}

void __wrap_hidScanInput(void) {
    __real_hidScanInput();
    if (st_on < 0) st_init();
    if (st_on != 1) return;
    st_frame++;
    st_beat = st_frame;
    if (st_done) {
        st_held = st_prev = 0;
        return;
    }
    st_step();
}
bool __wrap_aptMainLoop(void) {   // the watchdog ends the game after the script; no quit request
    return __real_aptMainLoop();
}
u32 __wrap_hidKeysHeld(void) { return st_on == 1 ? st_held : __real_hidKeysHeld(); }
u32 __wrap_hidKeysDown(void) { return st_on == 1 ? (st_held & ~st_prev) : __real_hidKeysDown(); }
u32 __wrap_hidKeysDownRepeat(void) { return st_on == 1 ? (st_held & ~st_prev) : __real_hidKeysDownRepeat(); }
u32 __wrap_hidKeysUp(void) { return st_on == 1 ? (st_prev & ~st_held) : __real_hidKeysUp(); }
void __wrap_hidTouchRead(touchPosition* pos) {
    if (st_on != 1) { __real_hidTouchRead(pos); return; }
    pos->px = st_tapFrames ? st_tx : 0;
    pos->py = st_tapFrames ? st_ty : 0;
}
void __wrap_hidCircleRead(circlePosition* pos) {
    if (st_on != 1) { __real_hidCircleRead(pos); return; }
    pos->dx = st_stick == 1 ? st_sx : 0;
    pos->dy = st_stick == 1 ? st_sy : 0;
}
void __wrap_irrstCstickRead(circlePosition* pos) {
    if (st_on != 1) { __real_irrstCstickRead(pos); return; }
    pos->dx = st_stick == 2 ? st_sx : 0;
    pos->dy = st_stick == 2 ? st_sy : 0;
}

#ifdef __cplusplus
}
#endif
