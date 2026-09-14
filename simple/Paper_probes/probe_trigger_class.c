/*
 * probe_trigger_class.c — does the CHOICE of hooked instruction change the
 *                         cost of crossing into patch RAM?
 *
 * `curve25519/probe_trigger.c` already crossed vmwrite against vmread and found
 * the difference worth 0.0 cycles. But those are both VMX instructions, decoded
 * the same way and reaching the microsequencer by the same route, so the result
 * only shows that two members of one family agree. It does not test a
 * structurally different instruction. `loss-againstamd64-64.md:203` still lists
 * a cheaper trigger as an untested idea.
 *
 * This probe holds everything else fixed — one triad at U7c00, the same
 * dependency, the same loop — and varies only which macroinstruction is hooked.
 *
 * METHOD.  Copied from probe_vmwrite_cost.c so the numbers are comparable.
 * The patch is ZEROEXT_DSZ64_DR(RDI, RDI): each firing reads RDI and writes
 * RDI, so consecutive firings carry a true RAW dependency and the loop measures
 * firing-to-firing latency with the operands already in registers. Subtract the
 * empty-loop floor, then 1 cycle for the single triad, and what is left is the
 * cost of the crossing itself.
 *
 * TRIGGERS AND WHY THIS SET.  A hook is CPU-wide state on that core: any code,
 * ours or the kernel's, that executes the hooked instruction is redirected into
 * our patch. That rules most candidates out.
 *
 *   vmwrite 0x0cd8   safe. #UD outside VMX operation, so nothing else runs it.
 *   vmread  0x0618   safe, same reason. The production pair.
 *   rdseed  0x0430   rarely executed by anything else. Low risk.
 *   rdrand  0x0428   glibc and the kernel DO execute it. While the hook is
 *                    installed they get our patch instead, with a non-random
 *                    result and a clobbered RDI. Short window, opt-in only.
 *   rdtscp  0x0788   EXCLUDED. The vDSO uses it for clock_gettime; hooking it
 *                    breaks timekeeping for every process on the core, this
 *                    one included.
 *   pause   0x0bf0   EXCLUDED. Every spin loop in the kernel and in userspace.
 *
 * If rdseed and rdrand land on vmwrite's number, the crossing is
 * instruction-agnostic and the question is closed.
 *
 * SAFETY (probe_memops.c model): one arm per invocation, selected by argv[1];
 * an ATTEMPT line is fsynced before each firing so a hang names its own arm
 * after a reboot; the match table is restored at entry and at exit. Arm 4
 * (rdrand) must be asked for explicitly and is not part of the default run.
 *
 * Build: make PROG=probe_trigger_class
 * Run:   sudo taskset -c 0 ./probe_trigger_class_static        (arms 1-3)
 *        sudo taskset -c 0 ./probe_trigger_class_static 4      (rdrand, opt-in)
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
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

#define N_CALLS    100000
#define BENCH_REPS 100
#define LOGPATH    "probe_trigger_class.log"

static void logline(const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    b[n++] = '\n';
    int fd = open(LOGPATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { ssize_t w = write(fd, b, n); (void)w; fsync(fd); close(fd); }
}

static inline uint64_t tsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t tsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* One triad: RDI = RDI. Reads and writes the same register, so back-to-back
 * firings form a dependent chain with no memory traffic at all. */
static void install(uint64_t xlat) {
    ucode_t p[] = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, 1);
    hook_match_and_patch(0, xlat, 0x7c00);
}
static void uninstall(void) { init_match_and_patch(); do_fix_IN_patch(); }

#define TIME_LOOP(BODY, OUT)                                              \
    do {                                                                  \
        uint64_t s[BENCH_REPS];                                           \
        for (int r = 0; r < BENCH_REPS; r++) {                            \
            uint64_t t0 = tsc_start();                                    \
            for (int i = 0; i < N_CALLS; i++) { BODY }                    \
            uint64_t t1 = tsc_end();                                      \
            s[r] = t1 - t0;                                               \
        }                                                                 \
        qsort(s, BENCH_REPS, sizeof s[0], cmp_u64);                       \
        OUT = s[0];                                                       \
    } while (0)

int main(int argc, char **argv) {
    int only = (argc > 1) ? atoi(argv[1]) : 0;

    printf("================================================================\n");
    printf("  probe_trigger_class — is the crossing cost the same for every\n");
    printf("  hooked instruction?  1 triad (ZEROEXT rdi,rdi) at U7c00,\n");
    printf("  dependent chain through RDI, no memory traffic.\n");
    printf("================================================================\n");

    assign_to_core(0);
    uninstall();

    /* Loop framing floor, measured with no hook installed at all. */
    uint64_t empty;
    TIME_LOOP(asm volatile("" ::: "memory");, empty);
    printf("\n  empty loop floor: %.2f cyc/iter\n\n", empty / (double)N_CALLS);

    struct { int id; const char *name; uint64_t xlat; int risky; } arms[] = {
        { 1, "vmwrite  (0x0cd8)", 0x0cd8, 0 },
        { 2, "vmread   (0x0618)", 0x0618, 0 },
        { 3, "rdseed   (0x0430)", 0x0430, 0 },
        { 4, "rdrand   (0x0428)", 0x0428, 1 },
    };

    printf("  %-20s %12s %12s\n", "trigger", "cyc/call", "minus triad");
    printf("  %-20s %12s %12s\n", "--------------------", "------------", "------------");

    double base = -1.0;
    for (unsigned k = 0; k < sizeof arms / sizeof arms[0]; k++) {
        if (only && arms[k].id != only) continue;
        if (!only && arms[k].risky) continue;

        logline("ATTEMPT arm=%d %s", arms[k].id, arms[k].name);
        install(arms[k].xlat);

        uint64_t t = 0;
        switch (arms[k].id) {
        case 1: TIME_LOOP(asm volatile("vmwrite rcx, rdx" ::: "rcx","rdx","rdi","memory","cc");, t); break;
        case 2: TIME_LOOP(asm volatile(".byte 0x0f,0x78,0xca" ::: "rcx","rdx","rdi","memory","cc");, t); break;
        case 3: TIME_LOOP(asm volatile(".byte 0x0f,0xc7,0xf8" ::: "rax","rdi","memory","cc");, t); break;
        case 4: TIME_LOOP(asm volatile(".byte 0x0f,0xc7,0xf0" ::: "rax","rdi","memory","cc");, t); break;
        }

        uninstall();   /* close the window immediately, rdrand especially */
        logline("RESULT  arm=%d total=%" PRIu64, arms[k].id, t);

        double per = ((int64_t)t - (int64_t)empty) / (double)N_CALLS;
        printf("  %-20s %12.2f %12.2f\n", arms[k].name, per, per - 1.0);
        if (base < 0) base = per;
        else printf("  %-20s %12s %+12.2f  vs first arm\n", "", "", per - base);
    }

    uninstall();
    printf("\n  A spread of ~0 means the crossing is instruction-agnostic: the\n");
    printf("  match compares an MSROM address, and which instruction produced\n");
    printf("  that address does not reach the cost.\n");
    printf("================================================================\n");
    return 0;
}
