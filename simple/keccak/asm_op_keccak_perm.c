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

static int verify(void) {
    int fails=0;

    /* 1. all-zero state: official Keccak-f[1600](0) -> lane0 = 0xF1258F7940E1DDE7 */
    uint64_t z[25]={0};
    fails += check("zero-state", z);
    /* independent KAT anchor: recompute ref for zero and confirm the known value */
    uint64_t zr[25]={0}; keccak_perm_ref(zr);
    printf("  KAT anchor lane0 = %016" PRIx64 " (expect F1258F7940E1DDE7) %s\n",
           zr[0], zr[0]==0xF1258F7940E1DDE7ULL ? "OK" : "REF-WRONG");

    /* 2. non-trivial state */
    uint64_t a[25];
    for (int i=0;i<25;i++) a[i]=0x0123456789ABCDEFULL*(i+1) ^ 0xDEADBEEFCAFEBABEULL;
    fails += check("nontrivial", a);

    /* 3. a few random states */
    uint64_t seed=0x12345;
    for (int t=0;t<3;t++){
        uint64_t r[25];
        for (int i=0;i<25;i++){ seed=seed*6364136223846793005ULL+1; r[i]=seed; }
        char lbl[24]; snprintf(lbl,sizeof(lbl),"random[%d]",t);
        fails += check(lbl, r);
    }
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
    printf("baseline x86_64_asm: 939 cyc.  %s\n",
           min/BATCH < 939 ? "*** WIN ***" : "(loss)");
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
