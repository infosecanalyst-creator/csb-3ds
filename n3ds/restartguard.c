// restartguard.c - a crashed port restarts the console by itself (Nov 20 2026).
// Always on (not only during scripted tests). Add this file to a port's build; nothing else
// is needed: a constructor installs the handler on the main thread before main() runs.
// A port's own threadOnException() call later simply replaces it.
//
// On a CPU exception (data abort, prefetch abort, undefined instruction) the handler writes one
// line to sdmc:/3ds/restartguard.log, then cold-restarts the console through the MCU (the register
// Luma's own arm9 uses). Luma treats it as a power-on and autoboots /boot.3dsx (the test agent), so
// nobody has to hold POWER. APT_HardwareResetAsync is the fallback (lands on the HOME Menu).
//
// Limits: threadOnException is per thread, so other threads call restartguard_thread() first thing.
// svcBreak(USERBREAK_PANIC) and kernel panics never reach a user handler: Luma shows its own
// exception screen for those and waits for a button.
#include <3ds.h>
#include <stdio.h>
#include <string.h>

static u8 rg_stack[0x2000] __attribute__((aligned(8)));
static volatile int rg_busy;

static void rg_handler(ERRF_ExceptionInfo* e, CpuRegisters* r) {
    if (rg_busy) for (;;) svcSleepThread(1000000000LL);   // a second thread crashed while the first restarts
    rg_busy = 1;
    FILE* f = fopen("sdmc:/3ds/restartguard.log", "a");
    if (f) {
        fprintf(f, "CRASH type %d pc=%08lx lr=%08lx sp=%08lx far=%08lx\n", (int)e->type,
                (unsigned long)r->pc, (unsigned long)r->lr, (unsigned long)r->sp, (unsigned long)e->far);
        fclose(f);
    }
    svcSleepThread(1000000000LL);
    if (R_SUCCEEDED(mcuHwcInit())) {
        u8 v = 1 << 2;
        MCUHWC_WriteRegister(0x20, &v, 1);
        mcuHwcExit();
        svcSleepThread(3000000000LL);
    }
    APT_HardwareResetAsync();
    for (;;) svcSleepThread(1000000000LL);
}

void restartguard_thread(void) {
    // each thread needs its own handler stack
    static u8 extra[4][0x1800] __attribute__((aligned(8)));
    static volatile int n;
    int i = __sync_fetch_and_add(&n, 1);
    if (i < 4) threadOnException(rg_handler, extra[i] + sizeof extra[i], WRITE_DATA_TO_HANDLER_STACK);
}

__attribute__((constructor)) static void rg_install(void) {
    threadOnException(rg_handler, rg_stack + sizeof rg_stack, WRITE_DATA_TO_HANDLER_STACK);
}
