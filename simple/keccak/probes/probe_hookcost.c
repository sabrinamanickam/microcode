/*
 * probe_hookcost.c — Calibrated cost of redirecting into patch RAM.
 *
 * The paper quotes a figure for what it costs to reach a microcode patch
 * through the hooked instruction. The original measurement (docs/keccak-plan.md,
 * "no-op microcode hook = 4") was taken before the frequency-pinning regime the
 * rest of the paper uses: at that time RDTSC was counting at the TSC rate while
 * the core ran at turbo, so those ticks understate true cycles by roughly 2.2x.
 * This probe re-measures under the pinned regime and with the same statistics
 * every other table reports (median headline, min, p10-p90).
 *
 * Method. Install a ONE-triad patch that does nothing but end the sequence, and
 * hook it on vmwrite. Then time three loops of identical shape:
 *
 *   A  empty loop                 -> loop overhead alone
 *   B  loop + vmwrite (hooked)    -> loop + instruction + redirection + return
 *   C  loop + a plain ALU op      -> loop + one ordinary instruction
 *
 * B-A is what a firing costs end to end: decoding vmwrite, redirecting the
 * sequencer into patch RAM, running one no-op triad, and returning. B-C removes
 * one ordinary instruction's worth of that, and so brackets the redirection
 * proper from below. We report both rather than pretending a single number
 * cleanly separates "redirection" from "executing the instruction that triggers
 * it" — architecturally they are not separable, since vmwrite has no unhooked
 * cost we could subtract (outside VMX operation it faults).
 *
 * Build: make PROG=probe_hookcost ; Run: sudo taskset -c 0 ./probe_hookcost_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include "../../../../include/patch.h"
#include "../../../../include/ucode_macro.h"
#include "../../../../include/misc.h"

#define BATCH 1000
#define REPS  200

static int cmp_u64(const void *a, const void *b){
    uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b;
    return (x>y)-(x<y);
}
static uint64_t median_u64(uint64_t *v,int n){
    qsort(v,n,sizeof(uint64_t),cmp_u64);
    return (n&1)? v[n/2] : (v[n/2-1]+v[n/2])/2;
}
static uint64_t robust_min(const uint64_t *sorted,int n,uint64_t med){
    uint64_t fl=med/2;
    for(int i=0;i<n;i++) if(sorted[i]>=fl) return sorted[i];
    return sorted[n-1];
}
static uint64_t pct_u64(const uint64_t *sorted,int n,double p){
    int i=(int)(p/100.0*(n-1)+0.5); if(i<0)i=0; if(i>=n)i=n-1; return sorted[i];
}

static inline uint64_t rdtsc_start(void){
    uint32_t lo,hi; asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");
    return ((uint64_t)hi<<32)|lo;
}
static inline uint64_t rdtsc_end(void){
    uint32_t lo,hi; asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");
    asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");
    return ((uint64_t)hi<<32)|lo;
}

/* One triad that immediately ends the sequence: the smallest patch that can be
 * reached, so the measurement is dominated by getting there and back. */
static void install_noop_patch(void){
    ucode_t patch[] = {
        { NOP, NOP, NOP, END_SEQWORD },
    };
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, 1);
}

/* Same firing shape the production kernel uses, so the measured cost is the one
 * the Keccak numbers actually pay. */
static uint64_t g_dummy[8];
static inline void fire(void){
    register uint64_t *_b asm("rcx") = &g_dummy[0];
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

/* An ordinary single-uop instruction, wrapped so the compiler cannot hoist or
 * fold it, giving a reference point of "one real instruction in this loop". */
static inline void alu_op(void){
    uint64_t x=0;
    asm volatile("add %0, 1\n\t" : "+r"(x) :: "cc");
}

enum { K_EMPTY, K_FIRE, K_ALU };

static void run(int kind, const char *label, uint64_t out[4])
{
    static uint64_t s[REPS];
    for(int r=0;r<REPS;r++){
        uint64_t t0=rdtsc_start();
        switch(kind){
        case K_EMPTY: for(int i=0;i<BATCH;i++) asm volatile("":::"memory"); break;
        case K_FIRE:  for(int i=0;i<BATCH;i++) fire();                     break;
        case K_ALU:   for(int i=0;i<BATCH;i++) alu_op();                   break;
        }
        uint64_t t1=rdtsc_end();
        s[r]=(t1-t0);
    }
    /* Per-iteration cost in millicycles, so the sub-cycle loop overhead is not
     * rounded away before the subtraction. */
    for(int r=0;r<REPS;r++) s[r]=s[r]*1000ULL/BATCH;
    uint64_t med=median_u64(s,REPS);
    out[0]=med; out[1]=robust_min(s,REPS,med);
    out[2]=pct_u64(s,REPS,10.0); out[3]=pct_u64(s,REPS,90.0);
    printf("  %-22s median %7.3f  min %7.3f  p10 %7.3f  p90 %7.3f  cyc/iter\n",
           label, out[0]/1000.0, out[1]/1000.0, out[2]/1000.0, out[3]/1000.0);
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== probe_hookcost: calibrated patch-redirection cost ===\n");
    printf("g_dummy @ %p\n",(void*)g_dummy);
    if((uint64_t)g_dummy>=0x100000000ULL){ printf("FATAL >4GB\n"); return 1; }

    assign_to_core(0);
    init_match_and_patch(); do_fix_IN_patch();
    install_noop_patch();

    /* Sanity: the hook must actually be live. With no patch installed vmwrite
     * would fault, so reaching this point after a firing proves redirection. */
    fire();
    printf("hook live: one no-op firing returned normally\n\n");

    /* warm-up to steady frequency, matching the benchmark harnesses */
    { volatile uint64_t w=0; for(uint64_t i=0;i<200000000ULL;i++) w+=i; (void)w; }

    printf("--- %d iterations/batch, %d batches, pinned ---\n", BATCH, REPS);
    uint64_t e[4],f[4],a[4];
    run(K_EMPTY,"empty loop",       e);
    run(K_ALU,  "loop + 1 ALU op",  a);
    run(K_FIRE, "loop + vmwrite",   f);

    double fire_cost  = (double)(f[0]-e[0])/1000.0;   /* B - A */
    double over_alu   = (double)(f[0]-a[0])/1000.0;   /* B - C */
    printf("\n  firing cost (vmwrite + redirect + no-op triad + return):"
           " %.2f cyc   [median(loop+vmwrite) - median(empty loop)]\n", fire_cost);
    printf("  excess over one ordinary instruction:                    "
           " %.2f cyc   [median(loop+vmwrite) - median(loop+ALU)]\n", over_alu);
    printf("\n  As a share of the 1918-cycle Keccak permutation: %.2f%%\n",
           100.0*fire_cost/1918.0);

    printf("\n=== paper-parse ===\n");
    printf("hookcost: empty %.3f alu %.3f fire %.3f fire_minus_empty %.3f fire_minus_alu %.3f\n",
           e[0]/1000.0, a[0]/1000.0, f[0]/1000.0, fire_cost, over_alu);

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
