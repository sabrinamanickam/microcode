/*
 * test_sq_fire.c — fire the new fe_sq patch ONCE, with the crash-localising
 * logging protocol, before anything runs it in a loop.
 *
 * WHY THIS EXISTS. The previous attempt at this patch hard-killed the
 * machine: the box died instantly at 20:55 with nothing in the kernel log
 * (its last message was from 10:03), which is the signature of a
 * microcode-level kill rather than anything the OS got a chance to see.
 * PLAN section 3 records the same protocol for opcode 0x957, which also
 * killed the machine: "the fsync-before-fire logging in probe_shrd.c
 * identified it precisely; keep that pattern".
 *
 * So every stage below writes a line to test_sq_fire.log and fsyncs it
 * before proceeding. If the machine dies again, the last line in that file
 * says exactly which step did it -- installing the patch, the very first
 * firing, or a later one -- instead of costing another boot to guesswork.
 *
 * It fires fe_sq a handful of times, not millions, and checks each result
 * against fiat-crypto. bench_kernel should not be run until this passes.
 *
 * TWO CHANGES SINCE THE CRASHED VERSION:
 *   1. The mask moved from RCX to R8. The fe_sq trigger is
 *      `.byte 0f 78 ca` = `vmread rdx, rcx`, so RCX is the VMCS
 *      field-encoding SOURCE operand. Loading a chosen value into it before
 *      that instruction had no precedent anywhere in this tree. R8 was
 *      already written (as zero) before the same trigger by the old wrapper.
 *   2. No triad holds two multiplies. The crashed patch had a
 *      three-multiply triad and a two-multiply one; no validated patch in
 *      the tree contains either, and probe_sched says clustering is slower
 *      anyway (1.96 vs 1.328 cyc per multiply).
 *
 * Build: make PROG=test_sq_fire
 * Run:   sudo taskset -c 0 ./test_sq_fire_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

static int g_log = -1;
static void stage(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) { (void)!write(g_log, buf, n); fsync(g_log); }
    (void)!write(1, buf, n);            /* unbuffered: stdio can lose a crash */
}

int main(void) {
    if (geteuid() != 0) { printf("needs root: sudo taskset -c 0 ./test_sq_fire_static\n"); return 1; }
    g_log = open("test_sq_fire.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    stage("00 start: about to call lib-micro helpers");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    stage("01 helpers done, microcode state is stock");

    stage("02 about to patch_ucode + hook (fe_mul 58 triads, fe_sq 41 triads,\n          three same-register MULs eliminated)");
    install_field_patches();
    stage("03 patches installed and hooked -- install did NOT crash");

    /* Inputs chosen to exercise the reduction, smallest first so the very
     * first firing is the least exotic thing the patch will ever see. */
    static const uint64_t cases[][5] = {
        { 1, 0, 0, 0, 0 },
        { 2, 3, 5, 7, 11 },
        { MASK51, MASK51, MASK51, MASK51, MASK51 },
        { MASK51 - 18, MASK51, MASK51, MASK51, MASK51 },
        { (1ULL<<53)-1, (1ULL<<53)-1, (1ULL<<53)-1, (1ULL<<53)-1, (1ULL<<53)-1 },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint64_t a[5], got[5], want[5], ra[5], rb[5];
        memcpy(a, cases[i], 40);
        stage("04.%u about to FIRE fe_sq, a = %016lx %016lx %016lx %016lx %016lx",
              i, a[0], a[1], a[2], a[3], a[4]);
        fe_sq_ucode(a, got);
        stage("05.%u fired OK, h = %016lx %016lx %016lx %016lx %016lx",
              i, got[0], got[1], got[2], got[3], got[4]);
        fe_sq_fiat(a, want);
        fe_reduce(ra, got); fe_reduce(rb, want);
        if (memcmp(ra, rb, 40)) { bad++; stage("06.%u MISMATCH vs fiat-crypto", i); }
        else                      stage("06.%u matches fiat-crypto", i);
    }

    stage("07 all firings survived; %d mismatches", bad);
    if (!bad) {
        stage("08 about to run the RFC 7748 gate (exercises the full ladder)");
        int f = test_rfc7748();
        stage("09 RFC 7748 returned %d", f);
        bad += f;
    }

    init_match_and_patch();
    do_fix_IN_patch();
    stage("10 microcode state restored; verdict %s", bad ? "FAIL" : "PASS");
    if (g_log >= 0) close(g_log);
    return bad ? 1 : 0;
}
