/*
 * probe_opsem.c — semantics probe for opcodes that opcode.h defines but
 * inst.h never generated macros for.
 *
 * WHY. Our field patches spend roughly 23% of their operations emulating a
 * carry flag and another 20% staging operands around a destructive MUL. Four
 * opcodes in opcode.h would attack exactly that, but gen_inst.py emitted no
 * macros for them, which suggests their operand encodings were never worked
 * out rather than that they do not work:
 *
 *   _SHRD 0x997 / _SHLD 0x996   funnel shift. If the form is
 *                               dst = (src:dst) >> imm then the per limb
 *                               carry extraction SHR + SHL + OR becomes ONE
 *                               operation, saving about 15 ops per kernel.
 *   _IMUL64L_DSZ64 0x264        64 bit low only multiply. If it takes three
 *                               distinct registers it is non destructive,
 *                               unlike MUL_DSZ64_DRR which writes the low
 *                               half into srcB and forces a staging copy.
 *   _SAHF 0x1bf / _LAHF 0x3c0   flags to and from a register, a possible
 *                               carry bridge into the ADC domain.
 *   _STC/_CLC/_CMC 0x338-0x33a  direct carry manipulation.
 *
 * HOW. Each candidate runs ONCE, alone, in a three triad patch, on register
 * operands only, with no memory operation inside the patch. Inputs are
 * distinctive constants so the output decodes unambiguously. Five registers
 * are captured afterwards: the nominated destination, both sources (to detect
 * a destructive write), and RCX and RDX (to detect an implicit one).
 *
 * SAFETY. These encodings are unvalidated and a bad one could wedge the
 * sequencer. Every candidate is written to probe_opsem_out.txt and flushed
 * and fsynced BEFORE it is fired, so if the machine stops, the last line in
 * that file names the opcode that did it. Pass a start index to resume past
 * it:  sudo taskset -c 0 ./probe_opsem_static 7
 *
 * Build: make PROG=probe_opsem
 * Run:   sudo taskset -c 0 ./probe_opsem_static [start_index]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdarg.h>
#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

/* Distinctive inputs. Every bit pattern is asymmetric so a shift, a rotate and
 * a byte swap all produce visibly different answers. */
#define VA 0x0123456789ABCDEFULL   /* RDI */
#define VB 0xFEDCBA9876543210ULL   /* RSI */
#define VD 0xAAAAAAAAAAAAAAAAULL   /* R15, the nominated destination */
#define VC 0xCCCCCCCCCCCCCCCCULL   /* RCX */
#define VX 0xDDDDDDDDDDDDDDDDULL   /* RDX */

static struct { uint64_t in[5]; uint64_t out[5]; } g_st;

/* Load the five knowns, fire, capture five registers. The patch itself does
 * the capture in triads 1 and 2 so that we can see sources and implicit
 * destinations, not just the nominated one. */
#define FIRE() \
    "mov rdi, [rbp + 0]\n\t"  \
    "mov rsi, [rbp + 8]\n\t"  \
    "mov r15, [rbp + 16]\n\t" \
    "mov rcx, [rbp + 24]\n\t" \
    "mov rdx, [rbp + 32]\n\t" \
    "vmwrite rcx, rdx\n\t"    \
    "mov [rbp + 40], r15\n\t" \
    "mov [rbp + 48], r13\n\t" \
    "mov [rbp + 56], r9\n\t"  \
    "mov [rbp + 64], r10\n\t" \
    "mov [rbp + 72], rax\n\t"

static void fire(void) {
    g_st.in[0] = VA; g_st.in[1] = VB; g_st.in[2] = VD;
    g_st.in[3] = VC; g_st.in[4] = VX;
    memset(g_st.out, 0, sizeof g_st.out);
    register void *st asm("rbp") = &g_st;
    asm volatile(FIRE() : : "r"(st)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");
}

typedef struct { const char *name; uint64_t uop; const char *expect; } cand_t;

#define PATCH_AT 0x7c00UL

static FILE *log_f;
static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); va_end(ap);
    va_start(ap, fmt); vfprintf(log_f, fmt, ap); va_end(ap);
    fflush(log_f); fsync(fileno(log_f)); fflush(stdout);
}

int main(int argc, char **argv) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    int start = (argc > 1) ? atoi(argv[1]) : 0;

    /* Controls first: two opcodes whose behaviour we already know. If these
     * do not decode as expected the harness is wrong and nothing after it
     * means anything. */
    cand_t C[] = {
    { "CONTROL ADD_DSZ64_DRR(R15,RDI,RSI)", ADD_DSZ64_DRR(R15, RDI, RSI),
      "R15 = VA+VB, sources untouched" },
    { "CONTROL MUL_DSZ64_DRR(R15,RDI,RSI)", MUL_DSZ64_DRR(R15, RDI, RSI),
      "R15 = hi(VA*VB), RSI = lo (destructive srcB)" },

    { "IMUL64L_DSZ64 DRR(R15,RDI,RSI)", (0x264UL << 32) | INSTR_DRR(R15, RDI, RSI),
      "hoping R15 = lo(VA*VB) with BOTH sources intact" },
    { "IMUL64L_DSZ64 DRI(R15,RDI,19)",  (0x264UL << 32) | INSTR_DRI(R15, RDI, 19),
      "hoping R15 = lo(VA*19), RDI intact" },

    { "SHRD DRI(R15,RDI,13)",  (0x997UL << 32) | INSTR_DRI(R15, RDI, 13),
      "hoping R15 = (RDI:R15)>>13 or (R15:RDI)>>13" },
    { "SHRD DRR(R15,RDI,RSI)", (0x997UL << 32) | INSTR_DRR(R15, RDI, RSI),
      "hoping R15 = (RDI:R15)>>RSI, variable count" },
    { "SHLD DRI(R15,RDI,13)",  (0x996UL << 32) | INSTR_DRI(R15, RDI, 13),
      "hoping R15 = (R15:RDI)<<13" },

    /* opcode.h defines _CONCAT only for DSZ8/16/32, so there is no 64 bit
     * CONCAT to test; the 32 bit form is probed only to learn the semantics. */
    { "CONCAT_DSZ32_DRI(R15,RDI,13)", CONCAT_DSZ32_DRI(R15, RDI, 13),
      "unknown; CONCAT may be a funnel shift or a bitfield insert" },
    { "CONCAT_DSZ32_DRR(R15,RDI,RSI)", CONCAT_DSZ32_DRR(R15, RDI, RSI),
      "unknown" },

    { "READAFLAGS_DR(R15,RDI)", READAFLAGS_DR(R15, RDI),
      "flags of RDI's domain into R15" },
    { "LAHF DR0(R15,RDI)", (0x3c0UL << 32) | INSTR_DR0(R15, RDI),
      "hoping R15 receives the flag byte" },
    { "SAHF R0(RDI)", (0x1bfUL << 32) | INSTR_R0(RDI),
      "sets arch flags from RDI; look for an effect on a later ADC" },
    { "STC (no operand)", (0x33aUL << 32), "sets CF" },
    { "CLC (no operand)", (0x338UL << 32), "clears CF" },
    };
    const int N = (int)(sizeof C / sizeof C[0]);

    log_f = fopen("probe_opsem_out.txt", "a");
    if (!log_f) { perror("log"); return 1; }

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    logline("\n=== probe_opsem run, %d candidates, starting at %d ===\n", N, start);
    logline("inputs  RDI=%016" PRIx64 " RSI=%016" PRIx64 " R15=%016" PRIx64
            " RCX=%016" PRIx64 " RDX=%016" PRIx64 "\n\n", VA, VB, VD, VC, VX);

    for (int i = start; i < N; i++) {
        /* triad 0 runs the candidate; triads 1 and 2 capture the registers */
        ucode_t p[3] = {
            { C[i].uop, NOP, NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R13, RDI), ZEROEXT_DSZ64_DR(R9, RSI),
              ZEROEXT_DSZ64_DR(R10, RCX), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(RAX, RDX), NOP, NOP, END_SEQWORD }
        };

        /* Written and synced BEFORE the firing, so a hang names its cause. */
        logline("[%2d] %-36s uop=%012" PRIx64 "\n     expect: %s\n",
                i, C[i].name, C[i].uop, C[i].expect);

        patch_ucode(PATCH_AT, p, 3);
        hook_match_and_patch(0, 0x0cd8, PATCH_AT);
        fire();

        logline("     R15=%016" PRIx64 "  RDI=%016" PRIx64 "  RSI=%016" PRIx64
                "  RCX=%016" PRIx64 "  RDX=%016" PRIx64 "\n",
                g_st.out[0], g_st.out[1], g_st.out[2], g_st.out[3], g_st.out[4]);
        logline("     dst %s | RDI %s | RSI %s | RCX %s | RDX %s\n\n",
                g_st.out[0] == VD ? "UNCHANGED" : "written",
                g_st.out[1] == VA ? "intact" : "CLOBBERED",
                g_st.out[2] == VB ? "intact" : "CLOBBERED",
                g_st.out[3] == VC ? "intact" : "CLOBBERED",
                g_st.out[4] == VX ? "intact" : "CLOBBERED");
    }

    logline("=== completed all %d candidates ===\n", N);
    fclose(log_f);
    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
