#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ---------- Patch A: MUL_DSZ32 with immediate (opcode sanity) ----------
 * EAX = EAX * 0x10
 * If this prints 0x12340 (for A=0x1234), your MUL32 opcode + INSTR_DRI are good.
 */
static void install_mul32_imm_patch(void) {
    ucode_t p[] = {{
        /* stage A from a quiet reg first so XLAT can’t disturb it */
        MOVE_DSZ32_RR(TMP1, R8),          /* TMP1 = A (from R8D) */
        MUL_DSZ32_DRI(RAX, TMP1, 0x10),   /* EAX = TMP1 * 0x10 (low32) */
        END_SEQWORD
    }};
    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(0x7c4c, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c4c);   /* vmwrite r64,r64 */
}

/* ---------- Patch B: MUL_DSZ32 reg×reg (quiet regs) ----------
 * EAX = R8D * R9D
 * If A works but B doesn’t, DRR operand mapping or src regs are the issue.
 */
static void install_mul32_reg_patch(void) {
    ucode_t p[] = {{
        /* latch args ASAP from quiet regs */
        MOVE_DSZ32_RR(TMP1, R8),          /* TMP1 = A (R8D) */
        MOVE_DSZ32_RR(TMP2, R9),          /* TMP2 = B (R9D) */
        MUL_DSZ32_DRR(RAX, TMP1, TMP2),   /* EAX = TMP1 * TMP2 (low32) */
        END_SEQWORD
    }};
    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(0x7c44, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c44);
}

static uint64_t run_once(void (*install)(void), uint32_t a, uint32_t b) {
    uint64_t out;
    install();
    asm volatile(
        "mov %2, %%r8d\n\t"          /* A in quiet reg */
        "mov %3, %%r9d\n\t"          /* B in quiet reg */
        "vmwrite %%rax, %%rcx\n\t"   /* just a trigger; rcx ignored */
        : "=a"(out)
        : "0"(0ULL), "r"(a), "r"(b)
        : "r8", "r9", "rcx", "cc", "memory");
    return out;
}

int main(void) {
    uint32_t a = 0x1234u, b = 0x10u;

    printf("== Patch A: MUL32 immediate (EAX *= 0x10) ==\n");
    uint64_t rA = run_once(install_mul3_imm_patch, a, b);
    printf("RAX = 0x%016lx  expected 0x%016lx\n",
           rA, (unsigned long)((uint64_t)a * 0x10u));

   /* printf("== Patch B: MUL32 reg×reg (EAX = R8D * R9D) ==\n");
    uint64_t rB = run_once(install_mul32_reg_patch, a, b);
    printf("RAX = 0x%016lx  expected 0x%016lx\n",
           rB, (unsigned long)((uint64_t)a * (uint64_t)b));*/

    return 0;
}

