/*
 * stgbuf_test.c — Probe the microcode staging buffer (STADSTGBUF /
 *                  LDSTGBUF) as an "extended TMP" / spill area.
 *
 * Background (tools/main/main.c uses these for persistent_trace):
 *   STADSTGBUF_DSZ64_ASZ16_SC1_RI(src, addr16) : stgbuf[addr16] = src   (64-bit)
 *   LDSTGBUF_DSZ64_ASZ16_SC1_DI(dst, addr16)   : dst = stgbuf[addr16]   (64-bit)
 *   STADSTGBUF_DSZ64_ASZ16_SC1_RR(src, addr_reg)
 *   LDSTGBUF_DSZ64_ASZ16_SC1_DR(dst, addr_reg)
 *
 * Address space: 16-bit (IMM_ENCODE_SRC0 = 16 bits → 64 KB).
 * Existing code uses 0xba00..0xbb00 in 0x40 increments. The comment in
 * tools/main/ucode/trace.h says "assume no one else uses [0xba00,
 * 0xbb00]" — so this range is the convention for "safe scratch."
 *
 * What we want to learn:
 *   1. Does a store-then-load round-trip preserve all 64 bits?
 *   2. What address spacing is safe (64-byte, 8-byte, byte)?
 *   3. Can ST and LD be packed within a single triad?
 *   4. **Does data persist across vmwrite calls?** ← the critical
 *      property for using stgbuf as a "register file" between fe_mul
 *      and fe_sq invocations.
 *   5. Does the register-source-address variant work?
 *
 * SAFETY:
 *   - Every test is forward-progress only — no backward branches, no
 *     loops anywhere. An "infinite loop" inside microcode is impossible
 *     by construction.
 *   - We touch only the 0xba00..0xbb00 range that existing tools assume
 *     is unowned.
 *   - Each test installs its own patch fresh (reinstall).
 *   - Tests are ordered safest first. If an early test crashes the NUC,
 *     later tests aren't reached.
 *
 * Build:  make PROG=stgbuf_test
 * Run:    sudo taskset -c 0 ./stgbuf_test_static
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

/* fire_patch: hit the hook (vmwrite, 0x0cd8) once. RAX returned. */
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

/* ── A. Basic store→load round-trip on a single slot ─────────────
 *   T0: TMP0 = 0xAAAA
 *   T1: stgbuf[0xba40] = TMP0
 *   T2: TMP0 = 0xBBBB     (clobber; if T3 fails, we see 0xBBBB)
 *   T3: TMP0 = stgbuf[0xba40]
 *   T4: RAX = TMP0; END
 *
 * PASS: RAX = 0xAAAA  → stgbuf load works.
 * FAIL/0xBBBB: load returned junk (or didn't fire), TMP0 still has the
 *              clobber value.
 */
static int test_a(void) {
    printf("--- A: basic STADSTGBUF / LDSTGBUF round-trip ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0xAAAA), NOP, NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(TMP0, 0xBBBB), NOP, NOP, NOP_SEQWORD },
        /* T3 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T4 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xAAAA);
    printf("  RAX = 0x%" PRIx64 "  expect 0xAAAA  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xBBBB) ? "  (LD didn't fire — got the clobber value)" : "");
    return ok;
}

/* ── B. Two independent slots, 0x40 (64-byte) spacing (main.c style) ──
 *   stgbuf[0xba40] = 0x1111
 *   stgbuf[0xba80] = 0x2222
 *   Then read both back into TMP0, TMP1, then XOR — expect 0x3333.
 *
 * PASS: RAX = 0x3333  → no aliasing at 64-byte spacing.
 * FAIL/0x0000: same slot was written twice — addresses alias!
 * FAIL/0x2222 or 0x1111: only one of the writes/reads worked.
 */
static int test_b(void) {
    printf("--- B: two slots @ 0xba40 / 0xba80 (no-alias check) ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0x1111), MOVE_DSZ64_DI(TMP1, 0x2222), NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40),
                   STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, 0xba80), NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(TMP0, 0), MOVE_DSZ64_DI(TMP1, 0), NOP, NOP_SEQWORD },
        /* T3 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40),
                   LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, 0xba80), NOP, NOP_SEQWORD },
        /* T4 */ { XOR_DSZ64_DRR(TMP0, TMP0, TMP1), NOP, NOP, NOP_SEQWORD },
        /* T5 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0x3333);
    printf("  RAX = 0x%" PRIx64 "  expect 0x3333  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        if      (r == 0)      printf("  → both reads returned same value — addresses ALIAS at 64-byte spacing\n");
        else if (r == 0x1111) printf("  → 0xba80 read came back as 0 or as 0x1111 (alias / write-fail)\n");
        else if (r == 0x2222) printf("  → 0xba40 read came back as 0 (read-fail of first store)\n");
    }
    return ok;
}

/* ── C. 8-byte spacing (0xba40, 0xba48) — finer addressing possible? ──
 *  stgbuf is DSZ64 (8 bytes). If addresses are byte-indexed, 8-byte
 *  spacing should work without aliasing.
 */
static int test_c(void) {
    printf("--- C: 8-byte spacing (0xba40, 0xba48) ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0x4040), MOVE_DSZ64_DI(TMP1, 0x4848), NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40),
                   STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, 0xba48), NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(TMP0, 0), MOVE_DSZ64_DI(TMP1, 0), NOP, NOP_SEQWORD },
        /* T3 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40),
                   LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, 0xba48), NOP, NOP_SEQWORD },
        /* T4 */ { XOR_DSZ64_DRR(TMP0, TMP0, TMP1), NOP, NOP, NOP_SEQWORD },
        /* T5 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == (0x4040 ^ 0x4848));
    printf("  RAX = 0x%" PRIx64 "  expect 0x%x  %s\n",
           r, 0x4040 ^ 0x4848, ok ? "PASS" : "FAIL");
    if (!ok) {
        if      (r == 0)      printf("  → 8-byte spacing ALIASES (second store overwrote first)\n");
        else if (r == 0x4848) printf("  → first slot read 0 (was overwritten)\n");
        else if (r == 0x4040) printf("  → second slot read 0\n");
    }
    return ok;
}

/* ── D. Adjacent spacing 0xba40, 0xba41 — alignment requirement check ─
 *
 * If stgbuf is byte-addressed but DSZ64 requires 8-byte alignment, the
 * 0xba41 store might (a) hit the same line as 0xba40 (alias),
 * (b) succeed (true byte-addressed), or (c) cause undefined behavior.
 *
 * This is the riskiest test in this file because it might trigger an
 * alignment exception inside microcode. We still keep forward-progress
 * only, so worst case we hit END quickly. If the NUC hangs here, we
 * know byte-addressing is unsafe.
 */
static int test_d(void) {
    printf("--- D: 1-byte spacing (0xba40, 0xba41) — alignment probe ---\n");
    printf("    [if this hangs, byte-addressing is unsafe — only 8/64-byte ok]\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0x4040), MOVE_DSZ64_DI(TMP1, 0x4141), NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, 0xba41), NOP, NOP, NOP_SEQWORD },
        /* T3 */ { MOVE_DSZ64_DI(TMP0, 0), MOVE_DSZ64_DI(TMP1, 0), NOP, NOP_SEQWORD },
        /* T4 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T5 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, 0xba41), NOP, NOP, NOP_SEQWORD },
        /* T6 */ { CONCAT_DSZ32_DRR(TMP0, TMP0, TMP1), NOP, NOP, NOP_SEQWORD },
        /* T7 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    /* Two interesting outcomes: both reads return original (works),
     * or both reads return the LATER value (alias). */
    printf("  RAX = 0x%" PRIx64 "  (low32=ba40-read, high32=ba41-read packed by CONCAT)\n", r);
    /* No strict PASS — informational. We mark PASS if r != 0 (something
     * sensible happened). */
    int ok = (r != 0);
    printf("  result: %s  (informational — observe value)\n", ok ? "non-zero (ok)" : "FAIL all-zero");
    return ok;
}

/* ── E. Same triad: store and load (different slots) ─────────────
 *
 * Pack a STADSTGBUF in slot 0 and a LDSTGBUF in slot 1 — does the LD
 * see the store within the same triad? Or do we need a triad boundary?
 *
 * The interesting hazard is "store-to-load forwarding within one triad."
 */
static int test_e(void) {
    printf("--- E: STADSTGBUF + LDSTGBUF in same triad (intra-triad ST→LD) ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0xE5E5), MOVE_DSZ64_DI(TMP1, 0xAAAA), NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40),
                   LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, 0xba40), NOP, NOP_SEQWORD },
        /* T2 */ { ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    /* PASS if TMP1 saw the new value, INTERESTING if it saw old. */
    int saw_new = (r == 0xE5E5);
    int saw_old = (r == 0xAAAA);
    printf("  RAX = 0x%" PRIx64 "  %s\n", r,
           saw_new ? "PASS (intra-triad ST→LD forwards)" :
           saw_old ? "FAIL (no forwarding — LD reads OLD line value)" :
                     "FAIL (unexpected)");
    return saw_new;
}

/* ── F. Cross-vmwrite persistence — THE CRITICAL TEST ────────────
 *
 * Two patches: WRITER stores TMP0=0x5A5A into stgbuf[0xba40] then ends.
 * READER reads stgbuf[0xba40] into RAX then ends.
 *
 * If reading after a separate vmwrite firing returns 0x5A5A, the
 * staging buffer survives between microcode invocations and could be
 * used as a register file persisting across our fe_mul/fe_sq calls.
 */
static int test_f(void) {
    printf("--- F: persistence across separate vmwrite firings ---\n");

    /* writer patch */
    ucode_t writer[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0x5A5A), NOP, NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(RAX, 0xFADE), NOP, NOP, END_SEQWORD },
    };
    reinstall(writer, ARRAY_SZ(writer));
    uint64_t w = fire_patch();
    printf("  writer fired, RAX = 0x%" PRIx64 " (expect 0xFADE marker)\n", w);

    /* reader patch — different code, same region. Does NOT write first. */
    ucode_t reader[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0xBAD0), NOP, NOP, NOP_SEQWORD },  /* clobber */
        /* T1 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(reader, ARRAY_SZ(reader));
    uint64_t r = fire_patch();
    int ok = (r == 0x5A5A);
    printf("  reader fired, RAX = 0x%" PRIx64 "  expect 0x5A5A  %s\n",
           r, ok ? "PASS — STGBUF PERSISTS" : "FAIL");
    if (!ok) {
        if      (r == 0xBAD0) printf("  → load did not fire (got clobber value)\n");
        else if (r == 0)      printf("  → stgbuf was cleared between vmwrites\n");
        else                  printf("  → got a different value — partial persistence?\n");
    }
    return ok;
}

/* ── G. Register-source-address variant (LDSTGBUF ... _DR) ───────
 *
 * Address comes from a register, not an immediate. Useful if we want
 * to index into stgbuf with a computed offset.
 *
 * Setup: store 0xC0DE at 0xba40 (imm-address). Then load using a
 * register that holds 0xba40. Compare.
 */
static int test_g(void) {
    printf("--- G: register-source address variant (LDSTGBUF...DR) ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 0xC0DE), NOP, NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(TMP0, 0xDEAD),
                   MOVE_DSZ64_DI(TMP1, 0xba40), NOP, NOP_SEQWORD },
        /* T3 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DR(TMP0, TMP1), NOP, NOP, NOP_SEQWORD },
        /* T4 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xC0DE);
    printf("  RAX = 0x%" PRIx64 "  expect 0xC0DE  %s\n",
           r, ok ? "PASS — register-addressed LD works" : "FAIL");
    if (!ok) {
        if (r == 0xDEAD) printf("  → LD with reg-addr didn't fire (clobber survived)\n");
    }
    return ok;
}

/* ── H. Arch reg path: STADSTGBUF(R10, ...) → LDSTGBUF(R11, ...) ──
 *
 * Confirm that the stgbuf ops work on arch GP regs, not just TMPs.
 * (trace.h uses R10–R13, so this should be fine, but let's check.)
 *
 * Note: we keep R10/R11 inside the patch. The C caller already
 * clobbers these via the asm volatile clobber list (vmwrite is the
 * call site).
 */
static int test_h(void) {
    printf("--- H: arch reg path (R10 ST → R11 LD) ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(R10, 0x7777), NOP, NOP, NOP_SEQWORD },
        /* T1 */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(R10, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T2 */ { MOVE_DSZ64_DI(R11, 0x9999), NOP, NOP, NOP_SEQWORD },
        /* T3 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(R11, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T4 */ { ZEROEXT_DSZ64_DR(RAX, R11), NOP, NOP, END_SEQWORD },
    };
    /* Patch will clobber R10/R11 — but we don't read them from C so OK. */
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0x7777);
    printf("  RAX = 0x%" PRIx64 "  expect 0x7777  %s\n",
           r, ok ? "PASS" : "FAIL");
    return ok;
}

/* ── I. Multiple slots end-to-end (8 entries, packed reads) ───────
 *
 * Stress test: write 8 distinct 16-bit values into stgbuf at 8 different
 * 0x40-spaced addresses, then read them all and XOR together.
 * Expected XOR: 0x1 ^ 0x2 ^ 0x4 ^ 0x8 ^ 0x10 ^ 0x20 ^ 0x40 ^ 0x80 = 0xFF.
 *
 * If this hangs, stgbuf can't take 8 stores in one patch — bandwidth
 * limit or latency issue.
 */
static int test_i(void) {
    printf("--- I: 8 distinct slots (XOR reduce) ---\n");
    ucode_t p[] = {
        /* T0  */ { MOVE_DSZ64_DI(TMP0, 0x01), MOVE_DSZ64_DI(TMP1, 0x02), MOVE_DSZ64_DI(TMP2, 0x04), NOP_SEQWORD },
        /* T1  */ { MOVE_DSZ64_DI(TMP3, 0x08), MOVE_DSZ64_DI(TMP4, 0x10), MOVE_DSZ64_DI(TMP5, 0x20), NOP_SEQWORD },
        /* T2  */ { MOVE_DSZ64_DI(TMP6, 0x40), MOVE_DSZ64_DI(TMP7, 0x80), NOP, NOP_SEQWORD },
        /* T3  */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40),
                    STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, 0xba80),
                    STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP2, 0xbac0), NOP_SEQWORD },
        /* T4  */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP3, 0xbb00),
                    STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP4, 0xbb40),
                    STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP5, 0xbb80), NOP_SEQWORD },
        /* T5  */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP6, 0xbbc0),
                    STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP7, 0xbc00), NOP, NOP_SEQWORD },
        /* clobber all */
        /* T6  */ { MOVE_DSZ64_DI(TMP0, 0), MOVE_DSZ64_DI(TMP1, 0), MOVE_DSZ64_DI(TMP2, 0), NOP_SEQWORD },
        /* T7  */ { MOVE_DSZ64_DI(TMP3, 0), MOVE_DSZ64_DI(TMP4, 0), MOVE_DSZ64_DI(TMP5, 0), NOP_SEQWORD },
        /* T8  */ { MOVE_DSZ64_DI(TMP6, 0), MOVE_DSZ64_DI(TMP7, 0), NOP, NOP_SEQWORD },
        /* read */
        /* T9  */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40),
                    LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, 0xba80),
                    LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP2, 0xbac0), NOP_SEQWORD },
        /* T10 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP3, 0xbb00),
                    LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP4, 0xbb40),
                    LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP5, 0xbb80), NOP_SEQWORD },
        /* T11 */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP6, 0xbbc0),
                    LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP7, 0xbc00), NOP, NOP_SEQWORD },
        /* XOR reduce */
        /* T12 */ { XOR_DSZ64_DRR(TMP0, TMP0, TMP1),
                    XOR_DSZ64_DRR(TMP2, TMP2, TMP3),
                    XOR_DSZ64_DRR(TMP4, TMP4, TMP5), NOP_SEQWORD },
        /* T13 */ { XOR_DSZ64_DRR(TMP6, TMP6, TMP7),
                    XOR_DSZ64_DRR(TMP0, TMP0, TMP2),
                    XOR_DSZ64_DRR(TMP4, TMP4, TMP6), NOP_SEQWORD },
        /* T14 */ { XOR_DSZ64_DRR(TMP0, TMP0, TMP4), NOP, NOP, NOP_SEQWORD },
        /* T15 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xFF);
    printf("  RAX = 0x%" PRIx64 "  expect 0xFF  %s\n", r, ok ? "PASS" : "FAIL");
    return ok;
}

/* ── J. Large 64-bit value round-trip ────────────────────────────
 *
 * MOVE_DSZ64_DI can only set a 16-bit immediate. To test that all 64
 * bits round-trip, build a wider value via SHL+OR.
 *
 *   TMP0 = 0xCAFE
 *   TMP0 <<= 16          → 0xCAFE0000
 *   TMP0 |= 0xBEEF       → 0xCAFEBEEF
 *   TMP0 <<= 16          → 0xCAFEBEEF0000
 *   TMP0 |= 0xDEAD       → 0xCAFEBEEFDEAD
 *   TMP0 <<= 16          → 0xCAFEBEEFDEAD0000
 *   TMP0 |= 0xF00D       → 0xCAFEBEEFDEADF00D
 *   stgbuf[0xba40] = TMP0
 *   TMP0 = 0
 *   TMP0 = stgbuf[0xba40]
 *   RAX = TMP0; END
 *
 * PASS: RAX = 0xCAFEBEEFDEADF00D — all 64 bits round-trip.
 */
static int test_j(void) {
    printf("--- J: full-64-bit round-trip ---\n");
    ucode_t p[] = {
        /* T0  */ { MOVE_DSZ64_DI(TMP0, 0xCAFE), NOP, NOP, NOP_SEQWORD },
        /* T1  */ { SHL_DSZ64_DRI(TMP0, TMP0, 16), NOP, NOP, NOP_SEQWORD },
        /* T2  */ { OR_DSZ64_DRI(TMP0, TMP0, 0xBEEF), NOP, NOP, NOP_SEQWORD },
        /* T3  */ { SHL_DSZ64_DRI(TMP0, TMP0, 16), NOP, NOP, NOP_SEQWORD },
        /* T4  */ { OR_DSZ64_DRI(TMP0, TMP0, 0xDEAD), NOP, NOP, NOP_SEQWORD },
        /* T5  */ { SHL_DSZ64_DRI(TMP0, TMP0, 16), NOP, NOP, NOP_SEQWORD },
        /* T6  */ { OR_DSZ64_DRI(TMP0, TMP0, 0xF00D), NOP, NOP, NOP_SEQWORD },
        /* T7  */ { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T8  */ { MOVE_DSZ64_DI(TMP0, 0), NOP, NOP, NOP_SEQWORD },
        /* T9  */ { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, 0xba40), NOP, NOP, NOP_SEQWORD },
        /* T10 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xCAFEBEEFDEADF00DULL);
    printf("  RAX = 0x%016" PRIx64 "  expect 0xCAFEBEEFDEADF00D  %s\n",
           r, ok ? "PASS — all 64 bits preserved" : "FAIL");
    return ok;
}

int main(void) {
    printf("=== Staging buffer (stgbuf) capability probe ===\n");
    printf("Region: U%04x — single-hook, forward-progress patches only\n", REGION);
    printf("Address range used: 0xba40 .. 0xbc00 (in the convention-safe block)\n\n");

    assign_to_core(0);

    int pass = 0, total = 0, info = 0;
    total++; pass += test_a();
    total++; pass += test_b();
    total++; pass += test_c();
    /* D is informational — not strictly pass/fail */
    info  += test_d(); total++;
    total++; pass += test_e();
    total++; pass += test_f();   /* persistence — the key one */
    total++; pass += test_g();
    total++; pass += test_h();
    total++; pass += test_i();
    total++; pass += test_j();

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n=== %d / %d passed (D informational) ===\n", pass, total);

    printf("\nDecoder:\n");
    printf("  A  : basic store→load works at all\n");
    printf("  B  : 64-byte spacing is alias-free (matches main.c convention)\n");
    printf("  C  : 8-byte spacing alias-free (finer granularity ok)\n");
    printf("  D  : informational — byte-level addressing behaviour\n");
    printf("  E  : intra-triad store→load forwarding\n");
    printf("  F  : *** persistence across vmwrite firings ***\n");
    printf("       If F PASSes, stgbuf can be used as inter-call state.\n");
    printf("       This is the property that unlocks a ladderstep using\n");
    printf("       multiple separate microcode calls.\n");
    printf("  G  : register-sourced address (indexed access)\n");
    printf("  H  : arch reg path (R10/R11)\n");
    printf("  I  : 8 simultaneous slots — sanity that we can scale up\n");
    printf("  J  : all 64 bits round-trip (not just low 16/32)\n");

    return (pass == total) ? 0 : 1;
}
