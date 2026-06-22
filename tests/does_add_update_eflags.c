/*
 * does_add_update_eflags.c — Smallest possible test: does a microcode
 * ADD write to any arch-flag register that READAFLAGS sees?
 *
 * Patch:
 *   T0: ADD TMP0 = R64DST + R64SRC        (overflow, internal CF=1)
 *   T1: READAFLAGS TMP1 = TMP1            (read arch flags into TMP1)
 *   T2: ZEROEXT RBX = TMP1                (return flags)
 *   T3: END
 *
 * Probe:
 *   RBX=0xFFFFFFFFFFFFFFFF, RCX=1, entry CF=0  (forced via popfq)
 *   slot-0 ADD does 0xFFFF…FFFF + 1, which overflows (TMP-CF=1).
 *
 * Compare two runs:
 *   (1) ADD-with-overflow,  entry CF=0
 *   (2) No-ADD (sanity),    entry CF=0
 *
 * If run 1's CF bit (RBX & 1) = 1 and run 2's = 0:
 *   → ADD updates a live arch flag visible to READAFLAGS.
 *   → That flag is in a *different* register from the one ADC reads
 *     (since ADC didn't see it).
 *
 * If both runs show CF=0:
 *   → ADD does NOT touch any arch flags. The TMP-domain (read by SETCC)
 *     is fully isolated. READAFLAGS reads either the dispatch snapshot
 *     or some unrelated source — and either way, no bridge exists.
 *
 * Build: make PROG=does_add_update_eflags
 * Run:   sudo taskset -c 0 ./does_add_update_eflags_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static void install_with_add(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R64DST, R64SRC), NOP, NOP, NOP_SEQWORD },
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_no_add(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t fire(uint64_t rbx, uint64_t rcx, int cf_in) {
    uint64_t res;
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "push %[flg]\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rbx\n\t"
        : [res] "=r"(res)
        : [a] "r"(rbx), [b] "r"(rcx), [flg] "r"(flags)
        : "rbx", "rcx", "cc", "memory"
    );
    return res;
}

static void show(const char *label, uint64_t flags, int entry_cf) {
    int cf = (int)(flags & 1);
    int zf = (int)((flags >> 6) & 1);
    int sf = (int)((flags >> 7) & 1);
    int of = (int)((flags >> 11) & 1);
    printf("  %-50s  entry CF=%d  flags=0x%016" PRIx64 "  CF=%d ZF=%d SF=%d OF=%d\n",
           label, entry_cf, flags, cf, zf, sf, of);
}

int main(void) {
    printf("============================================================\n");
    printf("  Does microcode ADD update any arch-flag register?\n");
    printf("============================================================\n\n");

    assign_to_core(0);

    /* Sanity baseline — no ADD; READAFLAGS reflects entry flags. */
    install_no_add();
    show("(sanity)  bare READAFLAGS, entry CF=0",
         fire(0xFFFFFFFFFFFFFFFFULL, 1, 0), 0);
    show("(sanity)  bare READAFLAGS, entry CF=1",
         fire(0xFFFFFFFFFFFFFFFFULL, 1, 1), 1);
    printf("\n");

    /* The actual probe — ADD overflows, then READAFLAGS. */
    install_with_add();
    show("ADD 0xFFFF…FFFF + 1 (overflow), entry CF=0",
         fire(0xFFFFFFFFFFFFFFFFULL, 1, 0), 0);
    show("ADD 0xFFFF…FFFF + 1 (overflow), entry CF=1",
         fire(0xFFFFFFFFFFFFFFFFULL, 1, 1), 1);
    /* Non-overflow control: ADD 1+1 = 2, CF=0. */
    show("ADD 1 + 1 (no overflow), entry CF=0",
         fire(1, 1, 0), 0);
    show("ADD 1 + 1 (no overflow), entry CF=1",
         fire(1, 1, 1), 1);
    printf("\n");

    printf("============================================================\n");
    printf("  Reading the data:\n");
    printf("    (a) If overflow rows show CF=1 while no-overflow rows show CF=0,\n");
    printf("        the ADD DID write to a live arch-flag register that\n");
    printf("        READAFLAGS reads — separate from ADC's port.\n");
    printf("    (b) If CF == entry CF in every row regardless of ADD's overflow,\n");
    printf("        ADD does NOT touch any arch flags. TMP-CF stays isolated.\n");
    printf("============================================================\n");
    return 0;
}
