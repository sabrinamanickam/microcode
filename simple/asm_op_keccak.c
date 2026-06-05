/*
 * asm_op_keccak.c — Keccak-f[1600] via microcode (vmwrite) vs SUPERCOP scalar
 *
 * Phase 3b (current): single Keccak round in microcode, verified against the
 * C reference. The round body (theta/rho/pi/chi/iota) is generated and
 * software-verified by keccak_gen.py and emitted into keccak_round.h.
 *
 *   - 25 lanes live in 13 GPR + 12 TMP (canonical layout, see keccak_gen.py).
 *   - RCX = base of g_keccak_buf for the whole patch.
 *   - g_keccak_buf[0..24] = state; [25..29] = C scratch; [30..34] = D scratch.
 *   - One vmwrite runs the whole round; theta spills C/D to the buffer, the
 *     apply+chi phases work in registers, results stored back to [0..24].
 *
 * SEG_DS (0x18) is the working segment on this box (probe_seg.c). -no-pie keeps
 * the buffer in low 4GB for LDZX ASZ32.
 *
 * Build: make PROG=asm_op_keccak
 * Run:   sudo taskset -c 0 ./asm_op_keccak_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* keccak_round.h provides KECCAK_ROUND_TRIADS / KECCAK_BUFLEN / KECCAK_BASE_LANE.
 * Buffer: 25 state lanes + 5 D-scratch lanes. Base RCX is CENTERED at lane
 * KECCAK_BASE_LANE so every LDZX/STAD offset stays within the 8-bit SIGNED
 * range (-128..+127 bytes). Must be < 4GB (-no-pie). */
#include "keccak_round.h"
static uint64_t g_keccak_buf[KECCAK_BUFLEN];

/* ── C reference: one Keccak round ───────────────────────────────── */

static const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const int RHO[25] = {
     0,  1, 62, 28, 27,  36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,  41, 45, 15, 21,  8,
    18,  2, 61, 56, 14,
};

static inline uint64_t rol64(uint64_t v, int n) {
    n &= 63;
    return n ? ((v << n) | (v >> (64 - n))) : v;
}

static void keccak_round_ref(uint64_t s[25], uint64_t RC) {
    uint64_t C[5], D[5], B[25];
    for (int x = 0; x < 5; x++)
        C[x] = s[x] ^ s[x+5] ^ s[x+10] ^ s[x+15] ^ s[x+20];
    for (int x = 0; x < 5; x++)
        D[x] = C[(x+4)%5] ^ rol64(C[(x+1)%5], 1);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
            B[y + 5*((2*x+3*y)%5)] = rol64(s[x+5*y] ^ D[x], RHO[x+5*y]);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
            s[x+5*y] = B[x+5*y] ^ (~B[(x+1)%5+5*y] & B[(x+2)%5+5*y]);
    s[0] ^= RC;
}

/* ── install + fire ──────────────────────────────────────────────── */

static void install_keccak_round_patch(void) {
    /* Local (auto) array: the epilogue base-reload uses the runtime address of
     * g_keccak_buf, which a static-const initializer can't hold. */
    ucode_t patch[] = {
        #include "keccak_round_body.h"
    };
    /* Hook before patch (bootstrap-reclaim order; harmless here as the patch
     * is well under 120 triads). */
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, KECCAK_ROUND_TRIADS);
    printf("Keccak round patch installed: %d triads at U7c00\n", KECCAK_ROUND_TRIADS);
}

/* Run one microcode round in place on g_keccak_buf[0..24].
 * RCX = CENTERED base = &g_keccak_buf[KECCAK_BASE_LANE]. */
static void keccak_round_ucode(void) {
    register uint64_t *_buf asm("rcx") = &g_keccak_buf[KECCAK_BASE_LANE];
    asm volatile(
        "vmwrite rcx, rcx\n\t"
        : "+r"(_buf)
        :
        : "rax", "rbx", "rdx", "rdi", "rsi", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* ── verification ────────────────────────────────────────────────── */

static int verify_round(void) {
    uint64_t ref[25], in[25];

    /* round-0 RC; non-trivial input. */
    for (int i = 0; i < 25; i++)
        in[i] = 0x0123456789ABCDEFULL * (i + 1) ^ 0xDEADBEEFCAFEBABEULL;

    memcpy(ref, in, sizeof(ref));
    keccak_round_ref(ref, KECCAK_RC[0]);

    memcpy(g_keccak_buf, in, sizeof(in));
    keccak_round_ucode();

    int fails = 0;
    for (int i = 0; i < 25; i++) {
        if (g_keccak_buf[i] != ref[i]) {
            fails++;
            printf("  lane[%2d]  ucode=%016" PRIx64 "  ref=%016" PRIx64 "  ***\n",
                   i, g_keccak_buf[i], ref[i]);
        }
    }
    if (fails == 0)
        printf("Phase 3b OK: microcode round == C reference (all 25 lanes).\n");
    else
        printf("Phase 3b FAIL: %d/25 lanes wrong.\n", fails);

    /* second vector: all-zero state -> round 0 -> state[0]=1, rest 0 */
    uint64_t z[25] = {0};
    keccak_round_ref(z, KECCAK_RC[0]);
    memset(g_keccak_buf, 0, 25*8);
    keccak_round_ucode();
    int z_ok = (memcmp(z, g_keccak_buf, 25*8) == 0);
    printf("Zero-state vector: %s\n", z_ok ? "PASS" : "FAIL");
    if (!z_ok) fails++;

    return fails;
}

/* ── timing (Phase 3c) ───────────────────────────────────────────── */
static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
#define BATCH 1000
#define REPS  100

static void bench_single_round(void) {
    for (int i = 0; i < 25; i++) g_keccak_buf[i] = 0x0123456789ABCDEFULL * (i+1);
    uint64_t min = UINT64_MAX, sum = 0;
    for (int r = 0; r < REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) keccak_round_ucode();
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("\n--- single-round microcode (incl. 50 I/O triads) ---\n");
    printf("min/round %4" PRIu64 "  avg/round %4" PRIu64 " cycles  (%d triads)\n",
           min/BATCH, sum/REPS/BATCH, KECCAK_ROUND_TRIADS);
    /* The 25 LD prologue + 25 ST epilogue (50 triads) are paid ONCE per
     * permutation in the looped design, not per round. Subtract to estimate
     * per-round compute cost, then project 24-round permutation. */
    (void)sum;
}

/* Single ISOLATED C round (GCC -O3): same load-25 + compute + store-25 as the
 * microcode round. This is the fair per-round comparison — both pay per-round
 * I/O. SUPERCOP's 39 cyc/round (939/24) avoids that I/O by unrolling all 24
 * rounds with state resident in registers across them. */
static uint64_t g_cstate[25];
static void __attribute__((noinline)) one_c_round(void) {
    keccak_round_ref(g_cstate, KECCAK_RC[0]);
    asm volatile("" :: "r"(g_cstate) : "memory");  /* defeat hoisting */
}
static void bench_c_round(void) {
    for (int i = 0; i < 25; i++) g_cstate[i] = 0x0123456789ABCDEFULL * (i+1);
    uint64_t min = UINT64_MAX;
    for (int r = 0; r < REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) one_c_round();
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0; if (dt < min) min = dt;
    }
    printf("--- single-round C reference (GCC -O3, isolated, incl. I/O) ---\n");
    printf("min/round %4" PRIu64 " cycles\n", min/BATCH);
}

static void bench_compare(void) {
    printf("\n=== PER-ROUND COMPARISON ===\n");
    bench_single_round();
    bench_c_round();
    printf("\nSUPERCOP x86_64_asm full perm = 939 cyc = %.1f cyc/round (24 rounds,\n", 939.0/24);
    printf("  UNROLLED — state stays in registers across all rounds, ZERO per-round I/O).\n");
    printf("The question: is one isolated round (C or ucode) ~the same? If so, the\n");
    printf("gap is per-round I/O / lack of cross-round register residency, not raw op speed.\n");
}

int main(void) {
    printf("=== asm_op_keccak Phase 3b/3c: single round ===\n\n");
    printf("g_keccak_buf @ %p  (below 4GB: %s)\n", (void*)g_keccak_buf,
           (uint64_t)g_keccak_buf < 0x100000000ULL ? "YES" : "NO");
    if ((uint64_t)g_keccak_buf >= 0x100000000ULL) {
        printf("FATAL: buffer above 4GB. Need -no-pie.\n");
        return 1;
    }

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_keccak_round_patch();

    int fails = verify_round();
    if (!fails) bench_compare();

    init_match_and_patch();
    do_fix_IN_patch();
    printf(fails ? "\nPhase 3b FAILED.\n" : "\nPhase 3b/3c OK.\n");
    return fails ? 1 : 0;
}
