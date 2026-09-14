/*
 * adcx_probe.c — Probe a single unknown microcode opcode for ADCX/ADOX-like
 * semantics. One opcode per process. Wrap with `timeout` so a core hang only
 * kills the current probe, not the whole sweep.
 *
 * Usage:   sudo taskset -c 0 ./adcx_probe_static 0x33b
 * Hexopt:  0x000 .. 0xfff (12-bit opcode id, shifted to bit 32 internally)
 *
 * Patch shape (R64DST=rbx in, R64SRC=rcx in, RBX=flags out, RDX=result out):
 *
 *   T0: ZEROEXT_DSZ32 TMP0 = 0xDEADBEEF       // sentinel — detect non-write
 *   T1: <candidate>   TMP0 = R64DST + R64SRC  // op under test (DRR encoded)
 *   T2: READAFLAGS    TMP1 = TMP1             // capture arch flags after op
 *   T3: ZEROEXT_DSZ64 RDX  = TMP0             // export result
 *   T4: ZEROEXT_DSZ64 RBX  = TMP1             // export flags
 *   T5: END
 *
 * Two input sets, four (CF_in, OF_in) vectors each → 8 probes per opcode:
 *   OVF: A = B = 0x8000_0000_0000_0000  →  A+B overflows BOTH unsigned (CF=1
 *        from the math) and signed (neg+neg→non-neg gives OF=1).
 *   NOO: A = 1, B = 2  →  A+B = 3, no overflow either way (math CF=OF=0).
 *
 * The two sets together let us tell apart "writes CF from the math" (CF_out
 * tracks overflow across both sets) from "preserves CF_in" (CF_out equals
 * CF_in regardless of math).
 *
 * Result × CF behavior × OF behavior → verdict:
 *
 *    result        CF behavior     OF behavior      label
 *    ──────        ───────────     ───────────      ─────
 *    A+B           writes-math     writes-math      ADD (classic)
 *    A+B           preserves       preserves        ADD-flag-clean
 *    A+B+CF_in     writes-math     writes-math      ADC (classic)
 *    A+B+CF_in     writes-math     preserves        ADCX  ★
 *    A+B+CF_in     preserves       preserves        ADC-flag-clean (0x37e)
 *    A+B+OF_in     preserves       writes-math      ADOX  ★
 *    sentinel      *               *                NOWR (op didn't write dst)
 *
 * Build:   make PROG=adcx_probe
 * Sweep:   see sweep_adcx.sh
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Raw opcode at bit 32, with a DRR operand encoding. */
#define RAW_DRR(opc12, dst, src0, src1) \
    ( ((uint64_t)((opc12) & 0xfff) << 32) | INSTR_DRR(dst, src0, src1) )

#define SENTINEL 0xDEADBEEFu

static void install_probe(uint32_t opc12) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP0, SENTINEL),       NOP, NOP, NOP_SEQWORD },
        { RAW_DRR(opc12, TMP0, R64DST, R64SRC),   NOP, NOP, NOP_SEQWORD },
        { READAFLAGS_DR(TMP1, TMP1),              NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RDX, TMP0),            NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1),            NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP,                          END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire the patched vmwrite with controlled entry CF/OF and return (flags,result). */
static void fire(uint64_t a, uint64_t b, int cf_in, int of_in,
                 uint64_t *flags_out, uint64_t *result_out) {
    /* EFLAGS bit 1 is reserved-1.  CF = bit 0.  OF = bit 11. */
    uint64_t flg = 0x2ULL | (cf_in ? 0x1ULL : 0) | (of_in ? (1ULL << 11) : 0);
    uint64_t flags, result;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "push %[flg]\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[flags],  rbx\n\t"
        "mov  %[result], rdx\n\t"
        : [flags]  "=r"(flags),
          [result] "=r"(result)
        : [a]   "r"(a),
          [b]   "r"(b),
          [flg] "r"(flg)
        : "rbx", "rcx", "rdx", "cc", "memory"
    );
    *flags_out  = flags;
    *result_out = result;
}

struct v {
    uint64_t a, b;
    int cf_in, of_in;
    int math_cf, math_of;     /* expected if op writes CF/OF "from the math" */
    uint64_t result, flags;
};

static int bit(uint64_t f, int b) { return (int)((f >> b) & 1); }

/* Classify 8 vectors (4 overflowing, 4 non-overflowing) against signatures. */
static const char *classify(struct v vec[8]) {
    int all_sentinel = 1, any_sentinel = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t lo = (uint32_t)vec[i].result;
        if (lo == SENTINEL) any_sentinel = 1; else all_sentinel = 0;
    }
    if (all_sentinel) return "NOWR/skip";
    if (any_sentinel) return "PARTIAL-WRITE";

    /* Result-shape candidates. */
    int is_result_add = 1, is_result_adc = 1, is_result_adox = 1;
    /* CF/OF behavior across all 8 vectors. */
    int cf_writes_math = 1, cf_preserved = 1;
    int of_writes_math = 1, of_preserved = 1;

    for (int i = 0; i < 8; i++) {
        uint64_t a = vec[i].a, b = vec[i].b;
        int cf_in = vec[i].cf_in, of_in = vec[i].of_in;
        int cf_out = bit(vec[i].flags, 0);
        int of_out = bit(vec[i].flags, 11);

        if (vec[i].result != a + b)                      is_result_add  = 0;
        if (vec[i].result != a + b + (uint64_t)cf_in)    is_result_adc  = 0;
        if (vec[i].result != a + b + (uint64_t)of_in)    is_result_adox = 0;

        if (cf_out != vec[i].math_cf) cf_writes_math = 0;
        if (of_out != vec[i].math_of) of_writes_math = 0;
        if (cf_out != cf_in)          cf_preserved   = 0;
        if (of_out != of_in)          of_preserved   = 0;
    }

    /* Verdict ladder.  Order matters: ADC and ADCX both have "result=A+B+CF",
     * so check the more specific (flag-preserving) variants first. */
    if (is_result_adc  && cf_writes_math && of_preserved)   return "ADCX  ★";
    if (is_result_adox && of_writes_math && cf_preserved)   return "ADOX  ★";
    if (is_result_adc  && cf_writes_math && of_writes_math) return "ADC (classic)";
    if (is_result_adc  && cf_preserved   && of_preserved)   return "ADC-flag-clean";
    if (is_result_add  && cf_writes_math && of_writes_math) return "ADD (classic)";
    if (is_result_add  && cf_preserved   && of_preserved)   return "ADD-flag-clean";
    if (is_result_add)                                      return "ADD-result/weird-flags";
    if (is_result_adc)                                      return "ADC-result/weird-flags";
    if (is_result_adox)                                     return "ADOX-result/weird-flags";
    if (cf_preserved && of_preserved)                       return "non-add result, flags preserved";
    return "OTHER";
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <opcode-hex 0x000..0xfff>\n", argv[0]);
        return 2;
    }
    uint32_t opc = (uint32_t)strtoul(argv[1], NULL, 0) & 0xfff;

    assign_to_core(0);
    install_probe(opc);

    /* Two input sets.  OVF: 0x8000..00 + 0x8000..00 overflows both unsigned
     * and signed (math_cf=1, math_of=1).  NOO: 1 + 2 = 3 (math_cf=0, math_of=0).
     * For each set we sweep all (CF_in, OF_in) pairs. */
    const uint64_t A_OVF = 0x8000000000000000ULL, B_OVF = 0x8000000000000000ULL;
    const uint64_t A_NOO = 1ULL,                  B_NOO = 2ULL;

    struct v vec[8] = {
        { A_OVF, B_OVF, 0, 0, 1, 1, 0, 0 },
        { A_OVF, B_OVF, 1, 0, 1, 1, 0, 0 },
        { A_OVF, B_OVF, 0, 1, 1, 1, 0, 0 },
        { A_OVF, B_OVF, 1, 1, 1, 1, 0, 0 },
        { A_NOO, B_NOO, 0, 0, 0, 0, 0, 0 },
        { A_NOO, B_NOO, 1, 0, 0, 0, 0, 0 },
        { A_NOO, B_NOO, 0, 1, 0, 0, 0, 0 },
        { A_NOO, B_NOO, 1, 1, 0, 0, 0, 0 },
    };
    for (int i = 0; i < 8; i++) {
        fire(vec[i].a, vec[i].b, vec[i].cf_in, vec[i].of_in,
             &vec[i].flags, &vec[i].result);
    }

    const char *verdict = classify(vec);

    printf("OPC=0x%03x VERDICT=%s\n", opc, verdict);
    for (int i = 0; i < 8; i++) {
        printf("  %s cf_in=%d of_in=%d  result=0x%016" PRIx64
               "  CF=%d OF=%d ZF=%d SF=%d  (math_cf=%d math_of=%d)\n",
               i < 4 ? "ovf" : "noo",
               vec[i].cf_in, vec[i].of_in, vec[i].result,
               bit(vec[i].flags, 0), bit(vec[i].flags, 11),
               bit(vec[i].flags, 6), bit(vec[i].flags, 7),
               vec[i].math_cf, vec[i].math_of);
    }
    return 0;
}
