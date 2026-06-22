/*
 * asm_op_keccak_vs.c — Head-to-head, SAME PROCESS / SAME FREQUENCY:
 *   SUPERCOP x86_64_asm KeccakPermutation  vs.  microcode looped permutation.
 *
 * Motivation: rdtsc on Goldmont counts at the constant reference rate, not the
 * core clock. With burst (2.4GHz) vs base (1.1GHz), the SAME work reads as ~2.2x
 * fewer/more rdtsc ticks. Measuring the two implementations in separate runs is
 * invalid. Here we time both back-to-back, interleaved, after a warmup, so they
 * see the same frequency — the RATIO is then frequency-invariant.
 *
 * Build: make PROG=asm_op_keccak_vs
 * Run:   sudo taskset -c 0 ./asm_op_keccak_vs_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../../../../include/patch.h"
#include "../../../../include/ucode_macro.h"
#include "../../../../include/misc.h"

/* median over a sample of per-rep batch totals (sorts in place). */
static int cmp_u64(const void *a, const void *b){
    uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b;
    return (x>y)-(x<y);
}
static uint64_t median_u64(uint64_t *v, int n){
    qsort(v, n, sizeof(uint64_t), cmp_u64);
    return (n&1) ? v[n/2] : (v[n/2-1]+v[n/2])/2;
}
/* Robust min: the smallest sample that ISN'T an implausible downward glitch.
 * Cycle counts only grow under noise (interrupts, contention add time) — a
 * sample below half the median means the work was mis-measured for that batch
 * (e.g. a CPU P-state transition straddling the rdtsc bracket, which can make a
 * batch read near-zero). Skip those so one bad batch can't poison the min.
 * `sorted` must be ascending (median_u64 sorts it in place). */
static uint64_t robust_min(const uint64_t *sorted, int n, uint64_t med){
    uint64_t floor = med/2;
    for(int i=0;i<n;i++) if(sorted[i]>=floor) return sorted[i];
    return sorted[n-1];
}

#include "../keccak_perm.h"
static uint64_t g_keccak_buf[KECCAK_BUFLEN];

/* SUPERCOP Keccak permutations — benchmark vs EVERY keccakc1024 variant that
 * runs on this x86-64 Goldmont core (hand asm, 64-bit C, x86 SIMD) plus the
 * reference C, so we compare against the genuinely fastest, not a strawman.
 * Deliberately EXCLUDED (cannot run / not competitive on a 64-bit Intel core):
 *   - xopu24                : AMD XOP -> #UD (illegal instruction) on GenuineIntel
 *   - avr8* / *armv* / *rv* : other ISAs (won't build for x86-64)
 *   - inplace* / *32bi* / compact* : 32-bit bit-interleaved / size-optimised,
 *                             structurally slower on a 64-bit core
 *   - sphlib / sphlib-small : no cleanly-exposed bare permutation (sponge inline) */
extern void keccak_x86_64_asm_perm(uint64_t state[25]);        /* hand asm, ROL */
extern void keccak_x86_64_shld_perm(uint64_t state[25]);       /* hand asm, SHLD */
extern void keccak_opt64lcu24_perm(uint64_t state[25]);        /* 64-bit C, lane-compl + unroll 24 */
extern void keccak_opt64lcu24shld_perm(uint64_t state[25]);    /* 64-bit C, SHLD + unroll 24 */
extern void keccak_opt64lcu6_perm(uint64_t state[25]);         /* 64-bit C, lane-compl + unroll 6 */
extern void keccak_opt64u6_perm(uint64_t state[25]);           /* 64-bit C, plain + unroll 6 */
extern void keccak_sseu2_perm(uint64_t state[25]);             /* SSE2/SSSE3 SIMD */
extern void keccak_mmxu1_perm(uint64_t state[25]);             /* MMX SIMD */
extern void keccak_simple_F(uint64_t *state, const uint64_t *in, int laneCount); /* reference C */
/* simple's KeccakF absorbs `laneCount` lanes then permutes; laneCount=0 skips the
 * absorb (while(--0>=0) is false) -> a pure Keccak-f[1600] permutation. */
static void keccak_simple_perm(uint64_t s[25]){ keccak_simple_F(s, s, 0); }

/* XKCP (eXtended Keccak Code Package) single Keccak-p[1600], plain-64bits. On
 * Goldmont (no AVX2/AVX-512, and XKCP has no SSE single-perm kernel) this is the
 * only XKCP single permutation that runs, and what its recommended x86-64 target
 * falls back to. It's the current upstream of SUPERCOP's opt64lcu* family. */
extern void keccak_xkcp_g64_perm(uint64_t state[25]);    /* generic64   : full unroll, no lane-complement */
extern void keccak_xkcp_g64lc_perm(uint64_t state[25]);  /* generic64lc : full unroll, lane-complement (Goldmont default) */

/* OpenSSL keccak1600, x86-64 scalar assembly (CRYPTOGAMS/Polyakov) — what OpenSSL
 * runs on x86-64; no BMI/AVX, so it executes on Goldmont. Distinct hand-asm from
 * SUPERCOP's x86_64_asm (Van Keer). Takes the 25-lane state (A[5][5]) pointer. */
extern void keccak_openssl_perm(uint64_t state[25]);

static const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL,0x0000000000008082ULL,0x800000000000808aULL,0x8000000080008000ULL,
    0x000000000000808bULL,0x0000000080000001ULL,0x8000000080008081ULL,0x8000000000008009ULL,
    0x000000000000008aULL,0x0000000000000088ULL,0x0000000080008009ULL,0x000000008000000aULL,
    0x000000008000808bULL,0x800000000000008bULL,0x8000000000008089ULL,0x8000000000008003ULL,
    0x8000000000008002ULL,0x8000000000000080ULL,0x000000000000800aULL,0x800000008000000aULL,
    0x8000000080008081ULL,0x8000000000008080ULL,0x0000000080000001ULL,0x8000000080008008ULL,
};

static void install_perm_patch(void){
    ucode_t patch[] = {
        #include "../keccak_perm_body.h"
    };
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, KECCAK_PERM_TRIADS);
}
static void reset_control(void){
    g_keccak_buf[KECCAK_COUNTER_LANE]=(KECCAK_RCTAB_LANE-KECCAK_BASE_LANE)*8;
    for(int r=0;r<24;r++) g_keccak_buf[KECCAK_RCTAB_LANE+r]=KECCAK_RC[r];
}
static inline void ucode_perm(void){
    register uint64_t *_b asm("rcx") = &g_keccak_buf[KECCAK_BASE_LANE];
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

static inline uint64_t rdtsc_start(void){uint32_t lo,hi;asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");return((uint64_t)hi<<32)|lo;}
static inline uint64_t rdtsc_end(void){uint32_t lo,hi;asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");return((uint64_t)hi<<32)|lo;}
#define BATCH 1000
#define REPS  200

int main(void){
    printf("=== Keccak head-to-head (same process, same frequency) ===\n");
    printf("g_keccak_buf @ %p\n", (void*)g_keccak_buf);
    if((uint64_t)g_keccak_buf>=0x100000000ULL){printf("FATAL >4GB\n");return 1;}
    assign_to_core(0);
    init_match_and_patch(); do_fix_IN_patch();
    install_perm_patch(); reset_control();

    /* Correctness: the microcode perm is independently KAT-verified
     * (asm_op_keccak_perm.c, anchor 0xF1258F7940E1DDE7). SUPERCOP's x86_64_asm
     * uses the "bebigokimisa" complemented-lane representation, so its raw state
     * array won't match standard form — that's a representation difference, not
     * a correctness issue, and irrelevant for timing (identical 24-round work).
     * We sanity-check that BOTH actually permute (output != input). */
    uint64_t a[25], b[25];
    for(int i=0;i<25;i++){ a[i]=0x0123456789ABCDEFULL*(i+1); b[i]=a[i]; }
    keccak_x86_64_asm_perm(a);
    memcpy(g_keccak_buf,b,25*8); reset_control(); ucode_perm();
    int sc_perm = memcmp(a,b,25*8)!=0;
    int uc_perm = memcmp(g_keccak_buf,b,25*8)!=0;
    printf("sanity: SUPERCOP permutes=%d, microcode permutes=%d (raw arrays differ\n", sc_perm, uc_perm);
    printf("        by representation; microcode is KAT-verified elsewhere)\n");
    if(!sc_perm || !uc_perm){ init_match_and_patch(); do_fix_IN_patch(); return 1; }

    /* warmup to reach steady frequency */
    volatile uint64_t w=0; for(uint64_t i=0;i<200000000ULL;i++) w+=i; (void)w;

    /* interleaved timing: each rep times every SUPERCOP variant + microcode,
     * all close in time so they see the same frequency. min over reps. */
    typedef void (*permfn)(uint64_t*);
    #define NCONT 12
    struct { const char *name; const char *type; const char *key; permfn fn; uint64_t min; uint64_t med; } C[NCONT] = {
        {"x86_64_asm",     "asm",  "x86_64_asm",     keccak_x86_64_asm_perm,     UINT64_MAX, 0},
        {"x86_64_shld",    "asm",  "x86_64_shld",    keccak_x86_64_shld_perm,    UINT64_MAX, 0},
        {"openssl",        "asm",  "openssl",        keccak_openssl_perm,        UINT64_MAX, 0},
        {"opt64lcu24",     "C64",  "opt64lcu24",     keccak_opt64lcu24_perm,     UINT64_MAX, 0},
        {"opt64lcu24shld", "C64",  "opt64lcu24shld", keccak_opt64lcu24shld_perm, UINT64_MAX, 0},
        {"opt64lcu6",      "C64",  "opt64lcu6",      keccak_opt64lcu6_perm,      UINT64_MAX, 0},
        {"opt64u6",        "C64",  "opt64u6",        keccak_opt64u6_perm,        UINT64_MAX, 0},
        {"sseu2",          "SSE2", "sseu2",          keccak_sseu2_perm,          UINT64_MAX, 0},
        {"mmxu1",          "MMX",  "mmxu1",          keccak_mmxu1_perm,          UINT64_MAX, 0},
        {"simple",         "ref",  "simple",         keccak_simple_perm,         UINT64_MAX, 0},
        {"xkcp_g64",       "xkcp", "xkcp_g64",       keccak_xkcp_g64_perm,       UINT64_MAX, 0},
        {"xkcp_g64lc",     "xkcp", "xkcp_g64lc",     keccak_xkcp_g64lc_perm,     UINT64_MAX, 0},
    };
    int NC = NCONT;
    uint64_t sc_state[25];
    uint64_t uc_min=UINT64_MAX, uc_med=0;
    /* per-rep batch totals, kept so we can compute median + a robust min. */
    static uint64_t cval[NCONT][REPS];
    static uint64_t ucval[REPS];
    for(int r=0;r<REPS;r++){
        for(int c=0;c<NC;c++){
            for(int i=0;i<25;i++) sc_state[i]=0x0123456789ABCDEFULL*(i+1);
            uint64_t t0=rdtsc_start();
            for(int i=0;i<BATCH;i++) C[c].fn(sc_state);
            uint64_t t1=rdtsc_end();
            cval[c][r]=t1-t0;
        }
        for(int i=0;i<25;i++) g_keccak_buf[i]=0x0123456789ABCDEFULL*(i+1);
        uint64_t t2=rdtsc_start();
        for(int i=0;i<BATCH;i++){ g_keccak_buf[KECCAK_COUNTER_LANE]=(KECCAK_RCTAB_LANE-KECCAK_BASE_LANE)*8; ucode_perm(); }
        uint64_t t3=rdtsc_end();
        ucval[r]=t3-t2;
    }
    /* median sorts each sample array in place; robust_min then reads the sorted
     * array, skipping implausible near-zero batches (see robust_min above). */
    for(int c=0;c<NC;c++){
        C[c].med=median_u64(cval[c],REPS);
        C[c].min=robust_min(cval[c],REPS,C[c].med);
    }
    uc_med=median_u64(ucval,REPS);
    uc_min=robust_min(ucval,REPS,uc_med);

    uint64_t uc=uc_min/BATCH;

    /* Assemble all rows (every SUPERCOP variant + microcode), sort ascending by
     * min cyc/perm, and print one aligned table. */
    struct row { const char *name; const char *type; uint64_t min, med; int uco; };
    struct row R[NCONT+1];
    for(int c=0;c<NC;c++) R[c]=(struct row){C[c].name, C[c].type, C[c].min/BATCH, C[c].med/BATCH, 0};
    R[NC]=(struct row){"microcode", "ucode", uc, uc_med/BATCH, 1};
    int NR=NC+1;
    for(int i=1;i<NR;i++){ struct row k=R[i]; int j=i-1;
        while(j>=0 && R[j].min>k.min){ R[j+1]=R[j]; j--; } R[j+1]=k; }

    /* fastest non-microcode contender = the baseline we must beat */
    uint64_t best=UINT64_MAX; const char *bestname=""; const char *besttype="";
    for(int c=0;c<NC;c++){ uint64_t v=C[c].min/BATCH;
        if(v<best){best=v; bestname=C[c].name; besttype=C[c].type;} }

    printf("\n--- Keccak-f[1600] head-to-head: same process & frequency (cyc/perm) ---\n");
    printf("  %-15s %-5s %8s %8s   %s\n", "contender","type","min","median","x vs ucode");
    printf("  %-15s %-5s %8s %8s   %s\n", "---------------","-----","--------","--------","----------");
    for(int i=0;i<NR;i++){
        double x=(double)R[i].min/(double)uc;
        printf("  %-15s %-5s %8" PRIu64 " %8" PRIu64 "   %6.2fx%s\n",
               R[i].name, R[i].type, R[i].min, R[i].med, x,
               R[i].uco ? "  <== microcode" : (R[i].min<uc ? "  (beats ucode!)" : ""));
    }
    printf("\n  fastest non-microcode:   %s (%s) = %" PRIu64 " cyc/perm\n", bestname, besttype, best);
    printf("  microcode (looped):      %" PRIu64 " cyc/perm\n", uc);
    printf("  ratio microcode/fastest: %.3fx  (%s)\n",
           (double)uc/(double)best,
           uc<best ? "*** microcode WINS vs the fastest runnable variant ***" : "microcode loses");
    printf("\n(all measured back-to-back at the same CPU frequency -> ratios valid)\n");

    /* Machine-readable block scraped by bench_keccak_matrix.sh. One line per
     * contender: "keccak/<key>: ... min N median M" in cyc/perm. The matrix
     * driver greps "^keccak/<key>:" and records min/median per (config,key). */
    printf("\n=== matrix-parse ===\n");
    for(int c=0;c<NC;c++)
        printf("keccak/%s: min %" PRIu64 " median %" PRIu64 "\n",
               C[c].key, C[c].min/BATCH, C[c].med/BATCH);
    printf("keccak/microcode: min %" PRIu64 " median %" PRIu64 "\n",
           uc_min/BATCH, uc_med/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
