/*
 * probe_stlf.c — Isolate the Phase 3b compute bug. Two independent tests:
 *
 *   T1 STLF: store a reg to buf[25], reload it from buf[25], store to buf[1].
 *            If buf[1]==V, store-to-load forwarding within a patch works.
 *
 *   T2 RAW3: 3-deep dependent XOR chain in ONE triad
 *            { XOR(RAX,RDI,RSI), XOR(RAX,RAX,RBX), XOR(RAX,RAX,RDX) }
 *            then store RAX. If == RDI^RSI^RBX^RDX, 3-deep intra-triad RAW works.
 *
 * Both use SEG_DS (known good) and tiny patches. vmwrite hook.
 *
 * Build: make PROG=probe_stlf
 * Run:   sudo taskset -c 0 ./probe_stlf_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../../../include/patch.h"
#include "../../../../include/ucode_macro.h"
#include "../../../../include/misc.h"

#define SEG 0x18
static uint64_t g_buf[40];

static void fire(void) {
    register uint64_t *_b asm("rcx") = g_buf;
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

static void test_stlf(void) {
    /* buf[0]=V; LD RAX<-buf[0]; ST RAX->buf[25]; LD RBX<-buf[25]; ST RBX->buf[1] */
    ucode_t patch[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RAX, RCX, 0,   SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RAX, RCX, 25*8, SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RBX, RCX, 25*8, SEG), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RBX, RCX, 1*8,  SEG), NOP, NOP, END_SEQWORD },
    };
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, 4);

    memset(g_buf, 0, sizeof(g_buf));
    uint64_t V = 0xCAFEF00DD00DFEEDULL;
    g_buf[0] = V;
    fire();
    printf("T1 STLF: buf[1]=%016" PRIx64 "  expect %016" PRIx64 "  %s\n",
           g_buf[1], V, g_buf[1] == V ? "PASS" : "FAIL");
}

static void test_raw3(void) {
    /* load 4 vals into RDI,RSI,RBX,RDX; 3-deep XOR chain in one triad -> RAX; store */
    ucode_t patch[] = {
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, 0,  SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, 8,  SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RBX, RCX, 16, SEG), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDX, RCX, 24, SEG), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(RAX, RDI, RSI),
          XOR_DSZ64_DRR(RAX, RAX, RBX),
          XOR_DSZ64_DRR(RAX, RAX, RDX), NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RAX, RCX, 32*8, SEG), NOP, NOP, END_SEQWORD },
    };
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, 6);

    memset(g_buf, 0, sizeof(g_buf));
    uint64_t a=0x1111111111111111ULL, b=0x2222222222222222ULL,
             c=0x4444444444444444ULL, d=0x8888888888888888ULL;
    g_buf[0]=a; g_buf[1]=b; g_buf[2]=c; g_buf[3]=d;
    fire();
    uint64_t exp = a^b^c^d;
    printf("T2 RAW3: buf[32]=%016" PRIx64 "  expect %016" PRIx64 "  %s\n",
           g_buf[32], exp, g_buf[32] == exp ? "PASS" : "FAIL");
}

int main(void) {
    printf("=== probe_stlf: STLF + 3-deep intra-triad RAW ===\n");
    printf("g_buf @ %p (below 4GB: %s)\n\n", (void*)g_buf,
           (uint64_t)g_buf < 0x100000000ULL ? "YES" : "NO");
    assign_to_core(0);
    test_stlf();
    test_raw3();
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
