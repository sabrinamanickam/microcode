/*
 * probe_seg.c — Find which microcode SEG value makes LDZX/STAD work on THIS box.
 *
 * Background: a single LDZX hangs the core in our Keccak harness. The one
 * confirmed-working memory-I/O example on this hardware (tools/backdoor) uses
 * SEG_DS (0x18); test_memops used SEG=3. The working segment is machine-
 * specific (that's why test_ldzx.c sweeps it). A wrong SEG hangs the core so
 * hard that in-process watchdogs don't fire and the box can crash outright.
 *
 * SAFETY MODEL — because a bad SEG can take the NUC down:
 *   1. ONE seg per invocation. Pass it as argv[1] (hex). Default = SEG_DS.
 *      Run it, read the log, then run the next candidate. Never sweep blindly.
 *   2. fsync a log line BEFORE firing. If the box hard-crashes, after reboot
 *      the last "ATTEMPT" with no "RESULT" identifies the fatal seg.
 *   3. Restore the match-and-patch table at start AND end so a stale hook from
 *      a previous hung run can't poison this one.
 *
 * Hook: vmwrite (0x0cd8) — only our explicit vmwrite triggers it, so glibc /
 * the kernel can't fire the patch with garbage registers (unlike rdrand).
 *
 * Patch: single triad — LDZX [RCX] -> RAX, END. RCX = &g_buf, g_buf[0] holds
 * a sentinel. If the seg works, RAX == sentinel.
 *
 * Candidate segs to try, in order of likelihood:
 *   0x18 (SEG_DS)  <- start here (backdoor uses this)
 *   0x1a (SEG_SS)
 *   0x08 (SEG_ES)
 *   0x01 (PHYS), 0x00, 0x03, 0x06   <- speculative, try last
 *
 * Build: make PROG=probe_seg
 * Run:   sudo taskset -c 0 ./probe_seg_static 0x18
 *        (then check probe_seg.log, then try the next seg)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define SENTINEL 0xDEADBEEFCAFE1234ULL
#define LOGPATH  "probe_seg.log"

/* g_buf must be < 4GB for LDZX ASZ32. -no-pie keeps static globals low. */
static uint64_t g_buf[8] = { SENTINEL, 0, 0, 0, 0, 0, 0, 0 };

/* Append a line to the log and force it to disk before returning. This is the
 * crash-survival mechanism: if the system dies on the next instruction, this
 * line is already persisted. */
static void logline(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int fd = open(LOGPATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror("open log"); return; }
    write(fd, buf, strlen(buf));
    fsync(fd);
    close(fd);

    /* Also echo to stdout for live runs. */
    fputs(buf, stdout);
    fflush(stdout);
}

int main(int argc, char **argv) {
    unsigned long seg = 0x18; /* SEG_DS default */
    if (argc >= 2) seg = strtoul(argv[1], NULL, 0);

    if ((uint64_t)g_buf >= 0x100000000ULL) {
        printf("FATAL: g_buf above 4GB (%p). Rebuild with -no-pie.\n", (void*)g_buf);
        return 1;
    }

    logline("=== probe_seg run: seg=0x%02lx  g_buf=%p ===\n", seg, (void*)g_buf);

    assign_to_core(0);

    /* Restore table first — clears any stale hook from a prior hung run. */
    init_match_and_patch();
    do_fix_IN_patch();

    /* Single-triad patch: load [RCX] -> RAX, END. */
    ucode_t patch[1] = {{
        LDZX_DSZ64_ASZ32_SC1_DR(RAX, RCX, seg),
        NOP, NOP, END_SEQWORD
    }};
    patch_ucode(0x7c00, patch, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);

    /* THE DANGEROUS LINE — persist intent before firing. */
    logline("ATTEMPT seg=0x%02lx  about to fire vmwrite (LDZX [g_buf]=0x%016" PRIx64 ")\n",
            seg, g_buf[0]);

    uint64_t result = 0;
    asm volatile(
        "lea rcx, [%[addr]]\n\t"
        "vmwrite rcx, rcx\n\t"
        "mov %[out], rax\n\t"
        : [out] "=r"(result)
        : [addr] "m"(g_buf[0])
        : "rax", "rcx", "rdx", "memory", "cc"
    );

    /* If we got here, the seg did NOT hang. Record the outcome. */
    logline("RESULT  seg=0x%02lx  rax=0x%016" PRIx64 "  %s\n",
            seg, result,
            result == SENTINEL ? "PASS (seg works!)" : "WRONG (no hang, bad value)");

    /* Restore table so the next run / shell isn't left hooked. */
    init_match_and_patch();
    do_fix_IN_patch();

    logline("=== probe_seg done: seg=0x%02lx ===\n\n", seg);
    return result == SENTINEL ? 0 : 2;
}
