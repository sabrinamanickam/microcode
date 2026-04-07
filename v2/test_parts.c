/*
 * test_single_limb.c  — crash-hardened revision
 *
 * Changes from previous version:
 *   1. All input/output buffers are STATIC GLOBALS (guaranteed < 4GB)
 *   2. Address printed and validated before any LDZX executes
 *   3. Part 1 (probe) caps at 96 triads max — was 500, which likely
 *      overflowed patch RAM and corrupted microcode ROM
 *   4. Part 2 (single-limb) separated into its own function;
 *      comment out call to isolate if Part 1 still crashes
 *   5. Explicit barrier after patch write before rdrand
 *
 * BUILD:  make PROG=test_single_limb
 * RUN:    sudo ./test_single_limb_static
 *
 * If NUC still crashes on Part 1 alone, reduce MAX_PROBE_SIZE further
 * (try 32, then 16) to find the actual patch RAM limit safely.
 *
 * WORKING_SEG: override with -DWORKING_SEG=0x3 etc.
 *              Default tries a sweep.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include "../../include/misc.h"
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"

/* ══════════════════════════════════════════════════════════════════
 * STATIC GLOBALS — all live below 4GB, safe for ASZ32 loads
 * ══════════════════════════════════════════════════════════════════ */

/* Part 1 probe: a known magic value for LDZX to read */
static volatile uint64_t g_probe_val __attribute__((aligned(8)))
    = 0xDEADBEEFCAFE1234ULL;

/* Part 2 input: 5 limbs of a Curve25519 field element */
static volatile uint64_t g_limbs[5] __attribute__((aligned(64)));

/* Part 2 output: lo and hi of raw limb-0 accumulator */
static volatile uint64_t g_result_lo __attribute__((aligned(8)));
static volatile uint64_t g_result_hi __attribute__((aligned(8)));

/* ══════════════════════════════════════════════════════════════════
 * Address sanity check — call before any LDZX
 * ══════════════════════════════════════════════════════════════════ */
static int check_addr32(const char *name, const volatile void *p)
{
    uint64_t addr = (uint64_t)p;
    int ok = (addr <= 0xFFFFFFFFULL);
    printf("  %-16s @ 0x%016" PRIx64 "  %s\n",
           name, addr, ok ? "OK (<4GB)" : "FAIL (>4GB, LDZX ASZ32 will fault!)");
    return ok;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 1: Probe patch RAM capacity
 *
 * Tries increasing triad counts. Stops at MAX_PROBE_SIZE.
 * If the NUC crashes here, reduce MAX_PROBE_SIZE.
 * Safe starting point: 96. Try 32 if 96 crashes.
 * ══════════════════════════════════════════════════════════════════ */
#define MAX_PROBE_SIZE 32   /* <<< reduce to 32 if this crashes */

static void probe_patch_size(void)
{
    printf("=== PART 1: Patch RAM capacity probe (max %d triads) ===\n",
           MAX_PROBE_SIZE);

    /* Sizes to try — all <= MAX_PROBE_SIZE */
    int sizes[] = { 4, 8, 16, 24, 32, 48, 64, 80, 96 };
    int n = (int)(sizeof(sizes)/sizeof(sizes[0]));

    for (int t = 0; t < n; t++) {
        int count = sizes[t];
        if (count > MAX_PROBE_SIZE) break;

        /* Build a patch of 'count' triads.
         * All are NOPs except the last, which is END. */
        ucode_t *patch = calloc(count, sizeof(ucode_t));
        if (!patch) { printf("  OOM at %d\n", count); break; }

        for (int i = 0; i < count - 1; i++) {
            patch[i] = (ucode_t){ NOP, NOP, NOP, NOP_SEQWORD };
        }
        /* Last triad: move RAX→RAX (identity) + END */
        patch[count-1] = (ucode_t){
            MOVE_DSZ64_DR(RAX, RAX),
            NOP,
            NOP,
            END_SEQWORD
        };

        int patch_ok = do_patch(patch, count);
        free(patch);

        if (!patch_ok) {
            printf("  %3d triads: do_patch FAILED (patch RAM limit exceeded)\n", count);
            printf("  → Max safe patch size is < %d triads\n", count);
            break;
        }

        /* Execute the patch: rdrand rax */
        uint64_t rax_out = 0;
        __asm__ volatile(
            "mfence\n\t"
            "rdrand %%rax\n\t"
            "mov %%rax, %[out]"
            : [out] "=r"(rax_out)
            :
            : "rax"
        );

        printf("  %3d triads: patch applied + executed OK  (rax=0x%016" PRIx64 ")\n",
               count, rax_out);
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════
 * PART 1b: Segment sweep — find working LDZX segment value
 *
 * Loads g_probe_val via LDZX into RAX; correct seg → magic value.
 * ══════════════════════════════════════════════════════════════════ */
static int probe_seg_sweep(void)
{
    printf("=== PART 1b: LDZX segment sweep ===\n");
    if (!check_addr32("g_probe_val", &g_probe_val)) {
        printf("  SKIP: address > 4GB, ASZ32 cannot work here.\n\n");
        return -1;
    }

    int segs[] = { 0, 1, 2, 3, 4, 5, 6, 7,
                   0x10, 0x18, 0x1a, 0x1b, 0x1c, 0x1e, 0x1f };
    int n = (int)(sizeof(segs)/sizeof(segs[0]));
    int found = -1;

    for (int i = 0; i < n; i++) {
        int seg = segs[i];

        ucode_t patch[] = {
            {
                LDZX_DSZ64_ASZ32_SC1_DR(TMP0, RCX, seg),
                NOP,
                NOP,
                NOP_SEQWORD
            },
            {
                MOVE_DSZ64_DR(RAX, TMP0),
                NOP,
                NOP,
                END_SEQWORD
            }
        };
        do_patch(patch, 2);

        uint64_t rax_out = 0;
        uint64_t addr = (uint64_t)&g_probe_val;
        __asm__ volatile(
            "mov %[a], %%rcx\n\t"
            "mfence\n\t"
            "rdrand %%rax\n\t"
            "mov %%rax, %[out]"
            : [out] "=r"(rax_out)
            : [a] "r"(addr)
            : "rax", "rcx"
        );

        int ok = (rax_out == 0xDEADBEEFCAFE1234ULL);
        printf("  seg=0x%02x: RAX=0x%016" PRIx64 "  %s\n",
               seg, rax_out, ok ? "✓ MATCH" : "");
        if (ok && found < 0) found = seg;
    }

    if (found >= 0)
        printf("  → Working segment: 0x%02x\n", found);
    else
        printf("  → No working segment found — LDZX may not be functional\n");
    printf("\n");
    return found;
}

/* ══════════════════════════════════════════════════════════════════
 * MAC128 emit helper
 *
 * Appends 6 triads to patch[*idx].
 * Before: RAX=acc_lo, R8=acc_hi, RCX=a, RDX=b
 * After:  RAX=new_acc_lo, R8=new_acc_hi
 * Clobbers: TMP0..TMP5, RDX
 *
 * MUL_DSZ64_DRR(dst_hi, src_lo, src_hi) where after execution:
 *   dst_hi = upper 64 bits of product
 *   RDX    = lower 64 bits of product  (confirmed from previous work)
 * ══════════════════════════════════════════════════════════════════ */
static void emit_mac128(ucode_t *patch, int *idx)
{
    int i = *idx;

    /* Triad 0: save old acc_lo; MUL RCX*RDX → TMP0(hi), RDX(lo) */
    patch[i++] = (ucode_t){
        MOVE_DSZ64_DR(TMP3, RAX),
        MUL_DSZ64_DRR(TMP0, RCX, RDX),
        NOP,
        NOP_SEQWORD
    };

    /* Triad 1: new_acc_lo = old_acc_lo + prod_lo (in RDX)
     *          carry inputs: AND, OR of operands */
    patch[i++] = (ucode_t){
        ADD_DSZ64_DRR(RAX, TMP3, RDX),
        AND_DSZ64_DRR(TMP1, TMP3, RDX),
        OR_DSZ64_DRR(TMP2, TMP3, RDX),
        NOP_SEQWORD
    };

    /* Triad 2: propagated carry term; acc_hi += prod_hi */
    patch[i++] = (ucode_t){
        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
        ADD_DSZ64_DRR(R8, R8, TMP0),
        NOP,
        NOP_SEQWORD
    };

    /* Triad 3: merge carry bits */
    patch[i++] = (ucode_t){
        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* Triad 4: extract carry (bit 63 → bit 0) */
    patch[i++] = (ucode_t){
        SHR_DSZ64_DRI(TMP5, TMP5, 63),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* Triad 5: fold carry into acc_hi */
    patch[i++] = (ucode_t){
        ADD_DSZ64_DRR(R8, R8, TMP5),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    *idx = i;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 2: Single-limb carry_square — limb 0 only
 *
 * limb0_raw = a[0]² + a[1]*(a[4]*38) + a[2]*(a[3]*38)
 *
 * Note: carry reduction is NOT done in microcode.
 *       Microcode stores raw 128-bit accumulator.
 *       x86 caller handles the carry chain.
 *
 * Register plan:
 *   RCX = base pointer to g_limbs[] (caller sets this before rdrand)
 *   RAX = acc_lo (init 0)
 *   R8  = acc_hi (init 0)
 *   RDX = scratch / MUL low-half output
 *   RBP = constant 38
 *   TMP0..TMP5 = MAC128 internals
 *   TMP6, TMP7 = loaded limb temporaries
 *
 * Sequence (27 triads):
 *   T0:  LDZX a[0] → TMP6
 *   T1:  LDZX a[1] → TMP7
 *   T2:  LDZX a[4] → RDX; MOVE RCX=TMP6 (a[0]); MOVE RAX=0
 *   T3:  MOVE R8=0; MOVE RBP=38
 *   T4:  MOVE RDX=TMP7; MUL TMP6*TMP6 (a[0]²) — but we need RCX=RDX=a[0]
 *        Actually: set RCX=a[0], RDX=a[0], then MAC128
 *   ...
 *
 * Simplified: after loads, for a[0]²:
 *   RCX = a[0], RDX = a[0]  → MAC128
 * For a[1]*a[4]*38:
 *   RCX = a[1], RDX = a[4]*38 (precomputed with IMUL) → MAC128
 *   (we use x86 IMUL since microcode IMUL may not be available;
 *    alternatively use MUL + shift: 38 = 32+4+2 = but simplest is
 *    dedicated multiply: RDX = TMP7 * 38 in microcode via
 *    MOVE RCX=38; MUL; then restore RCX=a[1])
 *
 * Concrete triad layout below.
 * ══════════════════════════════════════════════════════════════════ */

/* C reference for validation */
static void c_limb0(const uint64_t *a, uint64_t *lo, uint64_t *hi)
{
    __uint128_t acc = 0;
    __uint128_t t;

    t = (__uint128_t)a[0] * a[0];  acc += t;
    t = (__uint128_t)a[1] * (a[4] * 38ULL);  acc += t;
    t = (__uint128_t)a[2] * (a[3] * 38ULL);  acc += t;

    *lo = (uint64_t)(acc & 0xFFFFFFFFFFFFFFFFULL);
    *hi = (uint64_t)(acc >> 64);
}

static void test_single_limb(int seg)
{
    printf("=== PART 2: Single-limb limb-0 in microcode (seg=0x%02x) ===\n", seg);

    /* Address validation first — crash prevention */
    int ok = 1;
    ok &= check_addr32("g_limbs",     g_limbs);
    ok &= check_addr32("g_result_lo", &g_result_lo);
    ok &= check_addr32("g_result_hi", &g_result_hi);
    if (!ok) {
        printf("  ABORT: buffers not in low 4GB. Cannot use LDZX ASZ32.\n\n");
        return;
    }
    printf("\n");

    /* Test vectors */
    static const uint64_t vecs[5][5] = {
        { 1, 0, 0, 0, 0 },
        { 1, 1, 1, 1, 1 },
        { 0x1FFFFFFFFFFFFFULL, 0x1FFFFFFFFFFFFFULL, 0x1FFFFFFFFFFFFFULL,
          0x1FFFFFFFFFFFFFULL, 0x1FFFFFFFFFFFFFULL },
        { 0x123456789ABCDEFULL, 0xFEDCBA987654321ULL, 0x111111111111111ULL,
          0xAAAAAAAAAAAAAAAULL, 0x555555555555555ULL },
        { 0, 0, 0, 0, 1 },
    };

    /*
     * Microcode triad layout (each LDZX uses offset encoding):
     *
     * LDZX_DSZ64_ASZ32_SC1_SIB_DR(dst, base, index, scale, seg)
     * — but for simple [base + imm8] we use the displacement form.
     * The exact macro depends on your inst.h — adjust if needed.
     *
     * Triad budget for limb 0:
     *   4 loads (a[0]..a[3] — a[4] handled via offset)   = 4
     *   1 load a[4]                                        = 1
     *   MOVE/init                                          = 3
     *   3 × MAC128                                         = 18
     *   STAD lo                                            = 1
     *   STAD hi + END                                      = 1
     *   Total                                              = 28 triads
     *
     * Register assignments after load phase:
     *   TMP6 = a[0]
     *   TMP7 = a[1]
     *   TMP8 = a[2]   (if available; else reload from memory)
     *   TMP9 = a[3]
     *   TMP10= a[4]
     *   RAX  = acc_lo (0)
     *   R8   = acc_hi (0)
     *   RBP  = 38
     */

    /* Build the patch */
    ucode_t patch[64];
    memset(patch, 0, sizeof(patch));
    int idx = 0;

    /* ── Load phase ──────────────────────────────────────────────── */

    /* T0: Load a[0] (offset 0) into TMP6 */
    patch[idx++] = (ucode_t){
        LDZX_DSZ64_ASZ32_SC1_DR(TMP6, RCX, seg),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* T1: Load a[1] (offset 8) into TMP7
     * Use RCX+8: needs displacement variant.
     * If LDZX_DSZ64_ASZ32_SC1_DISP8_DR exists, use it.
     * Otherwise: ADD RCX,8 → load → SUB RCX,8  (3 triads, expensive)
     * Using displacement form here — adjust macro name to match inst.h */
    patch[idx++] = (ucode_t){
        LDZX_DSZ64_ASZ32_SC1_DISP8_DR(TMP7, RCX, 8, seg),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* T2: Load a[2] (offset 16) */
    patch[idx++] = (ucode_t){
        LDZX_DSZ64_ASZ32_SC1_DISP8_DR(TMP8, RCX, 16, seg),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* T3: Load a[3] (offset 24), Load a[4] (offset 32) */
    patch[idx++] = (ucode_t){
        LDZX_DSZ64_ASZ32_SC1_DISP8_DR(TMP9,  RCX, 24, seg),
        LDZX_DSZ64_ASZ32_SC1_DISP8_DR(TMP10, RCX, 32, seg),
        NOP,
        NOP_SEQWORD
    };

    /* ── Init accumulators ───────────────────────────────────────── */

    /* T4: RAX=0, R8=0 */
    patch[idx++] = (ucode_t){
        XOR_DSZ64_DRR(RAX, RAX, RAX),
        XOR_DSZ64_DRR(R8,  R8,  R8),
        NOP,
        NOP_SEQWORD
    };

    /* T5: RBP = 38 (constant for ×38 multiplications) */
    patch[idx++] = (ucode_t){
        MOVE_DSZ64_DRI(RBP, 38),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* ── MAC 1: acc += a[0] * a[0] ──────────────────────────────── */
    /* T6: RCX = a[0], RDX = a[0] */
    patch[idx++] = (ucode_t){
        MOVE_DSZ64_DR(RCX, TMP6),
        MOVE_DSZ64_DR(RDX, TMP6),
        NOP,
        NOP_SEQWORD
    };
    emit_mac128(patch, &idx);   /* T7..T12 */

    /* ── MAC 2: acc += a[1] * (a[4] * 38) ──────────────────────── */
    /* T13: RCX = a[4]; RDX = 38 → multiply to get a[4]*38 */
    patch[idx++] = (ucode_t){
        MOVE_DSZ64_DR(RCX, TMP10),
        MOVE_DSZ64_DR(RDX, RBP),
        NOP,
        NOP_SEQWORD
    };
    /* T14: TMP0_scratch = a[4]*38 hi, RDX = a[4]*38 lo
     *      Then RCX = a[1], so we need a temp for the product lo */
    /* Use a nested MUL: MUL RCX*RDX → TMP11(hi), RDX(lo).
     * Then RCX=a[1], RDX stays = a[4]*38 lo.
     * (We only need lo since a[4]*38 fits in 64 bits for 25-bit limbs) */
    patch[idx++] = (ucode_t){
        MUL_DSZ64_DRR(TMP11, RCX, RDX),  /* TMP11=hi(ignored), RDX=lo=a[4]*38 */
        MOVE_DSZ64_DR(RCX, TMP7),         /* RCX = a[1] */
        NOP,
        NOP_SEQWORD
    };
    emit_mac128(patch, &idx);   /* T15..T20 */

    /* ── MAC 3: acc += a[2] * (a[3] * 38) ──────────────────────── */
    /* T21: RCX = a[3]; RDX = 38 */
    patch[idx++] = (ucode_t){
        MOVE_DSZ64_DR(RCX, TMP9),
        MOVE_DSZ64_DR(RDX, RBP),
        NOP,
        NOP_SEQWORD
    };
    /* T22: MUL a[3]*38, then RCX=a[2] */
    patch[idx++] = (ucode_t){
        MUL_DSZ64_DRR(TMP11, RCX, RDX),  /* RDX = a[3]*38 lo */
        MOVE_DSZ64_DR(RCX, TMP8),         /* RCX = a[2] */
        NOP,
        NOP_SEQWORD
    };
    emit_mac128(patch, &idx);   /* T23..T28 */

    /* ── Store results ───────────────────────────────────────────── */

    /* Need addresses of g_result_lo and g_result_hi.
     * Strategy: load them into registers before rdrand executes,
     * passed in via RDI = &g_result_lo  (8 bytes apart).
     * STAD [RDI+0] = RAX (lo), STAD [RDI+8] = R8 (hi)           */

    /* T29: STAD acc_lo → [RDI] */
    patch[idx++] = (ucode_t){
        STAD_DSZ64_ASZ32_SC1_DR(RAX, RDI, seg),
        NOP,
        NOP,
        NOP_SEQWORD
    };

    /* T30: STAD acc_hi → [RDI+8]; END */
    patch[idx++] = (ucode_t){
        STAD_DSZ64_ASZ32_SC1_DISP8_DR(R8, RDI, 8, seg),
        NOP,
        NOP,
        END_SEQWORD
    };

    int total = idx;
    printf("  Patch size: %d triads\n\n", total);

    /* Apply patch once */
    do_patch(patch, total);

    /* Run 5 test vectors */
    int pass = 0;
    for (int v = 0; v < 5; v++) {
        /* Load limbs into static buffer */
        for (int j = 0; j < 5; j++)
            g_limbs[j] = vecs[v][j];

        g_result_lo = 0xDEADDEADDEADDEADULL;
        g_result_hi = 0xDEADDEADDEADDEADULL;

        uint64_t limb_addr  = (uint64_t)g_limbs;
        uint64_t result_addr = (uint64_t)&g_result_lo;

        __asm__ volatile(
            "mov %[la], %%rcx\n\t"   /* RCX = &g_limbs[0] */
            "mov %[ra], %%rdi\n\t"   /* RDI = &g_result_lo */
            "mfence\n\t"
            "rdrand %%rax\n\t"
            :
            : [la] "r"(limb_addr), [ra] "r"(result_addr)
            : "rax", "rcx", "rdi", "rdx", "r8",
              "rbp", "r9", "r10", "r11"
        );

        uint64_t got_lo = g_result_lo;
        uint64_t got_hi = g_result_hi;

        uint64_t exp_lo, exp_hi;
        const uint64_t *a = vecs[v];
        c_limb0(a, &exp_lo, &exp_hi);

        int vok = (got_lo == exp_lo && got_hi == exp_hi);
        pass += vok;

        printf("  vec[%d]: %s\n", v, vok ? "PASS" : "FAIL");
        if (!vok) {
            printf("    got  lo=0x%016" PRIx64 " hi=0x%016" PRIx64 "\n", got_lo, got_hi);
            printf("    want lo=0x%016" PRIx64 " hi=0x%016" PRIx64 "\n", exp_lo, exp_hi);
        }
    }

    printf("\n  Result: %d/5 passed\n\n", pass);
}

/* ══════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("test_single_limb — crash-hardened\n");
    printf("===================================\n\n");

    /* Part 1: probe how many triads fit in patch RAM.
     * If the NUC crashes here, reduce MAX_PROBE_SIZE at top of file. */
    probe_patch_size();

    /* Part 1b: find working LDZX segment */
    int seg = probe_seg_sweep();
    if (seg < 0) {
        printf("No working segment found. Cannot run Part 2.\n");
        return 1;
    }

    /* Part 2: single-limb microcode.
     * Comment out this call to isolate Part 1 if needed. */
    //test_single_limb(seg);

    return 0;
}
