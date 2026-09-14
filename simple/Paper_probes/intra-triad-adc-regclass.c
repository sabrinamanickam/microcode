/*
 * intra-triad-adc-regclass.c — Does slot-0 ADD's CF propagate to slot-1 ADC
 * depending on the DESTINATION register class of the ADD?
 *
 * Hypothesis: Maybe writing the ADD's result to an "arch-class" register
 * (via R64DST/R64SRC encoding, an explicit arch reg ID like RAX/RBX, or
 * the alternate TMP-class IDs MM/TMM/TMPV) routes its CF into the arch
 * flag domain, instead of the TMP-CF domain that SETCC reads.
 *
 * Patch (one real triad):
 *   T0: slot 0 = ADD {dst_variant} = R64DST + R64SRC     (overflow → CF=1)
 *       slot 1 = ADC R64DST = R64SRC + R64SRC + arch_CF
 *       slot 2 = NOP
 *   T1: END
 *
 * Wrapper:
 *   RBX = 0xFFFFFFFFFFFFFFFF (=R64DST)
 *   RCX = 1                  (=R64SRC)
 *   entry arch CF = 0
 *   vmwrite rbx, rcx
 *
 * Slot 0 ADD: 0xFFFF…FFFF + 1 = 0  (overflow → internal CF=1)
 * Slot 1 ADC: 1 + 1 + arch_CF
 *   If slot-0's CF reached arch flags → RBX_out = 3
 *   If not                            → RBX_out = 2  (entry CF=0)
 *
 * Variants on slot-0 ADD's DESTINATION register:
 *
 *   TMP0      (baseline — already known to fail)
 *   R64DST    (macroinstruction operand class)
 *   R64SRC    (other macroinstruction operand class)
 *   RAX       (arch reg via direct ID 0x20)
 *   RDX       (arch reg via direct ID 0x22)
 *   TMPV0     (alternate temp class, 0x14)
 *   MM0       (MMX class, 0x30 — overlaps TMP0)
 *   TMM0      (AMX class, 0x38 — overlaps TMP8)
 *
 * Build: make PROG=intra-triad-adc-regclass
 * Run:   sudo taskset -c 0 ./intra-triad-adc-regclass_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Install a patch where slot-0 ADD writes to the requested destination ID. */
static void install_variant(unsigned long add_dst) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMPV0, R64DST, R64SRC),       /* slot 0 */
          ADC_DSZ64_DRR(R64DST, R64SRC, R64SRC),        /* slot 1 */
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire with RBX=a, RCX=b, arch CF=cf_in. Return RBX after the patch. */
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

typedef struct {
    const char *name;
    unsigned long dst_id;
} variant_t;

static variant_t variants[] = {
    { "TMP0   (baseline: TMP-class)",   TMP0   },
    { "R64DST (macroinstr operand)",    R64DST },
    { "R64SRC (macroinstr operand)",    R64SRC },
    { "RAX    (arch reg, 0x20)",        RAX    },
    { "RDX    (arch reg, 0x22)",        RDX    },
    { "TMPV0  (alt-temp class, 0x14)",  TMPV0  },
    { "MM0    (MMX class, 0x30)",       MM0    },
    { "TMM0   (AMX class, 0x38)",       TMM0   },
};

int main(void) {
    printf("============================================================\n");
    printf("  Does slot-0 ADD's destination register class affect whether\n");
    printf("  its CF reaches a slot-1 ADC in the same triad?\n");
    printf("============================================================\n\n");
    printf("  Triad: ADD {dst} = R64DST + R64SRC ;  ADC R64DST = R64SRC + R64SRC + CF\n");
    printf("  Inputs: RBX=0xFFFF…FFFF, RCX=1, entry arch CF=0\n");
    printf("  slot-0 ADD: 0xFFFF…FFFF + 1 = 0  (internal CF=1)\n");
    printf("  slot-1 ADC: 1 + 1 + arch_CF\n");
    printf("  → RBX_out = 3 means slot-0's CF reached arch CF\n");
    printf("  → RBX_out = 2 means it did not\n\n");

    assign_to_core(0);

    printf("  %-40s  RBX_out  verdict\n", "slot-0 ADD destination");
    printf("  ----------------------------------------  -------  -----------------------\n");

    int found_bridge = 0;
    for (size_t i = 0; i < sizeof(variants)/sizeof(variants[0]); i++) {
        install_variant(variants[i].dst_id);
        uint64_t r = fire(0xFFFFFFFFFFFFFFFFULL, 1, /*cf_in=*/0);
        const char *verdict;
        if      (r == 3) { verdict = "★ CF PROPAGATED (1+1+1)"; found_bridge = 1; }
        else if (r == 2) { verdict = "  no propagation (1+1+0)";                  }
        else             { verdict = "  unexpected value";                         }
        printf("  %-40s  %3" PRIu64 "      %s\n",
               variants[i].name, r, verdict);
    }

    printf("\n");
    printf("============================================================\n");
    if (found_bridge) {
        printf("  ★ One or more destination classes propagates ADD's CF\n");
        printf("    into arch CF, visible to ADC in the same triad.\n");
        printf("    → ADC chaining may be feasible via that encoding.\n");
    } else {
        printf("  All variants returned 2: no destination register class\n");
        printf("  routes slot-0's CF into arch CF. ADC stays one-shot.\n");
    }
    printf("============================================================\n");
    return 0;
}
