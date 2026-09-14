/*
 * intra-triad-adc.c — Does an ADD in slot 0 give ADC in slot 1 its CF
 * within the SAME triad?
 *
 * Patch (one real triad):
 *   T0: slot 0 = ADD TMP0 = R64DST + R64SRC      (sets some flags)
 *       slot 1 = ADC R64DST = R64SRC + R64SRC + arch_CF
 *       slot 2 = NOP
 *   T1: END
 *
 * The wrapper runs `vmwrite rbx, rcx`, so R64DST ↔ rbx and
 * R64SRC ↔ rcx. The slot-1 ADC overwrites RBX with
 * `RCX + RCX + arch_CF`. Reading back RBX tells us what CF the
 * slot-1 ADC saw.
 *
 * Two probes:
 *
 *   Probe A — slot 0 ADD OVERFLOWS (RBX=0xFFFF…FFFF, RCX=1).
 *     arch CF on entry = 0.
 *     If intra-triad propagation works, slot 1 sees CF=1.
 *       RBX_out = 1 + 1 + 1 = 3
 *     If not, slot 1 reads entry CF=0:
 *       RBX_out = 1 + 1 + 0 = 2
 *
 *   Probe B — slot 0 ADD does NOT overflow (RBX=1, RCX=1).
 *     arch CF on entry = 1.
 *     If intra-triad propagation works, slot 1 sees the just-cleared CF=0:
 *       RBX_out = 1 + 1 + 0 = 2
 *     If slot 1 still reads frozen entry CF=1:
 *       RBX_out = 1 + 1 + 1 = 3
 *
 * Two probes that flip the answer in opposite directions, so a result
 * of (Probe A → 3, Probe B → 2) uniquely says "intra-triad works",
 * and (Probe A → 2, Probe B → 3) uniquely says "intra-triad does NOT
 * propagate; ADC reads entry CF only".
 *
 * Build: make PROG=intra-triad-adc
 * Run:   sudo taskset -c 0 ./intra-triad-adc_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static void install(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* ONE triad with ADD in slot 0, ADC in slot 1. */
        { ADC_DSZ64_DRR(TMP0, R64DST, R64SRC),
          ADC_DSZ64_DRR(R64DST, R64SRC, R64SRC),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Wrapper:
 *   load RBX = a, RCX = b
 *   popfq with CF = cf_in (LAST flag op before vmwrite)
 *   vmwrite rbx, rcx
 *   read RBX back */
static uint64_t fire(uint64_t a, uint64_t b, int cf_in) {
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
        : [a] "r"(a), [b] "r"(b), [flg] "r"(flags)
        : "rbx", "rcx", "cc", "memory"
    );
    return res;
}

int main(void) {
    printf("============================================================\n");
    printf("  Intra-triad ADC: does slot-0 ADD's carry reach slot-1 ADC?\n");
    printf("  Patch triad:\n");
    printf("    slot 0: ADD TMP0   = R64DST + R64SRC        (sets flags?)\n");
    printf("    slot 1: ADC R64DST = R64SRC + R64SRC + CF   (reads flags)\n");
    printf("    slot 2: NOP\n");
    printf("============================================================\n\n");

    assign_to_core(0);
    install();

    /* Probe A: slot-0 ADD overflows, entry CF=0.
     * Detector: RBX=3 ⇒ slot-1 ADC saw new CF=1 from slot-0.
     *           RBX=2 ⇒ slot-1 ADC saw entry CF=0. */
    {
        uint64_t got = fire(0xFFFFFFFFFFFFFFFFULL, 1, /*cf_in=*/0);
        printf("  Probe A  RBX=0xFFFF…FFFF  RCX=1  entry CF=0\n");
        printf("           slot-0 ADD: 0xFFFF…FFFF + 1 = 0  (CF=1 if it propagates)\n");
        printf("           slot-1 ADC: 1 + 1 + CF\n");
        printf("           RBX after = %" PRIu64 "\n", got);
        if      (got == 3) printf("           → 3 = 1+1+1   slot-0's carry REACHED slot-1 (intra-triad WORKS)\n");
        else if (got == 2) printf("           → 2 = 1+1+0   slot-1 saw entry CF=0 (intra-triad does NOT propagate)\n");
        else               printf("           → unexpected value\n");
        printf("\n");
    }

    /* Probe B: slot-0 ADD does NOT overflow, entry CF=1.
     * Detector: RBX=2 ⇒ slot-1 ADC saw new CF=0 from slot-0 (intra-triad works).
     *           RBX=3 ⇒ slot-1 ADC still reads frozen entry CF=1. */
    {
        uint64_t got = fire(1, 1, /*cf_in=*/1);
        printf("  Probe B  RBX=1  RCX=1  entry CF=1\n");
        printf("           slot-0 ADD: 1 + 1 = 2  (CF=0 if it propagates)\n");
        printf("           slot-1 ADC: 1 + 1 + CF\n");
        printf("           RBX after = %" PRIu64 "\n", got);
        if      (got == 2) printf("           → 2 = 1+1+0   slot-1 saw new CF=0 from slot-0 (intra-triad WORKS)\n");
        else if (got == 3) printf("           → 3 = 1+1+1   slot-1 still reads entry CF=1 (intra-triad does NOT propagate)\n");
        else               printf("           → unexpected value\n");
        printf("\n");
    }

    printf("============================================================\n");
    printf("  Interpretation:\n");
    printf("    Probe A = 3 AND Probe B = 2  →  intra-triad CF propagation works\n");
    printf("    Probe A = 2 AND Probe B = 3  →  ADC reads frozen entry CF only\n");
    printf("    Anything else                →  weirder semantics, look at the values\n");
    printf("============================================================\n");
    return 0;
}
