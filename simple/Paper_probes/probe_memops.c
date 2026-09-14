/*
 * probe_memops.c — Directly test the "≤1 memory op per triad" rule.
 *
 * The whole toolchain enforces ≤1 LDZX/STAD per triad on the belief that a
 * second memory op "hangs the core (single L1 port)". But NO probe ever placed
 * two memory ops in one triad: every existing probe puts each LDZX/STAD alone
 * in slot 0. This probe tests the rule head-on. If two mem ops per triad work,
 * the 50-triad Keccak prologue+epilogue (25 single-load + 25 single-store) could
 * nearly halve, freeing ~25 triads of the 128-triad patch RAM budget.
 *
 * Four combinations, ONE per invocation (argv[1]=1..4) so a hang is isolated:
 *   1  two LOADs   in one triad:  { LDZX RDI<-[0], LDZX RSI<-[1], NOP }
 *   2  two STORES  in one triad:  { STAD RDI->[5], STAD RSI->[6], NOP }
 *   3  LOAD+STORE  in one triad:  { LDZX RDI<-[0], STAD RSI->[6], NOP }
 *   4  STORE+LOAD same addr:      { STAD RDI->[5], LDZX RSI<-[5], NOP }  (fwd?)
 *
 * SAFETY MODEL (mirrors probe_seg.c — a hang can take the box down hard):
 *   - ONE test per invocation; pass it as argv[1]. Default = 1 (loads, safest).
 *   - fsync an "ATTEMPT" log line BEFORE firing. If the box hard-hangs, after
 *     reboot the last ATTEMPT with no RESULT names the fatal combination.
 *   - Restore the match-and-patch table at start AND end, so a stale hook from a
 *     previous hung run can't poison this one.
 *   - vmwrite hook (0x0cd8): only our explicit vmwrite triggers the patch, so
 *     glibc / the kernel can't fire it with garbage registers (unlike rdrand).
 *
 * Buffer layout (all within the +127-byte signed-8 offset range, probe_offset):
 *   buf[0..3] = inputs A,B,C,D ;  buf[5..7] = outputs.
 *
 * Build: make PROG=probe_memops
 * Run:   sudo taskset -c 0 ./probe_memops_static 1   (then 2, then 3, then 4)
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
#include "../../../../include/patch.h"
#include "../../../../include/ucode_macro.h"
#include "../../../../include/misc.h"

#define SEG 0x18                 /* SEG_DS — confirmed working (probe_seg) */
#define LOGPATH "probe_memops.log"

#define A 0x1111111111111111ULL
#define B 0x2222222222222222ULL
#define C 0x4444444444444444ULL
#define D 0x8888888888888888ULL

/* must stay < 4GB for ASZ32 (-no-pie keeps static globals low) */
static uint64_t g_buf[16];

/* Append a log line and force it to disk BEFORE returning — crash survival. */
static void logline(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open(LOGPATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, buf, strlen(buf)); fsync(fd); close(fd); }
    fputs(buf, stdout);
    fflush(stdout);
}

static void fire(void) {
    register uint64_t *_b asm("rcx") = g_buf;
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

/* off in BYTES from base RCX = &g_buf[0]. idx*8. */
#define LDT(reg, idx) LDZX_DSZ64_ASZ32_SC1_DRI(reg, RCX, (idx)*8, SEG)
#define STT(reg, idx) STAD_DSZ64_ASZ32_SC1_RRI(reg, RCX, (idx)*8, SEG)

int main(int argc, char **argv) {
    int t = (argc >= 2) ? atoi(argv[1]) : 1;

    printf("=== probe_memops: two memory ops in one triad (test %d) ===\n", t);
    printf("g_buf=%p (below 4GB: %s)\n", (void*)g_buf,
           (uint64_t)g_buf < 0x100000000ULL ? "YES" : "NO");
    if ((uint64_t)g_buf >= 0x100000000ULL) { printf("FATAL >4GB; need -no-pie\n"); return 1; }
    assign_to_core(0);

    /* clear any stale hook from a prior hung run */
    init_match_and_patch(); do_fix_IN_patch();

    ucode_t patch[8];
    int n = 0;
    /* per-test: which output indices to check and expected values */
    int   chk_idx[2];
    uint64_t chk_exp[2];
    int   nchk = 0;
    const char *desc = "";

    switch (t) {
    case 1: /* two LOADs in one triad, then store both back */
        desc = "two LOADs in one triad { LDZX RDI<-[0], LDZX RSI<-[1] }";
        patch[n++] = (ucode_t){ LDT(RDI,0), LDT(RSI,1), NOP, NOP_SEQWORD };  /* DANGEROUS */
        patch[n++] = (ucode_t){ STT(RDI,5), NOP, NOP, NOP_SEQWORD };
        patch[n++] = (ucode_t){ STT(RSI,6), NOP, NOP, END_SEQWORD };
        chk_idx[0]=5; chk_exp[0]=A; chk_idx[1]=6; chk_exp[1]=B; nchk=2;
        break;
    case 2: /* two STORES in one triad (loads done safely first) */
        desc = "two STORES in one triad { STAD RDI->[5], STAD RSI->[6] }";
        patch[n++] = (ucode_t){ LDT(RDI,0), NOP, NOP, NOP_SEQWORD };
        patch[n++] = (ucode_t){ LDT(RSI,1), NOP, NOP, NOP_SEQWORD };
        patch[n++] = (ucode_t){ STT(RDI,5), STT(RSI,6), NOP, END_SEQWORD };   /* DANGEROUS */
        chk_idx[0]=5; chk_exp[0]=A; chk_idx[1]=6; chk_exp[1]=B; nchk=2;
        break;
    case 3: /* LOAD + STORE (distinct addresses) in one triad */
        desc = "LOAD+STORE in one triad { LDZX RDI<-[0], STAD RSI->[6] }";
        patch[n++] = (ucode_t){ LDT(RSI,1), NOP, NOP, NOP_SEQWORD };          /* preload RSI=B */
        patch[n++] = (ucode_t){ LDT(RDI,0), STT(RSI,6), NOP, NOP_SEQWORD };   /* DANGEROUS */
        patch[n++] = (ucode_t){ STT(RDI,5), NOP, NOP, END_SEQWORD };
        chk_idx[0]=5; chk_exp[0]=A; chk_idx[1]=6; chk_exp[1]=B; nchk=2;
        break;
    case 4: /* STORE then LOAD same address in one triad — intra-triad forwarding */
        desc = "STORE+LOAD same addr { STAD RDI->[5], LDZX RSI<-[5] }";
        patch[n++] = (ucode_t){ LDT(RDI,0), NOP, NOP, NOP_SEQWORD };          /* RDI=A */
        patch[n++] = (ucode_t){ STT(RDI,5), LDT(RSI,5), NOP, NOP_SEQWORD };   /* DANGEROUS */
        patch[n++] = (ucode_t){ STT(RSI,6), NOP, NOP, END_SEQWORD };
        /* buf[5]==A: store happened. buf[6]==A: load saw the forwarded store
         * (sequential+forwarding). buf[6]==0: load ran before store (no fwd). */
        chk_idx[0]=5; chk_exp[0]=A; chk_idx[1]=6; chk_exp[1]=A; nchk=2;
        break;
    default:
        printf("unknown test %d (use 1..4)\n", t);
        return 1;
    }

    patch_ucode(0x7c00, patch, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);

    memset(g_buf, 0, sizeof(g_buf));
    g_buf[0]=A; g_buf[1]=B; g_buf[2]=C; g_buf[3]=D;

    /* THE DANGEROUS LINE — persist intent before firing. */
    logline("ATTEMPT test=%d  %s  (%d triads)\n", t, desc, n);

    fire();

    /* If we reach here, no hang. Report each checked output. */
    int ok = 1;
    char detail[256] = "";
    for (int i = 0; i < nchk; i++) {
        int idx = chk_idx[i];
        if (g_buf[idx] != chk_exp[i]) ok = 0;
        char s[80];
        snprintf(s, sizeof(s), " buf[%d]=%016" PRIx64 "(exp %016" PRIx64 ")",
                 idx, g_buf[idx], chk_exp[i]);
        strncat(detail, s, sizeof(detail)-strlen(detail)-1);
    }
    logline("RESULT  test=%d  %s%s\n", t, ok ? "PASS (two mem ops/triad WORK)"
                                              : "FAIL (data wrong)", detail);

    /* extra nuance for test 4: distinguish forwarding from no-forwarding */
    if (t == 4 && g_buf[5] == A && g_buf[6] == 0)
        logline("  note test4: store landed but load did NOT see it in-triad (no forwarding)\n");

    /* restore table so the next run / shell isn't left hooked */
    init_match_and_patch(); do_fix_IN_patch();
    logline("=== probe_memops done: test=%d ===\n\n", t);
    return ok ? 0 : 2;
}
