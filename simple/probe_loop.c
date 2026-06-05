/*
 * probe_loop.c — Validate the Phase 4 loop mechanism + indexed load before
 * building the 24-round permutation. Three tests, bounded ones first:
 *
 *   T1  forward UJMPCC CONDZ taken/fall-through (bounded, no backward jump):
 *       confirms XOR sets ZF and the forward conditional reads it.
 *   T3  index-register LDZX [base + index_reg]: confirms RC-table-style access
 *       (offset immediate is only 8-bit signed; the index REGISTER is full width).
 *   T2  counted loop = forward UJMPCC CONDZ -> exit  +  unconditional
 *       SEQ_GOTO0 -> loop_top (backward). Run LAST, after T1 gives confidence
 *       the conditional exits (else infinite loop hangs the core).
 *
 * Loop rules (from microcode-control-flow memory, re-verified here):
 *   - UJMPCC tests arch RFLAGS, not its register operand; XOR sets ZF.
 *   - count UP + compare (SUB-immediate is reversed); 16-bit immediates.
 *
 * Build: make PROG=probe_loop ; Run: sudo taskset -c 0 ./probe_loop_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define SEG 0x18
#define REGION 0x7c00
#define T(n) (REGION + (n)*4)
static uint64_t g_buf[16];

static uint64_t fire(void) {
    uint64_t res;
    register uint64_t *_b asm("rcx") = g_buf;
    asm volatile("vmwrite rcx, rcx\n\t" : "=a"(res), "+r"(_b) :
        : "rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
    return res;
}
static void install(ucode_t *p, int n) {
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, REGION);
    patch_ucode(REGION, p, n);
}

static void t1_cond(void) {
    /* CONDZ taken: TMP0=5; TMP2=TMP0^5 (=0,ZF=1); CONDZ->T3 should branch. */
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP0,5), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRI(TMP2,TMP0,5), NOP, NOP, NOP_SEQWORD },
        { UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP2, T(4)), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ32_DI(RAX,0xDEAD), NOP, NOP, END_SEQWORD },   /* poison */
        { ZEROEXT_DSZ32_DI(RAX,0xBEEF), NOP, NOP, END_SEQWORD },   /* expected */
    };
    install(p, 5);
    uint64_t r = fire();
    printf("T1a CONDZ taken:     RAX=0x%" PRIx64 " exp 0xBEEF  %s\n", r, r==0xBEEF?"PASS":"FAIL");

    /* CONDZ fall-through: TMP2=TMP0^6 (!=0,ZF=0); should NOT branch. */
    ucode_t q[] = {
        { ZEROEXT_DSZ32_DI(TMP0,5), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRI(TMP2,TMP0,6), NOP, NOP, NOP_SEQWORD },
        { UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP2, T(4)), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ32_DI(RAX,0xABCD), NOP, NOP, END_SEQWORD },   /* expected */
        { ZEROEXT_DSZ32_DI(RAX,0xDEAD), NOP, NOP, END_SEQWORD },   /* poison */
    };
    install(q, 5);
    r = fire();
    printf("T1b CONDZ fallthru:  RAX=0x%" PRIx64 " exp 0xABCD  %s\n", r, r==0xABCD?"PASS":"FAIL");
}

static void t3_index(void) {
    /* index-register LDZX: g_buf[2]=0xAAA, g_buf[5]=0xBBB. Load via index reg. */
    memset(g_buf,0,sizeof(g_buf));
    g_buf[2]=0xAAA; g_buf[5]=0xBBB;
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 5*8), NOP, NOP, NOP_SEQWORD },                 /* index=40 bytes */
        { LDZX_DSZ64_ASZ32_SC1_DRR(RAX, RCX, TMP0, SEG), NOP, NOP, END_SEQWORD },
    };
    install(p, 2);
    uint64_t r = fire();
    printf("T3  index LDZX[5]:   RAX=0x%" PRIx64 " exp 0xBBB   %s\n", r, r==0xBBB?"PASS":"FAIL");
}

static void t2_loop(void) {
    /* counted loop: acc += 7, ten times => 70. */
    ucode_t p[] = {
        /*T0*/ { ZEROEXT_DSZ32_DI(TMP0,0), NOP, NOP, NOP_SEQWORD },          /* counter */
        /*T1*/ { ZEROEXT_DSZ32_DI(TMP1,0), NOP, NOP, NOP_SEQWORD },          /* acc */
        /*T2 loop_top*/ { ADD_DSZ64_DRI(TMP1,TMP1,7), NOP, NOP, NOP_SEQWORD },
        /*T3*/ { ADD_DSZ64_DRI(TMP0,TMP0,1), NOP, NOP, NOP_SEQWORD },
        /*T4*/ { XOR_DSZ64_DRI(TMP2,TMP0,10), NOP, NOP, NOP_SEQWORD },       /* ZF iff counter==10 */
        /*T5*/ { UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP2, T(7)), NOP, NOP, NOP_SEQWORD }, /* fwd exit */
        /*T6*/ { NOP, NOP, NOP, SEQ_GOTO0(T(2)) },                           /* uncond back */
        /*T7 exit*/ { ZEROEXT_DSZ64_DR(RAX,TMP1), NOP, NOP, END_SEQWORD },
    };
    install(p, 8);
    uint64_t r = fire();
    printf("T2  counted loop:    RAX=0x%" PRIx64 " (%" PRIu64 ") exp 70   %s\n",
           r, r, r==70?"PASS":"FAIL");
}

int main(void) {
    printf("=== probe_loop (SEG=0x%x) ===\n", SEG);
    printf("g_buf @ %p\n\n", (void*)g_buf);
    if ((uint64_t)g_buf >= 0x100000000ULL) { printf("FATAL >4GB\n"); return 1; }
    assign_to_core(0);
    t1_cond();    /* bounded: must pass before trusting the loop */
    t3_index();   /* bounded */
    t2_loop();    /* backward jump — runs last */
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
