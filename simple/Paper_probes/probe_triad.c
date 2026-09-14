/*
 * probe_triad.c — Pin down the exact intra-triad dependency rule on this box.
 * probe_stlf showed a 3-deep RAW chain in one triad fails. Determine which
 * intra-triad dependency patterns are safe so the Keccak packer can obey them.
 *
 * Each sub-test: load inputs from buf, run ONE test triad, store result(s),
 * compare to the value if the triad executed sequentially (slot0,1,2).
 *
 * Build: make PROG=probe_triad ;  Run: sudo taskset -c 0 ./probe_triad_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

#define SEG 0x18
static uint64_t g_buf[40];
#define A 0x1111111111111111ULL
#define B 0x2222222222222222ULL
#define C 0x4444444444444444ULL
#define D 0x8888888888888888ULL

static void fire(void) {
    register uint64_t *_b asm("rcx") = g_buf;
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

/* load A,B,C,D into RDI,RSI,RBX,RDX; run `mid` triad; store regs in `outs`. */
static void run(const char *name, ucode_t mid, const int *out_regs,
                const int *out_slots, int nout, uint64_t *expv) {
    ucode_t patch[8];
    int n = 0;
    patch[n++] = (ucode_t){ LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, 0,  SEG), NOP, NOP, NOP_SEQWORD };
    patch[n++] = (ucode_t){ LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, 8,  SEG), NOP, NOP, NOP_SEQWORD };
    patch[n++] = (ucode_t){ LDZX_DSZ64_ASZ32_SC1_DRI(RBX, RCX, 16, SEG), NOP, NOP, NOP_SEQWORD };
    patch[n++] = (ucode_t){ LDZX_DSZ64_ASZ32_SC1_DRI(RDX, RCX, 24, SEG), NOP, NOP, NOP_SEQWORD };
    patch[n++] = mid;
    /* store outputs to buf[5+i] — offset (5+i)*8 stays within the 8-bit SIGNED
     * offset range (probe_offset: usable -128..+127 bytes). buf[30] (offset 240)
     * was out of range and silently wrote base-16 — that's what corrupted the
     * earlier run, NOT a RAW-chain failure. */
    for (int i = 0; i < nout; i++) {
        patch[n] = (ucode_t){ STAD_DSZ64_ASZ32_SC1_RRI(out_regs[i], RCX, (5+i)*8, SEG),
                              NOP, NOP, (i==nout-1)?END_SEQWORD:NOP_SEQWORD };
        n++;
    }
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, n);

    memset(g_buf, 0, sizeof(g_buf));
    g_buf[0]=A; g_buf[1]=B; g_buf[2]=C; g_buf[3]=D;
    fire();
    int ok = 1;
    char detail[256] = "";
    for (int i = 0; i < nout; i++) {
        if (g_buf[5+i] != expv[i]) ok = 0;
        char s[64];
        snprintf(s, sizeof(s), " out%d=%016" PRIx64 "(exp %016" PRIx64 ")", i, g_buf[5+i], expv[i]);
        strncat(detail, s, sizeof(detail)-strlen(detail)-1);
    }
    (void)out_slots;
    printf("%-28s %s%s\n", name, ok ? "PASS" : "FAIL", ok ? "" : detail);
}

int main(void) {
    printf("=== probe_triad: intra-triad dependency rules ===\n\n");
    assign_to_core(0);
    int rax[1] = {RAX}, rbx[1] = {RBX};

    /* 1. RAW 0->1 same dest: RAX=A^B; RAX=RAX^C */
    { uint64_t e[1]={A^B^C};
      run("RAW 0->1 same dst (2-deep)",
        (ucode_t){ XOR_DSZ64_DRR(RAX,RDI,RSI), XOR_DSZ64_DRR(RAX,RAX,RBX), NOP, NOP_SEQWORD },
        rax, rax, 1, e); }

    /* 2. RAW 1->2 same dest */
    { uint64_t e[1]={A^B^C};
      run("RAW 1->2 same dst (2-deep)",
        (ucode_t){ NOP, XOR_DSZ64_DRR(RAX,RDI,RSI), XOR_DSZ64_DRR(RAX,RAX,RBX), NOP_SEQWORD },
        rax, rax, 1, e); }

    /* 3. RAW 0->2 same dest */
    { uint64_t e[1]={A^B^C};
      run("RAW 0->2 same dst",
        (ucode_t){ XOR_DSZ64_DRR(RAX,RDI,RSI), NOP, XOR_DSZ64_DRR(RAX,RAX,RBX), NOP_SEQWORD },
        rax, rax, 1, e); }

    /* 4. RAW 3-deep chain 0->1->2 (probe_stlf showed FAIL) */
    { uint64_t e[1]={A^B^C^D};
      run("RAW 3-deep chain",
        (ucode_t){ XOR_DSZ64_DRR(RAX,RDI,RSI), XOR_DSZ64_DRR(RAX,RAX,RBX), XOR_DSZ64_DRR(RAX,RAX,RDX), NOP_SEQWORD },
        rax, rax, 1, e); }

    /* 5. RAW 0->1 distinct dest: RAX=A^B; RBX=RAX^C(old RBX irrelevant) -> store RBX */
    { uint64_t e[1]={(A^B)^C};
      run("RAW 0->1 distinct dst",
        (ucode_t){ XOR_DSZ64_DRR(RAX,RDI,RSI), XOR_DSZ64_DRR(RBX,RAX,RBX), NOP, NOP_SEQWORD },
        rbx, rbx, 1, e); }

    /* 6. XOR then ROL same dest (apply pattern): RAX=A^B; RAX=rol(RAX,4) */
    { uint64_t v=A^B; uint64_t e[1]={ (v<<4)|(v>>60) };
      run("XOR->ROL same dst",
        (ucode_t){ XOR_DSZ64_DRR(RAX,RDI,RSI), ROL_DSZ64_DRI(RAX,RAX,4), NOP, NOP_SEQWORD },
        rax, rax, 1, e); }

    /* 7. two independent ops + a third independent (no deps) */
    { uint64_t e[1]={A^B};
      run("independent (sanity)",
        (ucode_t){ XOR_DSZ64_DRR(RAX,RDI,RSI), XOR_DSZ64_DRR(RBP,RBX,RDX), NOP, NOP_SEQWORD },
        rax, rax, 1, e); }

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nInterpretation: PASS rows are safe to pack; FAIL rows must be split.\n");
    return 0;
}
