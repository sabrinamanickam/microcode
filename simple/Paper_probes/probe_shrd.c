/*
 * probe_shrd.c — is there a 64 bit funnel shift, and can we address it?
 *
 * Two questions, in order, because the second only matters if the first
 * succeeds.
 *
 * SEMANTICS. The known opcode 0x997 behaved inconsistently across operand
 * forms. DRI(R15,RDI,13) returned (RDI_lo32 : 0) >> 13, a count with a zero
 * low half. DRR(R15,RDI,RSI) returned RSI_lo32, a low half with a zero count.
 * A funnel shift needs three inputs and INSTR_DRI/INSTR_DRR carry two, so it
 * may be unaddressable whatever its width. Part A sweeps the immediate on the
 * known opcode: if the result tracks the immediate as a shift count, the imm
 * field is the count and the low half is unreachable; if it tracks as data,
 * the reverse.
 *
 * WIDTH. Across 28 opcode pairs in opcode.h the size field is bits 7 and 6:
 * DSZ64 = DSZ32 + 0x40 in 27 of them, DSZ16 = +0x80, DSZ8 = +0xC0. Clearing
 * those bits in 0x997 gives a base of 0x917 and four size slots, so the 64 bit
 * form of this operation, if it exists, is one of 0x917, 0x957 or 0x9d7. Parts
 * B and C fire each against inputs whose 64 bit funnel result is distinctive.
 *
 * The microcode ROM dump cannot answer this: MUL_DSZ64 appears zero times in
 * it despite being in every patch we ship, so absence there proves nothing.
 *
 * Same safety as the earlier probes: one candidate per firing, logged and
 * fsynced before firing, resumable via a start index.
 *
 * Build: make PROG=probe_shrd
 * Run:   sudo taskset -c 0 ./probe_shrd_static [start_index]
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

#define VA 0x0123456789ABCDEFULL   /* RDI, the high half candidate */
#define VB 0xFEDCBA9876543210ULL   /* RSI */
#define VD 0xAAAAAAAAAAAAAAAAULL   /* R15, the destination */
#define VC 0xCCCCCCCCCCCCCCCCULL
#define VX 0xDDDDDDDDDDDDDDDDULL

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
    memset(g_st.out,0,sizeof g_st.out);
    register void *st asm("rbp") = &g_st;
    asm volatile(FIRE() : : "r"(st)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");
}

static FILE *log_f;
static void logline(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap);
    va_start(ap,fmt); vfprintf(log_f,fmt,ap); va_end(ap);
    fflush(log_f); fsync(fileno(log_f)); fflush(stdout);
}

typedef struct { const char *name; uint64_t uop; } cand_t;

int main(int argc, char **argv) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    int start = (argc>1) ? atoi(argv[1]) : 0;

    static cand_t C[64]; int n=0;
    static char nm[64][64];
    const int imms[] = { 0, 1, 13, 19, 31, 32, 51, 63 };

    /* Part A: does the immediate act as a shift count on the known opcode */
    for (unsigned i=0;i<sizeof imms/sizeof imms[0];i++) {
        snprintf(nm[n],64,"A: 0x997 DRI(R15,RDI,%d)",imms[i]);
        C[n].name=nm[n]; C[n].uop=(0x997UL<<32)|INSTR_DRI(R15,RDI,imms[i]); n++;
    }
    /* Parts B and C: the four size slots of each base, DRI and DRR */
    const unsigned bases[2]={0x917,0x916}; const char *bn[2]={"SHRD","SHLD"};
    const unsigned offs[4]={0x00,0x40,0x80,0xc0}; const char *on[4]={"32","64","16","8"};
    for (int b=0;b<2;b++) for (int s=0;s<4;s++) {
        unsigned op=bases[b]|offs[s];
        snprintf(nm[n],64,"%s slot%s 0x%03x DRI(R15,RDI,13)",bn[b],on[s],op);
        C[n].name=nm[n]; C[n].uop=((uint64_t)op<<32)|INSTR_DRI(R15,RDI,13); n++;
        snprintf(nm[n],64,"%s slot%s 0x%03x DRR(R15,RDI,RSI)",bn[b],on[s],op);
        C[n].name=nm[n]; C[n].uop=((uint64_t)op<<32)|INSTR_DRR(R15,RDI,RSI); n++;
    }

    log_f=fopen("probe_shrd_out.txt","a");
    if(!log_f){perror("log");return 1;}
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    logline("\n=== probe_shrd, %d candidates, starting at %d ===\n", n, start);
    logline("RDI=%016" PRIx64 " RSI=%016" PRIx64 " R15=%016" PRIx64 "\n", VA, VB, VD);
    logline("a real 64 bit funnel would give one of:\n");
    logline("  (RDI:R15)>>13 = %016" PRIx64 "\n",
            (uint64_t)(((__uint128_t)VA<<64 | VD) >> 13));
    logline("  (R15:RDI)>>13 = %016" PRIx64 "\n",
            (uint64_t)(((__uint128_t)VD<<64 | VA) >> 13));
    logline("  (RDI:RSI)>>13 = %016" PRIx64 "\n\n",
            (uint64_t)(((__uint128_t)VA<<64 | VB) >> 13));

    for (int i=start;i<n;i++) {
        ucode_t p[3] = {
            { C[i].uop, NOP, NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R9, RDI), ZEROEXT_DSZ64_DR(R10, RSI),
              ZEROEXT_DSZ64_DR(R13, RCX), NOP_SEQWORD },
            { NOP, NOP, NOP, END_SEQWORD }
        };
        logline("[%2d] %-38s uop=%012" PRIx64 "\n", i, C[i].name, C[i].uop);
        patch_ucode(0x7c00,p,3);
        hook_match_and_patch(0,0x0cd8,0x7c00);
        fire();
        uint64_t r=g_st.out[0];
        logline("     R15=%016" PRIx64 "%s  RDI %s RSI %s\n", r,
                r==VD ? "  (UNCHANGED)" : (r>>32) ? "  <- has high bits, 64 bit!" : "",
                g_st.out[2]==VA?"ok":"CLOBBERED", g_st.out[3]==VB?"ok":"CLOBBERED");
    }
    logline("=== completed all %d ===\n", n);
    fclose(log_f);
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
