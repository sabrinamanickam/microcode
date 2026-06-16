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

/* SUPERCOP scalar Keccak permutations — benchmark vs ALL fast x86-64 variants
 * so we compare against the TRUE fastest (what SUPERCOP's autotuner would pick). */
extern void keccak_x86_64_asm_perm(uint64_t state[25]);        /* Van Keer, ROL */
extern void keccak_x86_64_shld_perm(uint64_t state[25]);       /* Van Keer, SHLD */
extern void keccak_opt64lcu24_perm(uint64_t state[25]);        /* C, 24x unroll, bebigokimisa */
extern void keccak_opt64lcu24shld_perm(uint64_t state[25]);    /* C, 24x unroll, SHLD */

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
    #define NCONT 4
    struct { const char *name; const char *key; permfn fn; uint64_t min; uint64_t med; } C[NCONT] = {
        {"x86_64_asm    ", "x86_64_asm",     keccak_x86_64_asm_perm,     UINT64_MAX, 0},
        {"x86_64_shld   ", "x86_64_shld",    keccak_x86_64_shld_perm,    UINT64_MAX, 0},
        {"opt64lcu24    ", "opt64lcu24",     keccak_opt64lcu24_perm,     UINT64_MAX, 0},
        {"opt64lcu24shld", "opt64lcu24shld", keccak_opt64lcu24shld_perm, UINT64_MAX, 0},
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

    uint64_t best=UINT64_MAX; const char *bestname="";
    printf("\n--- SUPERCOP scalar Keccak variants (cyc/perm, same freq) ---\n");
    for(int c=0;c<NC;c++){
        uint64_t v=C[c].min/BATCH;
        printf("  %s %5" PRIu64 "\n", C[c].name, v);
        if(v<best){best=v; bestname=C[c].name;}
    }
    uint64_t uc=uc_min/BATCH;
    printf("  >> fastest SUPERCOP: %s %5" PRIu64 " cyc/perm\n", bestname, best);
    printf("microcode (looped):    %5" PRIu64 " cyc/perm\n", uc);
    printf("ratio ucode/FASTEST:   %.3fx  (%s)\n",
           (double)uc/(double)best,
           uc<best ? "*** microcode WINS vs the fastest ***" : "microcode loses");
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
