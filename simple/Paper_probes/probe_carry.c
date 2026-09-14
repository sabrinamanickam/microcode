/*
 * probe_carry.c — can we drive ADC from a computed carry?
 *
 * probe_opsem settled three things. IMUL64L_DSZ64 is a genuine non destructive
 * 64 bit low multiply, both operand forms verified exactly. SHRD and SHLD are
 * 32 bit only, so they cannot funnel a 51 bit limb and the hoped for three ops
 * to one carry extraction is dead. STC, CLC and SAHF executed without faulting
 * but changed no register, which is expected since they only touch CF, so they
 * remain untested.
 *
 * That leaves the carry flag as the one remaining lever, worth roughly 20 to
 * 25 operations per kernel. Our patches emulate it with ADD then SETCC then a
 * fold, three operations where native uses one ADC. This probe asks, in order
 * of increasing usefulness:
 *
 *   does ADC read a carry we can set at all          (STC / CLC)
 *   does an ADD's carry out reach a following ADC    (no bridge)
 *   does GENARITHFLAGS bridge it                     (the known mechanism)
 *   does an ADC's carry out reach the NEXT ADC       (chaining, the real prize)
 *   can SAHF inject a computed carry from a register (a cheaper bridge)
 *
 * Inputs are chosen so one bit of carry is unmistakable:
 *   RDI + RSI = ffffffffffffffff, so with a carry in it is 0000000000000000
 *   RDI + RDI = 02468acf13579bde, so with a carry in it ends bdf not bde
 * R15 = aaaa... doubles with a carry out, RDI does not, giving a matched pair
 * of carry producing and non carry producing ADDs.
 *
 * Each candidate also captures the flag word through LAHF, which in
 * probe_opsem returned 0200, so a CF of one should read back as 0201.
 *
 * Same safety as probe_opsem: one candidate per firing, logged and fsynced
 * before it fires, resumable with a start index.
 *
 * Build: make PROG=probe_carry
 * Run:   sudo taskset -c 0 ./probe_carry_static [start_index]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

#define VA 0x0123456789ABCDEFULL   /* RDI */
#define VB 0xFEDCBA9876543210ULL   /* RSI */
#define VD 0xAAAAAAAAAAAAAAAAULL   /* R15 */
#define VC 0xCCCCCCCCCCCCCCCCULL   /* RCX */
#define VX 0xDDDDDDDDDDDDDDDDULL   /* RDX */

#define _STC_OP  (0x33aUL << 32)
#define _CLC_OP  (0x338UL << 32)
#define _SAHF_OP (0x1bfUL << 32)
#define _LAHF_OP (0x3c0UL << 32)

static struct { uint64_t in[5]; uint64_t out[5]; } g_st;

#define FIRE() \
    "mov rdi, [rbp + 0]\n\t"  "mov rsi, [rbp + 8]\n\t"  \
    "mov r15, [rbp + 16]\n\t" "mov rcx, [rbp + 24]\n\t" \
    "mov rdx, [rbp + 32]\n\t" "vmwrite rcx, rdx\n\t"    \
    "mov [rbp + 40], r15\n\t" "mov [rbp + 48], r13\n\t" \
    "mov [rbp + 56], r9\n\t"  "mov [rbp + 64], r10\n\t" \
    "mov [rbp + 72], rax\n\t"

static void fire(void) {
    g_st.in[0]=VA; g_st.in[1]=VB; g_st.in[2]=VD; g_st.in[3]=VC; g_st.in[4]=VX;
    memset(g_st.out, 0, sizeof g_st.out);
    register void *st asm("rbp") = &g_st;
    asm volatile(FIRE() : : "r"(st)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");
}

typedef struct { const char *name; uint64_t u0,u1,u2; const char *expect; } cand_t;
static FILE *log_f;
static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap);
    va_start(ap,fmt); vfprintf(log_f,fmt,ap); va_end(ap);
    fflush(log_f); fsync(fileno(log_f)); fflush(stdout);
}

int main(int argc, char **argv) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    int start = (argc > 1) ? atoi(argv[1]) : 0;

    cand_t C[] = {
    { "CONTROL ADD R15=RDI+RSI",
      ADD_DSZ64_DRR(R15, RDI, RSI), NOP, NOP,
      "R15 = ffffffffffffffff exactly (no carry in anywhere)" },
    { "ADC alone, CF at patch entry",
      ADC_DSZ64_DRR(R15, RDI, RSI), NOP, NOP,
      "ffff..ff means CF=0 on entry, 0000..00 means CF=1" },
    { "STC then ADC",
      _STC_OP, ADC_DSZ64_DRR(R15, RDI, RSI), NOP,
      "0000000000000000 if STC drives ADC" },
    { "CLC then ADC",
      _CLC_OP, ADC_DSZ64_DRR(R15, RDI, RSI), NOP,
      "ffffffffffffffff if CLC drives ADC" },

    { "carrying ADD then ADC, no bridge",
      ADD_DSZ64_DRR(TMP0, R15, R15), ADC_DSZ64_DRR(R15, RDI, RSI), NOP,
      "R15+R15 carries out; 0000..00 means ADD's carry reached ADC unaided" },
    { "non-carrying ADD then ADC, no bridge",
      ADD_DSZ64_DRR(TMP0, RDI, RDI), ADC_DSZ64_DRR(R15, RDI, RSI), NOP,
      "control for the previous line; should be ffff..ff either way" },

    { "carrying ADD, GFL_RR(TMP0,TMP0), ADC",
      ADD_DSZ64_DRR(TMP0, R15, R15), GENARITHFLAGS_RR(TMP0, TMP0),
      ADC_DSZ64_DRR(R15, RDI, RSI),
      "0000..00 if GENARITHFLAGS bridges a computed carry into ADC" },
    { "non-carrying ADD, GFL_RR, ADC",
      ADD_DSZ64_DRR(TMP0, RDI, RDI), GENARITHFLAGS_RR(TMP0, TMP0),
      ADC_DSZ64_DRR(R15, RDI, RSI),
      "ffff..ff; together with the previous line this is the bridge working" },

    { "CHAIN: STC, ADC->R15, ADC->R13",
      _STC_OP, ADC_DSZ64_DRR(R15, RDI, RSI), ADC_DSZ64_DRR(R13, RDI, RDI),
      "R15=0000..00 and R13 ending bdf means ADC's carry out fed the next ADC" },
    { "CHAIN: CLC, ADC->R15, ADC->R13",
      _CLC_OP, ADC_DSZ64_DRR(R15, RDI, RSI), ADC_DSZ64_DRR(R13, RDI, RDI),
      "R15=ffff..ff and R13 ending bde; the no carry version of the chain" },

    { "SAHF from bit 0 set, then ADC",
      ZEROEXT_DSZ32_DI(TMP0, 0x1), _SAHF_OP | INSTR_R0(TMP0),
      ADC_DSZ64_DRR(R15, RDI, RSI),
      "0000..00 if SAHF takes CF from bit 0 of the register" },
    { "SAHF from bit 8 set, then ADC",
      ZEROEXT_DSZ32_DI(TMP0, 0x100), _SAHF_OP | INSTR_R0(TMP0),
      ADC_DSZ64_DRR(R15, RDI, RSI),
      "0000..00 if SAHF takes CF from bit 8, the classic AH position" },
    { "SAHF from zero, then ADC",
      ZEROEXT_DSZ32_DI(TMP0, 0x0), _SAHF_OP | INSTR_R0(TMP0),
      ADC_DSZ64_DRR(R15, RDI, RSI),
      "ffff..ff; the control for the two SAHF lines above" },

    { "SETCC then SAHF bit 0, then ADC",
      ADD_DSZ64_DRR(TMP0, R15, R15), SETCC_CONDB_DR(TMP1, TMP0),
      _SAHF_OP | INSTR_R0(TMP1),
      "sets up a COMPUTED carry; R15 unchanged here, see the next entry" },
    { "intra-triad ADC, GFL_RR, ADC",
      ADC_DSZ64_DRR(R15, RDI, RSI), GENARITHFLAGS_RR(TMP0, TMP0),
      ADC_DSZ64_DRR(R13, RDI, RDI),
      "the pattern recorded earlier as bridging CF inside one triad" },
    };
    const int N = (int)(sizeof C / sizeof C[0]);

    log_f = fopen("probe_carry_out.txt", "a");
    if (!log_f) { perror("log"); return 1; }
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    logline("\n=== probe_carry, %d candidates, starting at %d ===\n", N, start);
    logline("RDI=%016" PRIx64 " RSI=%016" PRIx64 " R15=%016" PRIx64 "\n",
            VA, VB, VD);
    logline("RDI+RSI=ffffffffffffffff  RDI+RSI+1=0000000000000000\n");
    logline("RDI+RDI=%016" PRIx64 "  RDI+RDI+1=%016" PRIx64 "\n\n",
            (VA+VA), (VA+VA+1));

    for (int i = start; i < N; i++) {
        ucode_t p[3] = {
            { C[i].u0, C[i].u1, C[i].u2, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R9, RDI), ZEROEXT_DSZ64_DR(R10, RSI),
              _LAHF_OP | INSTR_DR0(RAX, RDI), NOP_SEQWORD },
            { NOP, NOP, NOP, END_SEQWORD }
        };
        logline("[%2d] %s\n     expect: %s\n", i, C[i].name, C[i].expect);
        patch_ucode(0x7c00, p, 3);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        fire();
        logline("     R15=%016" PRIx64 "  R13=%016" PRIx64 "  flags(LAHF)=%016" PRIx64 "\n",
                g_st.out[0], g_st.out[1], g_st.out[4]);
        logline("     RDI %s  RSI %s\n\n",
                g_st.out[2]==VA?"intact":"CLOBBERED", g_st.out[3]==VB?"intact":"CLOBBERED");
    }
    logline("=== completed all %d candidates ===\n", N);
    fclose(log_f);
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
