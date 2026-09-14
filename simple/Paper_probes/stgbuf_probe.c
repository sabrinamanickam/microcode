/*
 * stgbuf_probe.c — Map the staging buffer: minimum reliable stride
 *                  and number of distinct usable slots.
 *
 * Two probes:
 *   PART 1: stride table — store distinct values at (0xba40, 0xba40+s)
 *           for s in {2, 4, 8, 16, 32, 48, 64} and check both reads.
 *           Tells us the smallest non-aliasing stride.
 *
 *   PART 2: address-space sweep at 0x40 stride. For each candidate
 *           addr in expanding rings around 0xba40, do a write/read
 *           round-trip with a distinct marker derived from the addr.
 *           Tells us how many slots we have and which addresses work.
 *           Rings are printed band-by-band so a hang stops at a known
 *           radius.
 *
 * SAFETY:
 *   - All patches are forward-progress (no backward branches, no loops)
 *   - Each probe iteration installs a fresh patch and fires once
 *   - PART 2 expands outward from the known-safe block. If it hangs
 *     at radius R, the previous band's results are already printed and
 *     known-safe.
 *   - Output is flushed after each probe so partial results survive a
 *     reboot.
 *
 * Build:  make PROG=stgbuf_probe
 * Run:    sudo taskset -c 0 ./stgbuf_probe_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION  0x7c00
#define T(n) (REGION + (n) * 4)

static uint64_t fire_patch(void) {
    uint64_t res;
    asm volatile(
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        : "=a"(res)
        :
        : "rcx", "rdx", "memory", "cc"
    );
    return res;
}

static void reinstall(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(REGION, p, n);
    hook_match_and_patch(0, 0x0cd8, REGION);
}

/* ─────────────────────────── PART 1 ─────────────────────────────
 *
 * Stride probe: write at base (0xba40) and base+stride. Read both
 * back. Report PASS if both reads return their distinct markers; FAIL
 * with mode-decode otherwise.
 *
 * Distinct markers: base gets 0xA000+stride, base+stride gets 0xB000+stride.
 * (So we can tell which read returned which value, even if they alias.)
 */
static int probe_stride(unsigned stride) {
    const uint16_t base = 0xba40;
    const uint16_t high = base + stride;
    const uint16_t mark_lo = 0xA000 | stride;   /* value at base */
    const uint16_t mark_hi = 0xB000 | stride;   /* value at base+stride */

    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, mark_lo), MOVE_DSZ64_DI(TMP1, mark_hi), NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, base),
                   STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, high), NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(TMP0, 0), MOVE_DSZ64_DI(TMP1, 0), NOP, NOP_SEQWORD },
        /* T3 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, base),
                   LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, high), NOP, NOP_SEQWORD },
        /* T4 */ { CONCAT_DSZ32_DRR(TMP0, TMP0, TMP1), NOP, NOP, NOP_SEQWORD },
        /* T5 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();

    uint32_t lo_read = (uint32_t)(r & 0xffffffff);
    uint32_t hi_read = (uint32_t)(r >> 32);

    /* Note: CONCAT_DSZ32_DRR(dst, a, b) packs as observed in earlier
     * test — `dst = (low32(a) << 32) | low32(b)`. So:
     *   lo_read corresponds to TMP1 (base+stride read)
     *   hi_read corresponds to TMP0 (base read)
     * (We can sanity-check this independently of the packing convention
     * by just looking at which of the two markers appears in which
     * half.)
     */

    int ok = (lo_read == mark_hi) && (hi_read == mark_lo);

    printf("  stride %2u (0x%04x ↔ 0x%04x): "
           "base→0x%08x, high→0x%08x  %s",
           stride, base, high, hi_read, lo_read, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (lo_read == 0 && hi_read == mark_lo)
            printf("  (high addr returned 0 — invalid/wraps)");
        else if (lo_read == hi_read)
            printf("  (both reads identical — aliasing)");
        else if (lo_read == mark_lo && hi_read == mark_lo)
            printf("  (both reads = mark_lo — high write missed)");
        else if (lo_read == mark_hi && hi_read == mark_hi)
            printf("  (both reads = mark_hi — low write was overwritten)");
        else
            printf("  (unexpected pattern)");
    }
    printf("\n");
    fflush(stdout);
    return ok;
}

/* ─────────────────────────── PART 2 ─────────────────────────────
 *
 * Address-space sweep. For each candidate address, install a write
 * patch and a read patch dynamically and check whether the value
 * survived. Patches are constructed with runtime-variable addresses
 * (the macros are all pure bit-math, so they work fine for variables).
 *
 * Each "ring" is a contiguous band of stride-0x40 addresses around
 * 0xba40. Rings get printed one at a time, so a hang stops at a known
 * boundary.
 */
static int probe_one_address(uint16_t addr) {
    uint64_t marker = ((uint64_t)addr) | ((uint64_t)(addr ^ 0xC0DE) << 16);
    /* marker has the addr embedded so we can tell true matches from
     * false coincidences. Low 16 bits = addr, next 16 = addr^0xC0DE. */

    /* Build value in TMP0 via MOVE+SHL+OR (MOVE_DI takes 16-bit imm). */
    uint16_t lo = (uint16_t)(marker & 0xffff);
    uint16_t hi = (uint16_t)((marker >> 16) & 0xffff);

    /* Write patch */
    ucode_t wr[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, hi), NOP, NOP, NOP_SEQWORD },
        /* T1 */ { SHL_DSZ64_DRI(TMP0, TMP0, 16), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { OR_DSZ64_DRI(TMP0, TMP0, lo), NOP, NOP, NOP_SEQWORD },
        /* T3 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, addr), NOP, NOP, NOP_SEQWORD },
        /* T4 */ { MOVE_DSZ64_DI(RAX, 0xCAFE), NOP, NOP, END_SEQWORD },
    };
    reinstall(wr, ARRAY_SZ(wr));
    (void)fire_patch();

    /* Read patch */
    ucode_t rd[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0), NOP, NOP, NOP_SEQWORD },
        /* T1 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, addr), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(rd, ARRAY_SZ(rd));
    uint64_t r = fire_patch();
    return (r == (marker & 0xffffffff));   /* Compare low 32 bits */
}

static int sweep_band(const char *label, uint32_t start, uint32_t end, uint32_t stride) {
    int total = 0, ok = 0;
    int first_bad = -1, last_good = -1;

    printf("\n  Ring %s: 0x%04x .. 0x%04x stride 0x%x\n", label, start, end, stride);
    fflush(stdout);

    /* Per-line print: 8 addresses per row, each "0x.. ✓" or "0x.. ✗" */
    int per_row = 8;
    int col = 0;
    for (uint32_t a = start; a < end; a += stride) {
        int pass = probe_one_address((uint16_t)a);
        total++;
        if (pass) { ok++; last_good = a; }
        else if (first_bad < 0) first_bad = a;
        printf("    0x%04x:%s%s",
               (uint16_t)a,
               pass ? "+ " : "- ",
               (++col % per_row == 0) ? "\n" : "");
        fflush(stdout);
    }
    if (col % per_row) printf("\n");
    printf("    ring summary: %d/%d ok", ok, total);
    if (first_bad >= 0) printf(", first bad = 0x%04x", first_bad);
    if (last_good >= 0) printf(", last good = 0x%04x", last_good);
    printf("\n");
    fflush(stdout);
    return ok;
}

int main(void) {
    printf("=== Staging buffer probe: stride + slot-count ===\n");
    printf("Region: U%04x — forward-progress patches only\n\n", REGION);
    fflush(stdout);

    assign_to_core(0);

    /* PART 1: STRIDE TABLE */
    printf("PART 1 — stride table (base = 0xba40):\n");
    fflush(stdout);
    int strides[] = {2, 4, 8, 16, 32, 48, 64};
    int n_strides = sizeof(strides) / sizeof(strides[0]);
    int stride_pass = 0;
    for (int i = 0; i < n_strides; i++) stride_pass += probe_stride(strides[i]);
    printf("  → %d/%d strides pass\n", stride_pass, n_strides);
    fflush(stdout);

    /* PART 2: ADDRESS SWEEP IN EXPANDING RINGS */
    printf("\nPART 2 — address sweep (expanding rings around 0xba40):\n");
    printf("  Each entry: 0x.. + (works) | 0x.. - (returned mismatching value)\n");
    printf("  Per-line FLUSH so a hang leaves printed state.\n");
    fflush(stdout);

    /* Ring 1: the known-safe block, sanity check. */
    sweep_band("1 (known-safe)", 0xba00, 0xbc00, 0x40);

    /* Ring 2: slightly wider, still in the "b" page. */
    sweep_band("2 (b-page)",      0xb000, 0xc000, 0x40);

    /* Ring 3: stretch into adjacent pages. */
    sweep_band("3 (wider)",       0x9000, 0xd000, 0x100);

    /* Ring 4: wide sweep, sparser (every 0x400 = 1024 bytes). */
    sweep_band("4 (sparse, full 16-bit)", 0x0000, 0x10000, 0x400);

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\nDone. If a ring is incomplete, the system hung mid-ring;\n");
    printf("the last printed line is the last known-safe address.\n");
    return 0;
}
