/*
 * probe_index.c — Verify index-register LDZX/STAD ([base + index_reg]) on this
 * box. Needed for Phase 4: the RC table + loop counter live beyond the 8-bit
 * signed immediate-offset range, so they must be addressed via a full-width
 * index register. Uses the proven probe_offset harness (result read from
 * memory, minimal clobbers).
 *
 * base = &g_buf[16]. g_buf[40] (offset +192, OUT of immediate range) holds a
 * marker. Load it via index reg = (40-16)*8 = 192, store it back via index reg
 * to g_buf[44] (offset +224, also out of immediate range). Read both.
 *
 * Build: make PROG=probe_index ; Run: sudo taskset -c 0 ./probe_index_static
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
#define BASE_IDX 16
static uint64_t g_buf[64];

static void fire(void) {
    register uint64_t *_b asm("rcx") = &g_buf[BASE_IDX];
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) : : "rax","memory","cc");
}

int main(void) {
    printf("=== probe_index: index-register LDZX/STAD ===\n");
    printf("g_buf=%p base=&g_buf[%d]\n\n", (void*)g_buf, BASE_IDX);
    if ((uint64_t)g_buf >= 0x100000000ULL) { printf("FATAL >4GB\n"); return 1; }
    assign_to_core(0);

    /* index-register LOAD then index-register STORE.
     *   TMP0 = 192  (= (40-16)*8) ; RAX = [base + TMP0] = g_buf[40]
     *   TMP1 = 224  (= (44-16)*8) ; [base + TMP1] = RAX  -> g_buf[44]
     * Index reg is full width, so 192/224 are fine even though they exceed the
     * 8-bit signed immediate offset range. */
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 192), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRR(RAX, RCX, TMP0, SEG), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ32_DI(TMP1, 224), NOP, NOP, NOP_SEQWORD },
        { STAD_DSZ64_ASZ32_SC1_RRR(RAX, RCX, TMP1, SEG), NOP, NOP, END_SEQWORD },
    };
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, p, 4);

    memset(g_buf, 0, sizeof(g_buf));
    g_buf[40] = 0xC0FFEE0011223344ULL;
    fire();

    int load_ok  = (g_buf[44] == 0xC0FFEE0011223344ULL);  /* round-tripped via index */
    printf("indexed load+store: g_buf[44]=%016" PRIx64 "  exp %016llx  %s\n",
           g_buf[44], 0xC0FFEE0011223344ULL, load_ok ? "PASS" : "FAIL");

    init_match_and_patch(); do_fix_IN_patch();
    return load_ok ? 0 : 1;
}
