/*
 * probe_rsp.c — Can microcode use RSP as a 32nd data register?
 *
 * The integer register file is 32 (16 GPR 0x20-0x2f + 16 TMP 0x30-0x3f). RSP is
 * reserved as the stack pointer, leaving 31 usable — and Keccak's register-
 * resident round needs exactly 32. But our microcode never touches the stack,
 * so if we SAVE RSP at entry, use it as data, and RESTORE it before the vmwrite
 * returns, RSP becomes a usable 32nd register. This probe tests that.
 *
 * Patch: save RSP->TMP0; RSP = sentinel; store RSP -> buf[0]; restore RSP<-TMP0; END.
 * Pass = buf[0]==sentinel AND the process doesn't crash (RSP correctly restored).
 *
 * Build: make PROG=probe_rsp ; Run: sudo taskset -c 0 ./probe_rsp_static
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
static uint64_t g_buf[8];

static void fire(void){
    register uint64_t *_b asm("rcx") = g_buf;
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
    /* NOTE: RSP intentionally NOT clobbered — the microcode must leave it intact. */
}

int main(void){
    printf("=== probe_rsp: use RSP as a data register in microcode ===\n");
    printf("g_buf @ %p\n\n", (void*)g_buf);
    if((uint64_t)g_buf>=0x100000000ULL){printf("FATAL >4GB\n");return 1;}
    assign_to_core(0);
    init_match_and_patch(); do_fix_IN_patch();

    /* T0: TMP0 = RSP            (save stack pointer)
     * T1: RSP  = 0x12345        (use RSP as a data reg)
     * T2: STAD RSP -> buf[0]    (prove we can read it back)
     * T3: RSP  = TMP0 ; END     (restore stack pointer) */
    ucode_t patch[] = {
        { ZEROEXT_DSZ64_DR(TMP0, RSP), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ32_DI(RSP, 0x12345 & 0xffff), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRI(RSP, RCX, 0, SEG), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RSP, TMP0), NOP, NOP, END_SEQWORD },
    };
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, 4);

    memset(g_buf, 0, sizeof(g_buf));
    fire();
    uint64_t want = 0x12345 & 0xffff;
    printf("buf[0] = 0x%" PRIx64 "  expect 0x%" PRIx64 "  %s\n",
           g_buf[0], want, g_buf[0]==want ? "PASS (RSP usable as data reg)" : "FAIL");
    printf("(reached here => RSP was restored correctly, no stack corruption)\n");

    init_match_and_patch(); do_fix_IN_patch();
    return g_buf[0]==want ? 0 : 1;
}
