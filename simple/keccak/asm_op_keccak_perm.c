/*
 * asm_op_keccak_perm.c — full Keccak-f[1600] (24 rounds) in microcode, looped
 * inside ONE vmwrite. Phase 4.
 *
 * The round body loops 24x via a backward UJMPCC CONDNZ (count-up counter in the
 * buffer); RC[round] is looked up from an in-buffer table via index-register
 * LDZX. State stays resident in 13 GPR + 12 TMP across all rounds; only the
 * prologue (load) and epilogue (store) touch the 25 state lanes in memory.
 *
 * Buffer (g_keccak_buf[KECCAK_BUFLEN]), base RCX = &buf[KECCAK_BASE_LANE]:
 *   [0..24]  state      [25..29] D scratch   [30] counter   [31] theta-D scratch
 *   [32..55] RC[0..23] table
 * The wrapper resets counter=0 and the RC table before each fire.
 *
 * Build: make PROG=asm_op_keccak_perm
 * Run:   sudo taskset -c 0 ./asm_op_keccak_perm_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

#include "keccak_perm.h"   /* KECCAK_PERM_TRIADS, BUFLEN, BASE_LANE, COUNTER/RCTAB lanes */
static uint64_t g_keccak_buf[KECCAK_BUFLEN];

/* ── C reference ─────────────────────────────────────────────────── */
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
static inline uint64_t rol64(uint64_t v, int n){ n&=63; return n?((v<<n)|(v>>(64-n))):v; }

static void keccak_round_ref(uint64_t s[25], uint64_t RC) {
    uint64_t C[5], D[5], B[25];
    for (int x=0;x<5;x++) C[x]=s[x]^s[x+5]^s[x+10]^s[x+15]^s[x+20];
    for (int x=0;x<5;x++) D[x]=C[(x+4)%5]^rol64(C[(x+1)%5],1);
    for (int y=0;y<5;y++) for(int x=0;x<5;x++)
        B[y+5*((2*x+3*y)%5)]=rol64(s[x+5*y]^D[x], RHO[x+5*y]);
    for (int y=0;y<5;y++) for(int x=0;x<5;x++)
        s[x+5*y]=B[x+5*y]^(~B[(x+1)%5+5*y]&B[(x+2)%5+5*y]);
    s[0]^=RC;
}
static void keccak_perm_ref(uint64_t s[25]) {
    for (int r=0;r<24;r++) keccak_round_ref(s, KECCAK_RC[r]);
}

/* ── install + fire ──────────────────────────────────────────────── */
static void install_perm_patch(void) {
    ucode_t patch[] = {
        #include "keccak_perm_body.h"
    };
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, KECCAK_PERM_TRIADS);
    printf("Keccak perm patch installed: %d triads at U7c00\n", KECCAK_PERM_TRIADS);
}

/* Reset the per-fire control state: counter=0, RC table. (State set separately.) */
static void reset_control(void) {
    g_keccak_buf[KECCAK_COUNTER_LANE] = (KECCAK_RCTAB_LANE-KECCAK_BASE_LANE)*8;
    for (int r = 0; r < 24; r++) g_keccak_buf[KECCAK_RCTAB_LANE + r] = KECCAK_RC[r];
}

/* Run the full 24-round permutation in place on g_keccak_buf[0..24]. */
static void keccak_perm_ucode(void) {
    register uint64_t *_buf asm("rcx") = &g_keccak_buf[KECCAK_BASE_LANE];
    asm volatile(
        "vmwrite rcx, rcx\n\t"
        : "+r"(_buf)
        :
        : "rax","rbx","rdx","rdi","rsi","rbp",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"
    );
}

/* ── verification ────────────────────────────────────────────────── */
static int check(const char *label, uint64_t in[25]) {
    uint64_t ref[25];
    memcpy(ref, in, sizeof(ref));
    keccak_perm_ref(ref);

    memcpy(g_keccak_buf, in, 25*8);
    reset_control();
    keccak_perm_ucode();

    int fails=0;
    for (int i=0;i<25;i++) if (g_keccak_buf[i]!=ref[i]) {
        if (fails<4) printf("  [%2d] ucode=%016" PRIx64 " ref=%016" PRIx64 " ***\n",
                            i, g_keccak_buf[i], ref[i]);
        fails++;
    }
    printf("%-22s %s (%d/25)\n", label, fails?"FAIL":"PASS", 25-fails);
    return fails;
}

/* ── independent KAT vectors ─────────────────────────────────────────
 * Full 25-lane outputs of Keccak-f[1600], matching the Keccak team's published
 * intermediate values (KeccakF-1600-IntermediateValues.txt). These anchor the
 * microcode against fixed expected states *without* going through the in-process
 * C reference — so a correlated bug in keccak_perm_ref AND the patch can't pass.
 *   KAT_F0   = f(0)     (zero input; lane0 is the canonical 0xF1258F7940E1DDE7)
 *   KAT_F0F0 = f(f(0))  (its INPUT, KAT_F0, is fully non-zero — a non-trivial KAT) */
static const uint64_t KAT_F0[25] = {
    0xF1258F7940E1DDE7ULL, 0x84D5CCF933C0478AULL, 0xD598261EA65AA9EEULL, 0xBD1547306F80494DULL, 0x8B284E056253D057ULL,
    0xFF97A42D7F8E6FD4ULL, 0x90FEE5A0A44647C4ULL, 0x8C5BDA0CD6192E76ULL, 0xAD30A6F71B19059CULL, 0x30935AB7D08FFC64ULL,
    0xEB5AA93F2317D635ULL, 0xA9A6E6260D712103ULL, 0x81A57C16DBCF555FULL, 0x43B831CD0347C826ULL, 0x01F22F1A11A5569FULL,
    0x05E5635A21D9AE61ULL, 0x64BEFEF28CC970F2ULL, 0x613670957BC46611ULL, 0xB87C5A554FD00ECBULL, 0x8C3EE88A1CCF32C8ULL,
    0x940C7922AE3A2614ULL, 0x1841F924A2C509E4ULL, 0x16F53526E70465C2ULL, 0x75F644E97F30A13BULL, 0xEAF1FF7B5CECA249ULL,
};
static const uint64_t KAT_F0F0[25] = {
    0x2D5C954DF96ECB3CULL, 0x6A332CD07057B56DULL, 0x093D8D1270D76B6CULL, 0x8A20D9B25569D094ULL, 0x4F9C4F99E5E7F156ULL,
    0xF957B9A2DA65FB38ULL, 0x85773DAE1275AF0DULL, 0xFAF4F247C3D810F7ULL, 0x1F1B9EE6F79A8759ULL, 0xE4FECC0FEE98B425ULL,
    0x68CE61B6B9CE68A1ULL, 0xDEEA66C4BA8F974FULL, 0x33C43D836EAFB1F5ULL, 0xE00654042719DBD9ULL, 0x7CF8A9F009831265ULL,
    0xFD5449A6BF174743ULL, 0x97DDAD33D8994B40ULL, 0x48EAD5FC5D0BE774ULL, 0xE3B8C8EE55B7B03CULL, 0x91A0226E649E42E9ULL,
    0x900E3129E7BADD7BULL, 0x202A9EC5FAA3CCE8ULL, 0x5B3402464E1C3DB6ULL, 0x609F4E62A44C1059ULL, 0x20D06CD26A8FBF5CULL,
};

/* Run the microcode on `in`, compare all 25 lanes to the embedded expected
 * vector `exp` (NOT to keccak_perm_ref). Returns the count of mismatched lanes. */
static int kat_check(const char *label, const uint64_t in[25], const uint64_t exp[25]) {
    memcpy(g_keccak_buf, in, 25*8);
    reset_control();
    keccak_perm_ucode();
    int fails=0;
    for (int i=0;i<25;i++) if (g_keccak_buf[i]!=exp[i]) {
        if (fails<4) printf("  [%2d] ucode=%016" PRIx64 " expect=%016" PRIx64 " ***\n",
                            i, g_keccak_buf[i], exp[i]);
        fails++;
    }
    printf("%-26s %s (%d/25)\n", label, fails?"FAIL":"PASS", 25-fails);
    return fails;
}

/* Independent KAT pass: zero input -> full f(0) vector, and a NON-ZERO input
 * (f(0)) -> f(f(0)). Neither uses the C reference. */
static int verify_kat(void) {
    int fails=0;
    uint64_t z[25]={0};
    fails += kat_check("KAT f(0)      [zero in]   ", z,      KAT_F0);
    fails += kat_check("KAT f(f(0))   [nonzero in]", KAT_F0, KAT_F0F0);
    return fails;
}

/* Random states compared against the C reference, on hardware. */
#define NRANDOM 1000
#define STR_(x) #x
#define STR(x)  STR_(x)

static int verify(void) {
    int fails=0;

    /* 0. independent published KAT: full 25-lane f(0) and the non-zero-input
     * f(f(0)), checked against fixed constants (not the C reference). */
    fails += verify_kat();

    /* 1. all-zero state vs the C reference (lane-by-lane). The KAT above already
     * anchors f(0)'s 25 lanes to the published 0xF1258F7940E1DDE7 vector. */
    uint64_t z[25]={0};
    fails += check("zero-state", z);

    /* 2. non-trivial state */
    uint64_t a[25];
    for (int i=0;i<25;i++) a[i]=0x0123456789ABCDEFULL*(i+1) ^ 0xDEADBEEFCAFEBABEULL;
    fails += check("nontrivial", a);

    /* 3. differential test against the C reference on many random states.
     * This is the tier the paper cites, so it runs on the INSTALLED PATCH rather
     * than in simulation; 1000 permutations cost a few milliseconds. Only
     * failures are printed per trial (check() prints its own lane diffs), with a
     * single summary line for the pass. */
    uint64_t seed=0x12345;
    int rnd_fail_states=0;
    for (int t=0;t<NRANDOM;t++){
        uint64_t r[25], ref[25];
        for (int i=0;i<25;i++){ seed=seed*6364136223846793005ULL+1; r[i]=seed; }
        memcpy(ref, r, sizeof(ref));
        keccak_perm_ref(ref);
        memcpy(g_keccak_buf, r, 25*8);
        reset_control();
        keccak_perm_ucode();
        int bad=0;
        for (int i=0;i<25;i++) if (g_keccak_buf[i]!=ref[i]) bad++;
        if (bad){
            rnd_fail_states++;
            if (rnd_fail_states<=4){
                char lbl[24]; snprintf(lbl,sizeof(lbl),"random[%d]",t);
                printf("  %s: %d/25 lanes wrong\n", lbl, bad);
            }
            fails += bad;
        }
    }
    printf("%-22s %s (%d/%d states)\n", "random x" STR(NRANDOM),
           rnd_fail_states?"FAIL":"PASS", NRANDOM-rnd_fail_states, NRANDOM);
    return fails;
}

/* ── timing ──────────────────────────────────────────────────────── */
static inline uint64_t rdtsc_start(void){uint32_t lo,hi;asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");return((uint64_t)hi<<32)|lo;}
static inline uint64_t rdtsc_end(void){uint32_t lo,hi;asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");return((uint64_t)hi<<32)|lo;}
#define BATCH 1000
#define REPS  100

static void bench(void) {
    for (int i=0;i<25;i++) g_keccak_buf[i]=0x0123456789ABCDEFULL*(i+1);
    uint64_t min=UINT64_MAX,sum=0;
    for (int r=0;r<REPS;r++){
        uint64_t t0=rdtsc_start();
        for (int i=0;i<BATCH;i++){ g_keccak_buf[KECCAK_COUNTER_LANE]=(KECCAK_RCTAB_LANE-KECCAK_BASE_LANE)*8; keccak_perm_ucode(); }
        uint64_t t1=rdtsc_end();
        uint64_t dt=t1-t0; sum+=dt; if(dt<min)min=dt;
    }
    /* Note: reset_control's RC-table fill is hoisted out; only counter is reset
     * in-loop (RC table is overwritten by nothing, stays valid). */
    printf("\n--- full permutation (24 rounds, %d triads) ---\n", KECCAK_PERM_TRIADS);
    printf("min/perm %5" PRIu64 "  avg/perm %5" PRIu64 " cycles\n", min/BATCH, sum/REPS/BATCH);
    /* NOTE: this standalone number is rdtsc-UNCALIBRATED — rdtsc ticks at a
     * constant ~1.1GHz reference rate, not the core clock, so it only equals true
     * cycles when the core is pinned to base, and it is NOT comparable to a
     * baseline captured at a different P-state. Do not read a win/loss here.
     * The valid, frequency-invariant comparison times microcode vs the SUPERCOP
     * scalars back-to-back in ONE process: bench/asm_op_keccak_vs.c
     * (microcode 1910 vs fastest scalar 2047 cyc/perm = 0.933x, microcode WINS). */
    printf("(rdtsc-uncalibrated; for the valid 0.93x win see bench/asm_op_keccak_vs.c)\n");
}

/* Diagnostic: vary the number of loop iterations by presetting the counter
 * (counter_init = 24-N runs N rounds). Correctness is violated (wrong RCs) but
 * timing is valid. Linear fit -> per-round slope + fixed (I/O+dispatch+branch). */
static void bench_sweep(void) {
    printf("\n--- round-count sweep (counter preset; timing only) ---\n");
    int Ns[] = { 1, 2, 4, 8, 12, 24 };
    for (size_t k=0;k<sizeof(Ns)/sizeof(Ns[0]);k++) {
        int N = Ns[k];
        /* counter is now a BYTE-INDEX; to run N rounds start N*8 before the end. */
        int c0 = (KECCAK_RCTAB_LANE-KECCAK_BASE_LANE)*8 + (24-N)*8;
        uint64_t min=UINT64_MAX;
        for (int r=0;r<REPS;r++){
            for (int i=0;i<25;i++) g_keccak_buf[i]=0x0123456789ABCDEFULL*(i+1);
            uint64_t t0=rdtsc_start();
            for (int i=0;i<BATCH;i++){ g_keccak_buf[KECCAK_COUNTER_LANE]=c0; keccak_perm_ucode(); }
            uint64_t t1=rdtsc_end();
            uint64_t dt=t1-t0; if(dt<min)min=dt;
        }
        printf("  N=%2d rounds: %5" PRIu64 " cyc  (%4" PRIu64 " cyc/round)\n",
               N, min/BATCH, (min/BATCH)/(uint64_t)N);
    }
    printf("  (slope between rows = true cyc/round in the loop; intercept = fixed I/O+dispatch)\n");
}

int main(void) {
    printf("=== asm_op_keccak_perm Phase 4: looped 24-round permutation ===\n\n");
    printf("g_keccak_buf @ %p (below 4GB: %s)\n", (void*)g_keccak_buf,
           (uint64_t)g_keccak_buf < 0x100000000ULL ? "YES":"NO");
    if ((uint64_t)g_keccak_buf >= 0x100000000ULL){ printf("FATAL >4GB\n"); return 1; }

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_perm_patch();
    reset_control();

    int fails = verify();
    if (!fails) { bench(); bench_sweep(); }

    init_match_and_patch();
    do_fix_IN_patch();
    printf(fails ? "\nPhase 4 FAILED.\n" : "\nPhase 4 OK.\n");
    return fails ? 1 : 0;
}
