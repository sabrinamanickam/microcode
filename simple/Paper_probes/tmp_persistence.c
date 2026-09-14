/*
 * tmp_persistence.c — RECHECK of the C1 claim:
 *
 *   "Temporary registers and their associated state cease to be available once
 *    execution returns to the architectural interface."
 *   (methodology.tex: "no temporary register survives a firing";
 *    CLAUDE.md: "TMP registers don't persist across vmwrite calls".)
 *
 * PROVENANCE OF THE CLAIM.  It is asserted in CLAUDE.md, SUMMARY.md,
 * microcode_findings.md §3, sq_pair_design.md and two paper sections, but no
 * probe in Paper_probes establishes it.  The only code that ever tested it is
 * test_two_patch.c "Test 4" (one TMP, one A->B pair, no recorded verdict).
 * SUMMARY.md states this rule two lines away from "MUL must be in slot 0" and
 * "SETCC only works on TMP registers", both of which this project has since
 * refuted (test_mul_slot2.c, carrystate_scope.c group B).  Same provenance.
 *
 * WHAT IS ACTUALLY BEING CLAIMED.  Two separable things:
 *   (V) the VALUE in a temporary register is gone at the next firing;
 *   (F) the per-destination CONDITION STATE attached to it is gone.
 * And a third, which is what a paper sentence with "cease to be available"
 * asserts but which neither (V) nor (F) alone establishes:
 *   (D) the loss is a property of the DESCENT — of returning to the
 *       architectural interface — rather than of whatever code happens to run
 *       between two firings.
 * If TMPs survive a back-to-back firing pair and are destroyed only by
 * intervening microcoded work, then (V) and (F) can still hold in practice
 * while (D) is false, and the paper's wording is wrong about the mechanism.
 *
 * SECTIONS
 *   1  Literal form of the claim: ONE patch, ONE hook, three consecutive
 *      vmwrites with NOTHING between them.  The patch dumps all 16 TMPs and
 *      then refills them with seed+i.  Firing 2's dump is firing 1's TMP file.
 *   2  Two hooks (A = vmwrite writes TMPs, B = vmread dumps them) with a
 *      controlled filler between.  Sweeps 9 fillers from nothing up to a
 *      context switch, so if survival is conditional we learn on what.
 *   3  Condition state (F): A latches a carry on TMP0, B reads it with SETCC.
 *      Control fires the same ADD+SETCC inside one firing.
 *
 * The seed is a fresh random 64 bits per run and each TMP gets seed+i, so a
 * "survived" verdict cannot be a coincidence with a stale ROM constant, and it
 * identifies WHICH temporary register survived.
 *
 * Safety: same model as probe_memops.c.  A patch that writes 16 TMPs and stores
 * 16 words is nothing production Keccak does not already do every firing, so
 * the risk here is ordinary.  Each section still fsyncs an ATTEMPT line before
 * firing, so a hang names its own section after a reboot.
 *
 * Build: make PROG=tmp_persistence
 * Run:   sudo taskset -c 0 ./tmp_persistence_static
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
#include <sched.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

#define SEG 0x18                      /* SEG_DS — probe_seg */
#define LOGPATH "tmp_persistence.log"

/* -no-pie keeps this below 4GB, required by the ASZ32 address size. */
static uint64_t g_buf[64];

static const uint64_t TMPREG[16] = {
    TMP0, TMP1, TMP2,  TMP3,  TMP4,  TMP5,  TMP6,  TMP7,
    TMP8, TMP9, TMP10, TMP11, TMP12, TMP13, TMP14, TMP15
};

/* ─────────────────────────── logging ─────────────────────────── */
static void logline(const char *fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    b[n++] = '\n';
    int fd = open(LOGPATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { ssize_t w = write(fd, b, n); (void)w; fsync(fd); close(fd); }
}

/* ─────────────────────────── install ─────────────────────────── */
static void install1(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install2(ucode_t *a, int na, ucode_t *b, int nb) {
    uint64_t addr_a = 0x7c00, addr_b = 0x7c00 + (uint64_t)na * 4;
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(addr_a, a, na);
    hook_match_and_patch(0, 0x0cd8, addr_a);   /* vmwrite */
    patch_ucode(addr_b, b, nb);
    hook_match_and_patch(1, 0x0618, addr_b);   /* vmread  */
}

/* ─────────────────────── reporting helper ─────────────────────── */
/* Returns the number of temporaries whose value equals seed+i. */
static int report_block(const char *what, const uint64_t *blk, uint64_t seed) {
    int n = 0;
    char mask[17];
    for (int i = 0; i < 16; i++) {
        int ok = (blk[i] == seed + (uint64_t)i);
        mask[i] = ok ? '#' : '.';
        n += ok;
    }
    mask[16] = 0;
    printf("    %-34s  %s  %2d/16 survived\n", what, mask, n);
    return n;
}

static void dump_block(const uint64_t *blk) {
    for (int i = 0; i < 16; i += 4)
        printf("      TMP%-2d..%-2d  %016" PRIx64 " %016" PRIx64
               " %016" PRIx64 " %016" PRIx64 "\n",
               i, i + 3, blk[i], blk[i+1], blk[i+2], blk[i+3]);
}

/* ════════════════════════════════════════════════════════════════════
 * SECTION 1 — the literal claim, one hook, no instruction between firings.
 *
 * Patch: dump TMP0..15 to [R14 + 8i], refill TMP_i = R13 + i, then R14 += 128.
 * Dumping BEFORE refilling means firing k's dump block is the TMP file as
 * firing k-1 left it, and the R14 bump lets consecutive firings write to
 * consecutive blocks with zero architectural instructions in between.
 * ════════════════════════════════════════════════════════════════════ */
static int section1(uint64_t seed) {
    ucode_t p[16 + 6 + 1];
    int t = 0;
    for (int i = 0; i < 16; i++) {
        p[t].uop0 = STAD_DSZ64_ASZ32_SC1_RRI(TMPREG[i], R14, i * 8, SEG);
        p[t].uop1 = NOP; p[t].uop2 = NOP; p[t].seqw = NOP_SEQWORD; t++;
    }
    for (int i = 0; i < 16; i += 3) {
        p[t].uop0 = ADD_DSZ64_DRI(TMPREG[i], R13, i);
        p[t].uop1 = (i + 1 < 16) ? ADD_DSZ64_DRI(TMPREG[i+1], R13, i + 1) : NOP;
        p[t].uop2 = (i + 2 < 16) ? ADD_DSZ64_DRI(TMPREG[i+2], R13, i + 2) : NOP;
        p[t].seqw = NOP_SEQWORD; t++;
    }
    p[t].uop0 = ADD_DSZ64_DRI(R14, R14, 128);
    p[t].uop1 = NOP; p[t].uop2 = NOP; p[t].seqw = END_SEQWORD; t++;

    printf("\n=== SECTION 1 — three consecutive firings, NOTHING between ===\n");
    printf("  patch = dump 16 TMPs, refill TMP_i = seed+i, bump base.  %d triads\n", t);
    printf("  seed = 0x%016" PRIx64 "\n", seed);
    logline("ATTEMPT section1 seed=%016" PRIx64, seed);

    memset(g_buf, 0, sizeof g_buf);
    install1(p, t);

    asm volatile(
        "mov r13, %[s]\n\t"
        "mov r14, %[b]\n\t"
        "vmwrite rcx, rdx\n\t"
        "vmwrite rcx, rdx\n\t"
        "vmwrite rcx, rdx\n\t"
        : : [s] "r"(seed), [b] "r"((uint64_t)(uintptr_t)g_buf)
        : "r13", "r14", "rcx", "rdx", "memory", "cc");

    logline("RESULT  section1 ok");

    printf("  block 0 = TMP file on entry to firing 1 (i.e. what the install\n"
           "            helpers left behind).  Not expected to match the seed.\n");
    report_block("block 0  (before firing 1)", &g_buf[0], seed);
    dump_block(&g_buf[0]);
    printf("  block 1 = TMP file as firing 1 left it, read by firing 2.\n"
           "            THIS is the claim.  0/16 = claim holds.\n");
    int s1 = report_block("block 1  (firing 1 -> firing 2)", &g_buf[16], seed);
    dump_block(&g_buf[16]);
    printf("  block 2 = same, one firing later.\n");
    int s2 = report_block("block 2  (firing 2 -> firing 3)", &g_buf[32], seed);
    dump_block(&g_buf[32]);

    printf("  VERDICT: ");
    if (s1 == 0 && s2 == 0)
        printf("claim HOLDS in its literal form — no temporary survived.\n");
    else if (s1 == 16 && s2 == 16)
        printf("claim REFUTED — every temporary survived a return to the\n"
               "           architectural interface and a re-entry.\n");
    else
        printf("claim PARTIAL — %d then %d of 16 survived; see the masks.\n", s1, s2);
    return s1;
}

/* ════════════════════════════════════════════════════════════════════
 * SECTION 2 — two hooks, with a filler between A and B.
 *
 * A (vmwrite) writes TMP_i = R13 + i and returns.  B (vmread) dumps the 16
 * temporaries to [R14 + 8i].  R13/R14 are set once before A, and every filler
 * that would clobber them is written not to.
 * ════════════════════════════════════════════════════════════════════ */
static ucode_t g_a[6], g_b[16];
static int g_na, g_nb;

static void build_ab(void) {
    g_na = 0;
    for (int i = 0; i < 16; i += 3) {
        g_a[g_na].uop0 = ADD_DSZ64_DRI(TMPREG[i], R13, i);
        g_a[g_na].uop1 = (i + 1 < 16) ? ADD_DSZ64_DRI(TMPREG[i+1], R13, i + 1) : NOP;
        g_a[g_na].uop2 = (i + 2 < 16) ? ADD_DSZ64_DRI(TMPREG[i+2], R13, i + 2) : NOP;
        g_a[g_na].seqw = NOP_SEQWORD; g_na++;
    }
    g_a[g_na - 1].seqw = END_SEQWORD;

    g_nb = 0;
    for (int i = 0; i < 16; i++) {
        g_b[g_nb].uop0 = STAD_DSZ64_ASZ32_SC1_RRI(TMPREG[i], R14, i * 8, SEG);
        g_b[g_nb].uop1 = NOP; g_b[g_nb].uop2 = NOP;
        g_b[g_nb].seqw = NOP_SEQWORD; g_nb++;
    }
    g_b[g_nb - 1].seqw = END_SEQWORD;
}

#define AB(name, FILLER, ...)                                              \
static void ab_##name(uint64_t seed, uint64_t *buf) {                      \
    asm volatile(                                                          \
        "mov r13, %[s]\n\t"                                                \
        "mov r14, %[b]\n\t"                                                \
        "vmwrite rcx, rdx\n\t"                                             \
        FILLER                                                             \
        ".byte 0x0f, 0x78, 0xca\n\t"                                       \
        : : [s] "r"(seed), [b] "r"((uint64_t)(uintptr_t)buf)               \
        : "r13", "r14", "rcx", "rdx", "memory", "cc", ##__VA_ARGS__);      \
}

AB(none,   "")
AB(nop8,   "nop\n\t nop\n\t nop\n\t nop\n\t nop\n\t nop\n\t nop\n\t nop\n\t")
AB(alu,    "xor eax, eax\n\t add eax, 1\n\t imul eax, eax, 3\n\t"
           "shl eax, 2\n\t xor eax, eax\n\t", "rax")
AB(mem,    "mov qword ptr [rsp-32], rax\n\t mov rax, qword ptr [rsp-32]\n\t"
           "mov qword ptr [rsp-40], rax\n\t", "rax")
AB(rdtsc,  "rdtsc\n\t", "rax", "rdx")
AB(pushf,  "pushfq\n\t popfq\n\t")
AB(lockop, "mov eax, 1\n\t lock xadd dword ptr [rsp-48], eax\n\t", "rax")
AB(divq,   "xor edx, edx\n\t mov eax, 100\n\t mov ecx, 7\n\t div ecx\n\t",
           "rax", "rcx", "rdx")
AB(cpuid,  "xor eax, eax\n\t cpuid\n\t", "rax", "rbx", "rcx", "rdx")

static void fire_a(uint64_t seed) {
    asm volatile("mov r13, %[s]\n\t vmwrite rcx, rdx\n\t"
                 : : [s] "r"(seed) : "r13", "rcx", "rdx", "memory", "cc");
}
static void fire_b(uint64_t *buf) {
    asm volatile("mov r14, %[b]\n\t .byte 0x0f, 0x78, 0xca\n\t"
                 : : [b] "r"((uint64_t)(uintptr_t)buf)
                 : "r14", "rcx", "rdx", "memory", "cc");
}

static void section2(uint64_t seed) {
    struct { const char *name; void (*fn)(uint64_t, uint64_t *); const char *note; } tight[] = {
        { "nothing at all",        ab_none,   "A and B are adjacent instructions" },
        { "8 x nop",               ab_nop8,   "front-end work, no execution" },
        { "5 ALU ops",             ab_alu,    "the shape of a wrapper's address math" },
        { "3 memory ops",          ab_mem,    "the shape of a wrapper's operand traffic" },
        { "pushfq / popfq",        ab_pushf,  "touches the architectural flags" },
        { "lock xadd",             ab_lockop, "atomic, serialising" },
        { "rdtsc",                 ab_rdtsc,  "microcoded" },
        { "div r32",               ab_divq,   "microcoded, multi-uop" },
        { "cpuid",                 ab_cpuid,  "heavily microcoded, serialising" },
    };

    printf("\n=== SECTION 2 — A writes the temporaries, B reads them back ===\n");
    printf("  A on vmwrite (%d triads), B on vmread (%d triads).\n", g_na, g_nb);
    printf("  A '#' means TMP_i still held seed+i when B ran.\n\n");
    logline("ATTEMPT section2 seed=%016" PRIx64, seed);

    for (unsigned k = 0; k < sizeof tight / sizeof tight[0]; k++) {
        memset(g_buf, 0, sizeof g_buf);
        install2(g_a, g_na, g_b, g_nb);
        tight[k].fn(seed, g_buf);
        char what[64];
        snprintf(what, sizeof what, "%-18s", tight[k].name);
        int n = report_block(what, g_buf, seed);
        printf("        %s%s\n", tight[k].note, n == 0 ? "  [all lost]" : "");
    }

    /* Fillers that cannot be expressed as a string of instructions.  These run
     * through the compiler, so a handful of moves surround them; that is the
     * point — they model a real gap between two firings. */
    printf("\n  Fillers that go through C (compiler-generated code included):\n");
    struct { const char *name; int kind; } wide[] = {
        { "a call to an empty fn", 0 },
        { "getpid()  (syscall)",   1 },
        { "sched_yield()",         2 },
        { "nanosleep 1 ms",        3 },
        { "printf to /dev/null",   4 },
    };
    for (unsigned k = 0; k < sizeof wide / sizeof wide[0]; k++) {
        memset(g_buf, 0, sizeof g_buf);
        install2(g_a, g_na, g_b, g_nb);
        fire_a(seed);
        switch (wide[k].kind) {
        case 0: { extern void tmp_empty_fn(void); tmp_empty_fn(); break; }
        case 1: (void)getpid(); break;
        case 2: sched_yield(); break;
        case 3: { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); break; }
        case 4: { FILE *f = fopen("/dev/null", "w");
                  if (f) { fprintf(f, "x"); fclose(f); } break; }
        }
        fire_b(g_buf);
        char what[64];
        snprintf(what, sizeof what, "%-18s", wide[k].name);
        report_block(what, g_buf, seed);
    }
}

__attribute__((noinline)) void tmp_empty_fn(void) { asm volatile("" ::: "memory"); }

/* ════════════════════════════════════════════════════════════════════
 * SECTION 3 — the condition state, not the value.
 *
 * A latches a carry on TMP0 (ADD R13+R12) and parks a witness in TMP2.
 * B asks for that carry with SETCC and stores what it got.  The two operand
 * pairs are chosen so the SUM cannot stand in for the carry:
 *     carry, sum 1   (0xFF..FE + 3)
 *     no carry, sum 2  (1 + 1)
 * so a SETCC result that tracks the carry can only have come from state that
 * outlived the firing.  The control runs ADD and SETCC in the same firing.
 * ════════════════════════════════════════════════════════════════════ */
static void section3(void) {
    /* A: latch a carry on TMP0, park a value witness in TMP2, return. */
    ucode_t a3[] = {
        { ADD_DSZ64_DRR(TMP0, R13, R12), ZEROEXT_DSZ64_DR(TMP2, R11),
          NOP, END_SEQWORD },
    };
    /* B: ask TMP0 for that carry, then store the answer and both witnesses. */
    ucode_t b3[] = {
        { SETCC_CONDB_DR(TMP1, TMP0), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(TMP1, R14, 0,  SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(TMP0, R14, 8,  SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(TMP2, R14, 16, SEG), NOP, NOP, END_SEQWORD },
    };
    /* Control: the same question asked entirely inside ONE firing, stores
     * included.  It must not share B, because B's own SETCC would overwrite
     * the result it is meant to be checking, and because a control that
     * depended on a temporary surviving would be assuming the answer. */
    ucode_t c3[] = {
        { ADD_DSZ64_DRR(TMP0, R13, R12), SETCC_CONDB_DR(TMP1, TMP0),
          ZEROEXT_DSZ64_DR(TMP2, R11), NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(TMP1, R14, 0,  SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(TMP0, R14, 8,  SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(TMP2, R14, 16, SEG), NOP, NOP, END_SEQWORD },
    };

    const uint64_t CA = 0xFFFFFFFFFFFFFFFEULL, CB = 3;   /* carry,    sum 1 */
    const uint64_t NA = 1, NB = 1;                        /* no carry, sum 2 */
    const uint64_t WIT = 0x5A5A5A5AULL;
    uint64_t setcc[2][2];                                 /* [ctl][carry] */

    printf("\n=== SECTION 3 - does the per-register condition state survive? ===\n");
    printf("  operands are chosen so the SUM cannot stand in for the carry:\n");
    printf("    carry in = 1 : 0xFF..FE + 3 -> carry, sum 1\n");
    printf("    carry in = 0 : 1 + 1        -> no carry, sum 2\n");
    logline("ATTEMPT section3");

    for (int ctl = 1; ctl >= 0; ctl--) {
        printf("  %s\n", ctl ? "control: ADD, SETCC and the stores in ONE firing"
                             : "test:    ADD in firing A, SETCC in firing B");
        for (int carry = 1; carry >= 0; carry--) {
            uint64_t a = carry ? CA : NA, b = carry ? CB : NB;
            memset(g_buf, 0, sizeof g_buf);
            if (ctl) {
                install1(c3, 4);
                asm volatile(
                    "mov r13, %[a]\n\t" "mov r12, %[b]\n\t"
                    "mov r11, %[w]\n\t" "mov r14, %[p]\n\t"
                    "vmwrite rcx, rdx\n\t"
                    : : [a] "r"(a), [b] "r"(b), [w] "r"(WIT),
                        [p] "r"((uint64_t)(uintptr_t)g_buf)
                    : "r11", "r12", "r13", "r14", "rcx", "rdx", "memory", "cc");
            } else {
                install2(a3, 1, b3, 4);
                asm volatile(
                    "mov r13, %[a]\n\t" "mov r12, %[b]\n\t"
                    "mov r11, %[w]\n\t" "mov r14, %[p]\n\t"
                    "vmwrite rcx, rdx\n\t"
                    ".byte 0x0f, 0x78, 0xca\n\t"
                    : : [a] "r"(a), [b] "r"(b), [w] "r"(WIT),
                        [p] "r"((uint64_t)(uintptr_t)g_buf)
                    : "r11", "r12", "r13", "r14", "rcx", "rdx", "memory", "cc");
            }
            setcc[ctl][carry] = g_buf[0];
            printf("    carry in = %d :  SETCC -> %" PRIu64
                   "   TMP0 (sum) = %" PRIu64
                   "   TMP2 (witness) = 0x%" PRIx64 "%s\n",
                   carry, g_buf[0], g_buf[1], g_buf[2],
                   (!ctl && g_buf[2] != WIT) ? "   <- value witness LOST" : "");
        }
    }
    logline("RESULT  section3 ok");

    int ctl_ok   = (setcc[1][1] == 1 && setcc[1][0] == 0);
    int test_ok  = (setcc[0][1] == 1 && setcc[0][0] == 0);
    printf("  control discriminates: %s\n", ctl_ok ? "yes" : "NO - section 3 is void");
    if (ctl_ok)
        printf("  VERDICT: condition state %s a return to the architectural interface.\n",
               test_ok ? "SURVIVES" :
               (setcc[0][1] == setcc[0][0] ? "does NOT survive" :
                "gave an inverted or unstable answer - read the rows"));
}

/* ════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("================================================================\n");
    printf("  tmp_persistence.c — recheck of \"no temporary register survives\n");
    printf("  a firing\".  Goldmont (Celeron N3350), vmwrite 0x0cd8 -> U7c00,\n");
    printf("  vmread 0x0618 -> the patch after it.\n");
    printf("================================================================\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    uint64_t seed;
    { FILE *f = fopen("/dev/urandom", "rb");
      if (!f || fread(&seed, 1, 8, f) != 8) seed = 0x0123456789ABCDEFULL;
      if (f) fclose(f); }
    seed &= ~0xFFULL;              /* low byte clear so seed+i is unambiguous */
    seed |= 0x0100000000000000ULL; /* and clearly not a small stale constant  */

    section1(seed);
    build_ab();
    section2(seed);
    section3();

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n================================================================\n");
    printf("  done.  seed was 0x%016" PRIx64 "\n", seed);
    printf("================================================================\n");
    return 0;
}
