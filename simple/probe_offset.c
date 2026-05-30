/*
 * probe_offset.c — Map the LDZX/STAD offset field behaviour exactly.
 * IMM0_ENCODE masks the offset to 8 bits; probe_loaduse showed offset 240 does
 * not reach buf[30]. Determine signed-vs-unsigned and the usable range by
 * storing a distinct marker at a set of byte-offsets from a CENTERED base and
 * scanning where each landed.
 *
 * base = &g_buf[64]. For test k (byte offset off_k), store marker (100+k) to
 * [base + off_k]. Afterwards, for each marker found in g_buf, report the index
 * vs the expected index if the offset were (a) unsigned or (b) signed.
 *
 * Build: make PROG=probe_offset ; Run: sudo taskset -c 0 ./probe_offset_static
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
#define BASE_IDX 64
static uint64_t g_buf[160];

/* byte offsets to test (as written into the 8-bit field) */
static const int offs[] = { 0, 8, 16, 64, 120, 128, 136, 192, 200, 240, 248, 256, 264 };
#define NOFF ((int)(sizeof(offs)/sizeof(offs[0])))

static void fire(void) {
    register uint64_t *_b asm("rcx") = &g_buf[BASE_IDX];
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","memory","cc");
}

int main(void) {
    printf("=== probe_offset: map 8-bit offset field ===\n");
    printf("g_buf=%p base=&g_buf[%d]=%p\n\n",
           (void*)g_buf, BASE_IDX, (void*)&g_buf[BASE_IDX]);
    if ((uint64_t)g_buf >= 0x100000000ULL) { printf("FATAL >4GB\n"); return 1; }
    assign_to_core(0);

    /* Build patch: for each offset, ZEROEXT RAX=marker (own triad), STAD RAX->[base+off]. */
    ucode_t patch[2*NOFF + 1];
    int n = 0;
    for (int k = 0; k < NOFF; k++) {
        patch[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(RAX, 100+k), NOP, NOP, NOP_SEQWORD };
        patch[n++] = (ucode_t){ STAD_DSZ64_ASZ32_SC1_RRI(RAX, RCX, offs[k], SEG), NOP, NOP, NOP_SEQWORD };
    }
    patch[n-1].seqw = END_SEQWORD;

    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, n);

    memset(g_buf, 0, sizeof(g_buf));
    fire();

    /* Scan: find each marker, report landing index and decode. */
    printf("%-8s %-10s %-12s %-12s %s\n", "off", "landed@", "unsigned_exp", "signed_exp", "verdict");
    for (int k = 0; k < NOFF; k++) {
        int marker = 100 + k;
        int found = -1;
        for (int i = 0; i < 160; i++) if ((int)g_buf[i] == marker) { found = i; break; }
        int8_t soff = (int8_t)(offs[k] & 0xff);
        int unsigned_idx = BASE_IDX + (offs[k] & 0xff) / 8;
        int signed_idx   = BASE_IDX + soff / 8;
        const char *verdict = "?";
        if (found < 0) verdict = "LOST";
        else if (found == unsigned_idx && found == signed_idx) verdict = "(both)";
        else if (found == unsigned_idx) verdict = "UNSIGNED";
        else if (found == signed_idx)   verdict = "SIGNED";
        else verdict = "other";
        printf("%-8d %-10d %-12d %-12d %s\n",
               offs[k], found, unsigned_idx, signed_idx, verdict);
    }

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
