/*
 * mul_dst_class.c — is MUL_DSZ64_DRR's high-half destination a free operand?
 *
 * Gating probe for a v4 of the saturated 4x64 fe_mul.
 *
 * MUL_DSZ64_DRR(dst, srcA, srcB): dst = high half, srcB = low half, srcA
 * preserved. Every production patch writes `dst` = RCX and then copies the high
 * half where it is actually wanted:
 *
 *     { MUL_DSZ64_DRR(RCX, TMP10, RDX), ZEROEXT(TMP0, RDX), ZEROEXT(TMP1, RCX) }
 *                                                            ^^^^ dead move if
 *                                                            dst could be TMP1
 *
 * A TMP destination is already proven for the IMMEDIATE form: the shipped,
 * RFC-7748-verified 5x51 squaring patch contains MUL_DSZ64_DIR(TMP6, 19, TMP0)
 * (asm_op_curve25519.c:235, asm_op_curve25519_mul.c:377). The register-register
 * form with a TMP destination appears only in simple/probe_mul_critpath.c, a
 * latency probe that never checked the value it produced. Before rescheduling
 * the 4x64 patch around it, verify the arithmetic.
 *
 * For each destination class we compute a*b and return both halves, checked
 * against __uint128_t:
 *     D1  dst = RCX   (the form every production patch uses — control)
 *     D2  dst = TMP1  (what v4 needs)
 *     D3  dst = R14   (an architectural register that is not RCX)
 * srcA must be preserved in every case; that is checked too, since the v4
 * schedule keeps b[j] in srcA across all four rows.
 *
 * Build: make PROG=mul_dst_class
 * Run:   sudo taskset -c 0 ./mul_dst_class_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *what, uint64_t got, uint64_t want) {
    int ok = (got == want);
    if (ok) g_pass++; else g_fail++;
    printf("    [%s] %-44s got=0x%016" PRIx64 " want=0x%016" PRIx64 "\n",
           ok ? "PASS" : "FAIL", what, got, want);
}

/* buf[0]=R8 (a), buf[1]=R9 (b); out buf[7]=RAX (hi), buf[8]=RBX (lo),
 * buf[9]=RDX (srcA preservation witness). */
static void fire(uint64_t a, uint64_t b,
                 uint64_t *hi, uint64_t *lo, uint64_t *srca) {
    uint64_t buf[10];
    buf[0] = a; buf[1] = b; buf[6] = 0x2ULL;
    asm volatile(
        "mov  r8,  qword ptr [%[bp] + 0]\n\t"
        "mov  r9,  qword ptr [%[bp] + 8]\n\t"
        "xor  rax, rax\n\t"
        "xor  rbx, rbx\n\t"
        "xor  rcx, rcx\n\t"
        "xor  rdx, rdx\n\t"
        "push qword ptr [%[bp] + 48]\n\t"
        "popfq\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov  qword ptr [%[bp] + 56], rax\n\t"
        "mov  qword ptr [%[bp] + 64], rbx\n\t"
        "mov  qword ptr [%[bp] + 72], rdx\n\t"
        :
        : [bp] "r"(buf)
        : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r14", "cc", "memory"
    );
    if (hi)   *hi   = buf[7];
    if (lo)   *lo   = buf[8];
    if (srca) *srca = buf[9];
}

static void install(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Each variant: TMP0 = a (srcA, must survive), TMP2 = b (srcB, becomes lo).
 * Results out: RAX = hi, RBX = lo, RDX = srcA after the MUL. */

/* D1: dst = RCX — the production form. */
static void install_d1(void) {
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP0, R8), ZEROEXT_DSZ64_DR(TMP2, R9),
          NOP, NOP_SEQWORD },
        { MUL_DSZ64_DRR(RCX, TMP0, TMP2), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, RCX), ZEROEXT_DSZ64_DR(RBX, TMP2),
          ZEROEXT_DSZ64_DR(RDX, TMP0), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* D2: dst = TMP1 — what the v4 schedule needs. */
static void install_d2(void) {
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP0, R8), ZEROEXT_DSZ64_DR(TMP2, R9),
          NOP, NOP_SEQWORD },
        { MUL_DSZ64_DRR(TMP1, TMP0, TMP2), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, TMP2),
          ZEROEXT_DSZ64_DR(RDX, TMP0), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* D3: dst = R14 — an architectural register other than RCX. */
static void install_d3(void) {
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP0, R8), ZEROEXT_DSZ64_DR(TMP2, R9),
          NOP, NOP_SEQWORD },
        { MUL_DSZ64_DRR(R14, TMP0, TMP2), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, R14), ZEROEXT_DSZ64_DR(RBX, TMP2),
          ZEROEXT_DSZ64_DR(RDX, TMP0), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void variant(const char *name, void (*inst)(void)) {
    printf("  %s\n", name);
    inst();

    struct { uint64_t a, b; const char *label; } v[] = {
        { 0, 0,                                             "0 * 0" },
        { 1, 1,                                             "1 * 1" },
        { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,     "max * max" },
        { 0xFFFFFFFFFFFFFFFFULL, 2,                         "max * 2" },
        { 0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL,     "mixed" },
        { 38, 0xFFFFFFFFFFFFFFFFULL,                        "38 * max" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        __uint128_t want = (__uint128_t)v[i].a * v[i].b;
        uint64_t hi, lo, srca;
        fire(v[i].a, v[i].b, &hi, &lo, &srca);
        char w[80];
        snprintf(w, sizeof w, "%s  hi", v[i].label);
        check(w, hi, (uint64_t)(want >> 64));
        snprintf(w, sizeof w, "%s  lo", v[i].label);
        check(w, lo, (uint64_t)want);
        snprintf(w, sizeof w, "%s  srcA preserved", v[i].label);
        check(w, srca, v[i].a);
    }

    uint64_t seed = 0x5AB121A5EEDULL, bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t a = splitmix64(&seed), b = splitmix64(&seed);
        __uint128_t want = (__uint128_t)a * b;
        uint64_t hi, lo, srca;
        fire(a, b, &hi, &lo, &srca);
        if (hi != (uint64_t)(want >> 64) || lo != (uint64_t)want || srca != a) bad++;
    }
    check("2000 random products (0 = all correct)", bad, 0);
}

int main(void) {
    printf("================================================================\n");
    printf("  mul_dst_class.c — MUL_DSZ64_DRR high-half destination classes\n");
    printf("  Goldmont (Celeron N3350), vmwrite (0x0cd8) hook -> U7c00\n");
    printf("================================================================\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    variant("D1  dst = RCX   (production form, control)", install_d1);
    variant("D2  dst = TMP1  (what a v4 schedule needs)", install_d2);
    variant("D3  dst = R14   (arch register other than RCX)", install_d3);

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n================================================================\n");
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("  MUL's high-half destination is a free operand in all three\n"
               "  classes — the v3 ZEROEXT-after-MUL moves are removable.\n");
    else
        printf("  *** destination class matters — see the failing rows ***\n");
    printf("================================================================\n");
    return g_fail ? 1 : 0;
}
