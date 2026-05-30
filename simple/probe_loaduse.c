/*
 * probe_loaduse.c — Why does "4 loads then XOR" return 0 while STLF worked?
 * Isolate: load-into-RDI, multiple loads, ALU-after-load, latency gap.
 *
 * Build: make PROG=probe_loaduse ; Run: sudo taskset -c 0 ./probe_loaduse_static
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
static uint64_t g_buf[40];

static void fire(void) {
    register uint64_t *_b asm("rcx") = g_buf;
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

static void doit(const char *name, ucode_t *p, int n, int out_idx, uint64_t exp) {
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, p, n);
    memset(g_buf, 0, sizeof(g_buf));
    g_buf[0]=0x1111111111111111ULL; g_buf[1]=0x2222222222222222ULL;
    g_buf[2]=0x4444444444444444ULL; g_buf[3]=0x8888888888888888ULL;
    fire();
    printf("%-34s buf[%d]=%016" PRIx64 " exp %016" PRIx64 " %s\n",
           name, out_idx, g_buf[out_idx], exp, g_buf[out_idx]==exp?"PASS":"FAIL");
}

int main(void) {
    printf("=== probe_loaduse ===\n\n");
    assign_to_core(0);

    /* L1: single load into RDI, store it */
    { ucode_t p[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, 0, SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RDI, RCX, 30*8, SEG), NOP, NOP, END_SEQWORD },
      }; doit("L1 LD RDI; ST RDI", p, 2, 30, 0x1111111111111111ULL); }

    /* L2: load RDI, load RSI (separate triads), store both */
    { ucode_t p[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, 0, SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, 8, SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RDI, RCX, 30*8, SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RSI, RCX, 31*8, SEG), NOP, NOP, END_SEQWORD },
      }; doit("L2 LD RDI,RSI; ST both[30]", p, 4, 30, 0x1111111111111111ULL);
         doit("L2 LD RDI,RSI; ST both[31]", p, 4, 31, 0x2222222222222222ULL); }

    /* L3: load RDI,RSI; XOR RAX=RDI^RSI (own triad); store RAX */
    { ucode_t p[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, 0, SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, 8, SEG), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(RAX, RDI, RSI), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RAX, RCX, 30*8, SEG), NOP, NOP, END_SEQWORD },
      }; doit("L3 LD RDI,RSI; XOR; ST", p, 4, 30, 0x3333333333333333ULL); }

    /* L4: like L3 but 3 NOP triads between loads and XOR (latency gap) */
    { ucode_t p[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, 0, SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, 8, SEG), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(RAX, RDI, RSI), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RAX, RCX, 30*8, SEG), NOP, NOP, END_SEQWORD },
      }; doit("L4 LD,LD,3xNOP,XOR,ST", p, 7, 30, 0x3333333333333333ULL); }

    /* L5: load into RAX,RBX (like working STLF T1 regs); XOR RDX=RAX^RBX; store */
    { ucode_t p[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RAX, RCX, 0, SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RBX, RCX, 8, SEG), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(RDX, RAX, RBX), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RDX, RCX, 30*8, SEG), NOP, NOP, END_SEQWORD },
      }; doit("L5 LD RAX,RBX; XOR RDX; ST", p, 4, 30, 0x3333333333333333ULL); }

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
