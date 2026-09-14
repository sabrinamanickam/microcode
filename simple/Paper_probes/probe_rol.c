/*
 * probe_rol.c — Verify ROL_DSZ64 works on this Goldmont before committing the
 * Keccak generator to it. Keccak needs 24 rotates/round; if ROL is a no-op or
 * wrong, we'd have to compose SHL+SHR+OR (3x the ops) and the budget collapses.
 *
 * Safe: pure ALU, no memory ops, single-triad patches. vmwrite hook.
 *
 * Build: make PROG=probe_rol
 * Run:   sudo taskset -c 0 ./probe_rol_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static uint64_t rol64(uint64_t v, int n) {
    n &= 63;
    return n ? ((v << n) | (v >> (64 - n))) : v;
}

/* Fire vmwrite with RCX=input; patch computes RAX=ROL(RCX,imm); return RAX. */
static uint64_t fire(uint64_t in) {
    uint64_t out;
    register uint64_t _in asm("rcx") = in;
    asm volatile(
        "vmwrite rcx, rcx\n\t"
        "mov %[o], rax\n\t"
        : [o] "=r"(out)
        : "r"(_in)
        : "rax", "rdx", "memory", "cc"
    );
    return out;
}

static int test_rol(int amt, uint64_t in) {
    ucode_t patch[1] = {{
        ROL_DSZ64_DRI(RAX, RCX, amt),
        NOP, NOP, END_SEQWORD
    }};
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, patch, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);

    uint64_t got = fire(in);
    uint64_t exp = rol64(in, amt);
    int ok = (got == exp);
    printf("  ROL(%016" PRIx64 ", %2d) = %016" PRIx64 "  expect %016" PRIx64 "  %s\n",
           in, amt, got, exp, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void) {
    printf("=== probe_rol: verify ROL_DSZ64_DRI on this box ===\n\n");
    assign_to_core(0);

    uint64_t v = 0x0123456789ABCDEFULL;
    /* The actual Keccak rho rotation amounts, plus a couple of edge cases. */
    int amts[] = { 1, 3, 6, 10, 14, 15, 18, 20, 21, 25, 27, 28,
                   36, 39, 41, 43, 44, 45, 55, 56, 61, 62, 0, 63 };
    int n = sizeof(amts)/sizeof(amts[0]);
    int pass = 0;
    for (int i = 0; i < n; i++) pass += test_rol(amts[i], v);

    printf("\n%d/%d ROL amounts correct.\n", pass, n);
    printf(pass == n ? "ROL works — generator can use it.\n"
                     : "ROL BROKEN — must compose SHL+SHR+OR instead.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return pass == n ? 0 : 1;
}
