/*
 * mac128.c — Microcode-accelerated 128-bit Multiply-Accumulate for Goldmont
 *
 * Implements:  acc_hi:acc_lo += src1 * src2   (64×64→128 MAC)
 *
 * Target: Intel Goldmont (Celeron N3350) with red-unlock
 * Hook:   RDRAND (for testing) — replace with UD2/chosen opcode for production
 *
 * Register convention (caller ↔ microcode):
 *   INPUT:   RCX = src1,  RDX = src2
 *            RAX = acc_lo, R8 = acc_hi
 *   OUTPUT:  RAX = acc_lo (updated), R8 = acc_hi (updated)
 *   CLOBBERED: RCX, RDX (src consumed)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  CRITICAL ASSUMPTION — MUST VALIDATE ON HARDWARE FIRST
 * ═══════════════════════════════════════════════════════════════════
 *
 *  MUL_DSZ64_DRR(TMP0, src0, src1) produces a 128-bit result:
 *    - Low  64 bits → TMP0
 *    - High 64 bits → TMP1  (paired register: TMP0+1)
 *
 *  This register-pair convention is the standard model from
 *  Goldmont microcode reverse engineering, but MUST be validated
 *  empirically with CustomProcessingUnit before deploying.
 *
 *  Run validate_mul_pair() below to confirm this on your NUC.
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"


/* ──────────────────────────────────────────────────────────────────
 *  MAC128 microcode patch — 3 triads
 *
 *  Triad 0: Multiply, begin low accumulate
 *    uop0: TMP0 = RCX * RDX           (low 64 bits of product)
 *           [implicit: TMP1 = high 64 bits via register pair]
 *    uop1: RAX  = RAX + TMP0          (acc_lo += prod_lo, sets CF)
 *    uop2: NOP                         (can't read CF in same triad)
 *    seq:  NOP_SEQWORD                 (continue to triad 1)
 *
 *  Triad 1: Capture carry, accumulate high part
 *    uop0: TMP2 = SETCC(CF)           (TMP2 = carry flag = 0 or 1)
 *    uop1: R8   = R8  + TMP1          (acc_hi += prod_hi)
 *    uop2: NOP
 *    seq:  NOP_SEQWORD                 (continue to triad 2)
 *
 *  Triad 2: Add carry into high accumulator, terminate
 *    uop0: R8   = R8  + TMP2          (acc_hi += carry)
 *    uop1: NOP
 *    uop2: NOP
 *    seq:  END_SEQWORD
 *
 *  Total: 3 triads, 5 active uops
 *
 *  Replaces x86 sequence (6 instructions):
 *    mov rax, [mem]     ; load src into implicit rax
 *    mul rXX            ; rdx:rax = rax * rXX
 *    add acc_lo, rax    ; accumulate low
 *    adc acc_hi, rdx    ; accumulate high + carry
 *    mov rYY, rax       ; spill (if needed)
 *    mov rZZ, rdx       ; spill (if needed)
 * ────────────────────────────────────────────────────────────────── */

void install_mac128(void) {
        ucode_t mac128_patch[] = {

                /* ── Triad 0: Widening multiply + low accumulate ──────── */
                {
                        MUL_DSZ64_DRR(R64SRC,R64SRC,R64DST),   /* TMP0 = lo64(RCX*RDX)      */
                                                           /* TMP1 = hi64(RCX*RDX) [!]  */
                        ADD_DSZ64_DRR(TMP0, RAX, TMP0),    /* acc_lo += prod_lo (CF set) */
                        NOP,                               /* can't consume CF this triad*/
                        NOP_SEQWORD
                },

                /* ── Triad 1: Extract carry, accumulate high ──────────── */
                {
                        SETCC_CONDB_DR(TMP2, TMP2),       /* TMP2 = CF (0 or 1)        */
                                                           /* CONDB = "below" = carry    */
                        ADD_DSZ64_DRR(R8, R8, TMP1),      /* acc_hi += prod_hi          */
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 2: Fold carry into high, done ──────────────── */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP2),      /* acc_hi += carry            */
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();

        /* Patch into ucode RAM */
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));

        /*
         * Hook RDRAND's microcode entry point → our patch
         * For production: hook UD2 or an unused opcode instead.
         * RDRAND entry = 0x0428, patch target = 0x7c00
         */
        hook_match_and_patch(0, 0x0428, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  VALIDATION TEST — run this FIRST to confirm register pair behavior
 *
 *  Tests: 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001
 *  With acc initialized to 0, result should be:
 *    RAX = 0xFFFFFFFE00000001 (low)
 *    R8  = 0x0000000000000000 (high, for 32-bit test values)
 *
 *  For full 64-bit test:
 *    0xFFFFFFFFFFFFFFFF * 2 = 0x1_FFFFFFFFFFFFFFFE
 *    acc = 0 → RAX=0xFFFFFFFFFFFFFFFE, R8=0x1
 * ══════════════════════════════════════════════════════════════════ */

void validate_mul_pair(void) {

        /* ── Test 1: Simple multiply, no accumulate ─────────────── */
        /*
         * Install a simpler patch first that JUST does the multiply
         * and moves both halves to architectural regs for inspection.
         */
        ucode_t mul_test_patch[] = {
                /* Triad 0: Multiply, move results out */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),   /* TMP0 = lo, TMP1 = hi [?] */
                        NOP,         /* RAX = low 64 bits        */
                        NOP,         /* RBX = high 64 bits [!]   */
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mul_test_patch, ARRAY_SZ(mul_test_patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        /*
         * Test: 0xFFFFFFFFFFFFFFFF × 0x2 = 0x1_FFFFFFFFFFFFFFFE
         *   Expected: RAX = 0xFFFFFFFFFFFFFFFE
         *             RBX = 0x0000000000000001
         */
        uint64_t rdx_out, rcx_out;
        asm volatile(
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 0x2\n\t"
                "vmwrite rcx, rdx\n\t"        /* triggers our hooked microcode */
                "mov %0, rcx\n\t"
                "mov %1, rdx\n\t"
                : "=r"(rcx_out), "=r"(rdx_out)
                :
                : "rax", "rbx", "rcx", "rdx"
        );

        printf("=== MUL PAIR VALIDATION ===\n");
        printf("0xFFFFFFFFFFFFFFFF * 0x2:\n");
        printf("  RCX (low)  = 0x%016" PRIx64 "  (expect 0xFFFFFFFFFFFFFFFE)\n", rcx_out);
        printf("  RDX (high) = 0x%016" PRIx64 "  (expect 0x0000000000000001)\n", rdx_out);

        /*if (rax_out == 0xFFFFFFFFFFFFFFFEULL && rbx_out == 0x1ULL) {
                printf("  ✓ PASS — Register pair TMP0:TMP1 confirmed!\n\n");
        } else if (rax_out == 0xFFFFFFFFFFFFFFFEULL && rbx_out != 0x1ULL) {
                printf("  ✗ FAIL — Low part correct but high part wrong.\n");
                printf("           TMP1 does NOT hold the high multiply result.\n");
                printf("           Try TMP0+2 or check URAM for spill location.\n\n");
        } else {
                printf("  ✗ FAIL — Unexpected results. Check MUL_DSZ64 behavior.\n\n");
        }*/

        /*
         * Test 2: Larger product to verify full 128-bit range
         * 0x8000000000000000 × 0x8000000000000000 = 0x40000000_00000000_00000000_00000000
         *   Expected: RAX = 0x0, RBX = 0x4000000000000000
         */
        asm volatile(
                "mov rcx, 0x8000000000000000\n\t"
                "mov rdx, rcx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rcx\n\t"
                "mov %1, rdx\n\t"
                : "=r"(rcx_out), "=r"(rdx_out)
                :
                : "rax", "rbx", "rcx", "rdx"
        );

        printf("0x8000000000000000 * 0x8000000000000000:\n");
        printf("  RCX (low)  = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", rcx_out);
        printf("  RDX (high) = 0x%016" PRIx64 "  (expect 0x4000000000000000)\n", rdx_out);

        /*if (rax_out == 0x0ULL && rbx_out == 0x4000000000000000ULL) {
                printf("  ✓ PASS — Full 128-bit multiply confirmed!\n\n");
        } else {
                printf("  ✗ FAIL — Check register pair convention.\n\n");
        }*/
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 INTEGRATION TEST
 *
 *  Verifies the full multiply-accumulate operation:
 *    acc = 100, then acc += 7 * 13 → acc should be 191
 *    acc = 0, then acc += 0xFFFFFFFFFFFFFFFF * 0xFFFFFFFFFFFFFFFF
 *      → acc should be 0xFFFFFFFFFFFFFFFE_0000000000000001
 * ══════════════════════════════════════════════════════════════════ */

void test_mac128(void) {

        install_mac128();

        /* Test 1: Small values, no overflow into high */
        uint64_t rax_out, r8_out;
        asm volatile(
                "mov rax, 100\n\t"      /* acc_lo = 100              */
                "mov r8,  0\n\t"        /* acc_hi = 0                */
                "mov rcx, 7\n\t"        /* src1 = 7                  */
                "mov rdx, 13\n\t"       /* src2 = 13                 */
                "rdrand rax\n\t"        /* triggers MAC128 microcode */
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );

        printf("=== MAC128 TEST ===\n");
        printf("Test 1: acc=100, acc += 7*13:\n");
        printf("  RAX (lo) = %" PRIu64 "  (expect 191)\n", rax_out);
        printf("  R8  (hi) = %" PRIu64 "  (expect 0)\n", r8_out);
        printf("  %s\n\n", (rax_out == 191 && r8_out == 0) ? "✓ PASS" : "✗ FAIL");


        /* Test 2: Max values — full 128-bit product */
        asm volatile(
                "mov rax, 0\n\t"                        /* acc_lo = 0 */
                "mov r8,  0\n\t"                        /* acc_hi = 0 */
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"       /* src1 = max */
                "mov rdx, 0xFFFFFFFFFFFFFFFF\n\t"       /* src2 = max */
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );

        printf("Test 2: acc=0, acc += 0xFFFF..F * 0xFFFF..F:\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000001)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0xFFFFFFFFFFFFFFFE)\n", r8_out);
        printf("  %s\n\n",
               (rax_out == 0x1ULL && r8_out == 0xFFFFFFFFFFFFFFFEULL)
               ? "✓ PASS" : "✗ FAIL");


        /* Test 3: Accumulate with carry propagation
         * acc = 0xFFFFFFFFFFFFFFFF:0xFFFFFFFFFFFFFFFF (max 128-bit)
         * acc += 1 * 2  → should wrap around:
         *   low:  0xFFFFFFFFFFFFFFFF + 2 = 0x0000000000000001 (carry 1)
         *   high: 0xFFFFFFFFFFFFFFFF + 0 + 1 = 0x0000000000000000
         */
        asm volatile(
                "mov rax, 0xFFFFFFFFFFFFFFFF\n\t"       /* acc_lo = max */
                "mov r8,  0xFFFFFFFFFFFFFFFF\n\t"       /* acc_hi = max */
                "mov rcx, 1\n\t"                        /* src1 = 1     */
                "mov rdx, 2\n\t"                        /* src2 = 2     */
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );

        printf("Test 3: acc=MAX128, acc += 1*2 (carry propagation):\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000001)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", r8_out);
        printf("  %s\n\n",
               (rax_out == 0x1ULL && r8_out == 0x0ULL)
               ? "✓ PASS — carry propagation works!" : "✗ FAIL");


        /* Test 4: Curve25519-relevant values
         * Simulating: arg1[2] * x5  where values are ~51-bit limbs
         * arg1[2] = 0x7FFFFFFFFFFFF (max 51-bit)
         * x5      = 0x26 * 0x7FFFFFFFFFFFF = 0x12FFFFFFFFFFFFF62
         * But x5 is pre-reduced, so let's use realistic limb values:
         *   src1 = 0x0006000000000000 (typical limb)
         *   src2 = 0x0004000000000000 (typical limb)
         *   product = 0x18000000000000000000000000000 — fits in 106 bits
         */
        uint64_t src1 = 0x0006000000000000ULL;
        uint64_t src2 = 0x0004000000000000ULL;
        /* Expected: 0x0006.. * 0x0004.. = 0x0000001800000000_0000000000000000 */
        /* But let's compute reference in C */
        __uint128_t ref = (__uint128_t)src1 * src2;
        uint64_t ref_lo = (uint64_t)ref;
        uint64_t ref_hi = (uint64_t)(ref >> 64);

        asm volatile(
                "mov rax, 0\n\t"
                "mov r8,  0\n\t"
                "mov rcx, %2\n\t"
                "mov rdx, %3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                : "r"(src1), "r"(src2)
                : "rax", "rcx", "rdx", "r8"
        );

        printf("Test 4: Curve25519 limb multiply:\n");
        printf("  0x%016" PRIx64 " * 0x%016" PRIx64 ":\n", src1, src2);
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")\n", rax_out, ref_lo);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")\n", r8_out, ref_hi);
        printf("  %s\n\n",
               (rax_out == ref_lo && r8_out == ref_hi)
               ? "✓ PASS" : "✗ FAIL");
}


/* ══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {

        printf("╔════════════════════════════════════════════╗\n");
        printf("║  MAC128 — Goldmont Microcode Accelerator   ║\n");
        printf("╚════════════════════════════════════════════╝\n\n");

                //printf("Running multiply pair validation first...\n\n");
                //validate_mul_pair();
                //return 0;
        

        printf("Running MAC128 tests...\n");
        printf("(Run with 'v' argument to validate MUL pair first)\n\n");
        test_mac128();

        return 0;
}


/*
 * ══════════════════════════════════════════════════════════════════
 *  BUILD & RUN
 * ══════════════════════════════════════════════════════════════════
 *
 *  Step 1 — Validate multiply pair assumption:
 *    $ sudo ./mac128 v
 *
 *  Step 2 — If pair validated, run MAC128 tests:
 *    $ sudo ./mac128
 *
 *  Step 3 — If SETCC_CONDB carry capture fails, try alternatives:
 *    - CMOVCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1) with TMP2 pre-zeroed
 *    - Read FLAGS via READAFLAGS_DR and mask CF bit
 *    - MOVEMERGEFLGS to capture flags in a register
 *
 * ══════════════════════════════════════════════════════════════════
 *  ALTERNATIVE: If TMP0+1 does NOT hold multiply high
 * ══════════════════════════════════════════════════════════════════
 *
 *  If the register pair assumption fails, try these approaches
 *  to locate the high multiply result:
 *
 *  1. Check TMP0+2 (some architectures skip a register)
 *  2. Read from URAM — the multiply unit may store high bits
 *     in a fixed URAM location
 *  3. Use MOVEFROMCREG to read a control register that holds
 *     the multiply result
 *  4. Trace with CustomProcessingUnit to see which internal
 *     registers change after MUL_DSZ64
 *
 * ══════════════════════════════════════════════════════════════════
 *  DEPLOYING IN CURVE25519: EXAMPLE USAGE
 * ══════════════════════════════════════════════════════════════════
 *
 *  Once validated, use in the carry_square hot loop like this:
 *
 *  // Initialize accumulator for limb 0
 *  // acc = 0
 *  xor rax, rax        ; acc_lo = 0
 *  xor r8, r8          ; acc_hi = 0
 *
 *  // MAC128: acc += arg1[1] * x2
 *  mov rcx, [rsi+0x08] ; src1 = arg1[1]
 *  mov rdx, r10        ; src2 = x2
 *  rdrand rax           ; ← triggers MAC128 microcode
 *
 *  // MAC128: acc += arg1[2] * x5
 *  mov rcx, [rsi+0x10] ; src1 = arg1[2]
 *  mov rdx, r9         ; src2 = x5
 *  rdrand rax           ; ← triggers MAC128 microcode
 *
 *  // MAC128: acc += arg1[0] * arg1[0]  (self-square)
 *  mov rcx, [rsi+0x00] ; src1 = arg1[0]
 *  mov rdx, rcx        ; src2 = arg1[0]
 *  rdrand rax           ; ← triggers MAC128 microcode
 *
 *  // Now RAX:R8 holds the full limb 0 accumulator (x24)
 *  // Proceed to carry extraction...
 *
 *  Each RDRAND replaces: mov rax,[mem] + mul + add + adc + spill + spill
 *  (6 instructions → 1 instruction + 3 microcode triads)
 *
 * ══════════════════════════════════════════════════════════════════
 */
