/*
 * bench_ablation.c — which design choice actually buys the speedup?
 *
 * The paper attributes the Keccak result to four choices: whole-state residency
 * with D kept in registers, a fused in-place transport for theta-apply/rho/pi, a
 * move-free chi, and a balanced theta-parity tree (plus a round-constant cursor
 * that keeps shifts off the iota path). Attribution is not evidence. This harness
 * measures it.
 *
 * keccak_ablate.py generates one variant of the kernel per design choice, with
 * that choice — and only that choice — removed. Every variant is verified in
 * simulation against the 24-round reference and the published KAT vectors before
 * it gets here, and is verified again ON HARDWARE below: a variant that computes
 * the wrong permutation would produce a meaningless cycle count.
 *
 * All variants are timed in ONE process, interleaved rep by rep, so they see the
 * same frequency and thermal state. Switching variants means rewriting patch RAM,
 * which happens outside the timed region.
 *
 * Build: make PROG=bench_ablation
 * Run:   sudo taskset -c 0 ./bench_ablation_static
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

#include "../keccak_perm.h"      /* KECCAK_BUFLEN / BASE_LANE / COUNTER / RCTAB */
#include "../abl/abl_meta.h"

static uint64_t g_buf[KECCAK_BUFLEN];

static const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL,0x0000000000008082ULL,0x800000000000808aULL,0x8000000080008000ULL,
    0x000000000000808bULL,0x0000000080000001ULL,0x8000000080008081ULL,0x8000000000008009ULL,
    0x000000000000008aULL,0x0000000000000088ULL,0x0000000080008009ULL,0x000000008000000aULL,
    0x000000008000808bULL,0x800000000000008bULL,0x8000000000008089ULL,0x8000000000008003ULL,
    0x8000000000008002ULL,0x8000000000000080ULL,0x000000000000800aULL,0x800000008000000aULL,
    0x8000000080008081ULL,0x8000000000008080ULL,0x0000000080000001ULL,0x8000000080008008ULL,
};

/* Published Keccak-f[1600] vectors: f(0), and f applied to that (a fully
 * non-zero input, so the second is a non-trivial check). */
static const uint64_t KAT_F0[25] = {
    0xF1258F7940E1DDE7ULL,0x84D5CCF933C0478AULL,0xD598261EA65AA9EEULL,0xBD1547306F80494DULL,0x8B284E056253D057ULL,
    0xFF97A42D7F8E6FD4ULL,0x90FEE5A0A44647C4ULL,0x8C5BDA0CD6192E76ULL,0xAD30A6F71B19059CULL,0x30935AB7D08FFC64ULL,
    0xEB5AA93F2317D635ULL,0xA9A6E6260D712103ULL,0x81A57C16DBCF555FULL,0x43B831CD0347C826ULL,0x01F22F1A11A5569FULL,
    0x05E5635A21D9AE61ULL,0x64BEFEF28CC970F2ULL,0x613670957BC46611ULL,0xB87C5A554FD00ECBULL,0x8C3EE88A1CCF32C8ULL,
    0x940C7922AE3A2614ULL,0x1841F924A2C509E4ULL,0x16F53526E70465C2ULL,0x75F644E97F30A13BULL,0xEAF1FF7B5CECA249ULL,
};
static const uint64_t KAT_F0F0[25] = {
    0x2D5C954DF96ECB3CULL,0x6A332CD07057B56DULL,0x093D8D1270D76B6CULL,0x8A20D9B25569D094ULL,0x4F9C4F99E5E7F156ULL,
    0xF957B9A2DA65FB38ULL,0x85773DAE1275AF0DULL,0xFAF4F247C3D810F7ULL,0x1F1B9EE6F79A8759ULL,0xE4FECC0FEE98B425ULL,
    0x68CE61B6B9CE68A1ULL,0xDEEA66C4BA8F974FULL,0x33C43D836EAFB1F5ULL,0xE00654042719DBD9ULL,0x7CF8A9F009831265ULL,
    0xFD5449A6BF174743ULL,0x97DDAD33D8994B40ULL,0x48EAD5FC5D0BE774ULL,0xE3B8C8EE55B7B03CULL,0x91A0226E649E42E9ULL,
    0x900E3129E7BADD7BULL,0x202A9EC5FAA3CCE8ULL,0x5B3402464E1C3DB6ULL,0x609F4E62A44C1059ULL,0x20D06CD26A8FBF5CULL,
};

/* ── the variants ── */
static const ucode_t P_baseline[]     = {
    #include "../abl/baseline_body.h"
};
static const ucode_t P_balanced_theta[] = {
    #include "../abl/balanced_theta_body.h"
};
static const ucode_t P_spill_d[]      = {
    #include "../abl/spill_d_body.h"
};
static const ucode_t P_savemov_chi[]  = {
    #include "../abl/savemov_chi_body.h"
};
static const ucode_t P_no_notand[]    = {
    #include "../abl/no_notand_body.h"
};
static const ucode_t P_rc_shift[]     = {
    #include "../abl/rc_shift_body.h"
};

struct variant {
    const char     *name;
    const char     *removed;
    const ucode_t  *patch;
    int             triads;
    int             ops;        /* operations in the round body, from the generator */
    int             memops;
    uint64_t        counter_init;
    uint64_t        med, min, p10, p90;
    int             ok;
};

static struct variant V[] = {
  {"baseline",     "-- (kernel as shipped)",        P_baseline,     ABL_BASELINE_TRIADS,     151, 18, ABL_BASELINE_COUNTER_INIT,     0,0,0,0,0},
  {"balanced_theta","serial theta-parity (rejected alt)", P_balanced_theta, ABL_BALANCED_THETA_TRIADS, 151, 18, ABL_BALANCED_THETA_COUNTER_INIT, 0,0,0,0,0},
  {"spill_d",      "resident D registers",          P_spill_d,      ABL_SPILL_D_TRIADS,      181, 48, ABL_SPILL_D_COUNTER_INIT,      0,0,0,0,0},
  {"savemov_chi",  "move-free chi",                 P_savemov_chi,  ABL_SAVEMOV_CHI_TRIADS,  161, 18, ABL_SAVEMOV_CHI_COUNTER_INIT,  0,0,0,0,0},
  {"no_notand",    "NOTAND micro-op",               P_no_notand,    ABL_NO_NOTAND_TRIADS,    176, 18, ABL_NO_NOTAND_COUNTER_INIT,    0,0,0,0,0},
  {"rc_shift",     "RC byte-index cursor",          P_rc_shift,     ABL_RC_SHIFT_TRIADS,     153, 18, ABL_RC_SHIFT_COUNTER_INIT,     0,0,0,0,0},
};
#define NVAR ((int)(sizeof V / sizeof V[0]))

static void install(const struct variant *v){
    patch_ucode(0x7c00, (ucode_t*)v->patch, v->triads);
}
static void reset_rc(void){
    for(int r=0;r<24;r++) g_buf[KECCAK_RCTAB_LANE+r]=KECCAK_RC[r];
}
static inline void set_counter(uint64_t init){ g_buf[KECCAK_COUNTER_LANE]=init; }

static inline void perm(uint64_t init){
    set_counter(init);
    register uint64_t *_b asm("rcx") = &g_buf[KECCAK_BASE_LANE];
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) :
        : "rax","rbx","rdx","rdi","rsi","rbp","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
}

/* Run the two published vectors through whichever variant is installed. */
static int kat_on_hw(const struct variant *v){
    memset(g_buf,0,25*sizeof(uint64_t)); reset_rc();
    perm(v->counter_init);
    if(memcmp(g_buf,KAT_F0,25*sizeof(uint64_t))) return 0;
    memcpy(g_buf,KAT_F0,25*sizeof(uint64_t)); reset_rc();
    perm(v->counter_init);
    if(memcmp(g_buf,KAT_F0F0,25*sizeof(uint64_t))) return 0;
    return 1;
}

static int cmp_u64(const void*a,const void*b){
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b; return (x>y)-(x<y);
}
static uint64_t median_u64(uint64_t*v,int n){
    qsort(v,n,sizeof(uint64_t),cmp_u64); return (n&1)?v[n/2]:(v[n/2-1]+v[n/2])/2;
}
static uint64_t robust_min(const uint64_t*s,int n,uint64_t med){
    uint64_t f=med/2; for(int i=0;i<n;i++) if(s[i]>=f) return s[i]; return s[n-1];
}
static uint64_t pct_u64(const uint64_t*s,int n,double p){
    int i=(int)(p/100.0*(n-1)+0.5); if(i<0)i=0; if(i>=n)i=n-1; return s[i];
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

#define BATCH 1000
#define REPS  200

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== Keccak microcode ablation: which choice buys the speedup? ===\n");
    printf("g_buf @ %p\n",(void*)g_buf);
    if((uint64_t)g_buf>=0x100000000ULL){ printf("FATAL >4GB\n"); return 1; }

    assign_to_core(0);
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, 0x7c00);

    /* ── correctness on hardware, before any timing ── */
    printf("\n--- hardware KAT per variant (f(0) and f(f(0)), 25 lanes each) ---\n");
    int nok=0;
    for(int i=0;i<NVAR;i++){
        install(&V[i]);
        V[i].ok = kat_on_hw(&V[i]);
        printf("  %-13s %-30s triads %3d  %s\n",
               V[i].name, V[i].removed, V[i].triads, V[i].ok?"PASS":"FAIL — excluded");
        nok += V[i].ok;
    }
    if(!V[0].ok){ printf("\nbaseline failed; aborting\n"); goto done; }

    /* warm-up to steady frequency, as in the other harnesses */
    { volatile uint64_t w=0; for(uint64_t i=0;i<200000000ULL;i++) w+=i; (void)w; }

    /* ── interleaved timing ── */
    static uint64_t sample[NVAR][REPS];
    printf("\n--- timing: %d permutations/batch, %d batches, interleaved ---\n", BATCH, REPS);
    for(int r=0;r<REPS;r++){
        for(int i=0;i<NVAR;i++){
            if(!V[i].ok) continue;
            install(&V[i]);                       /* outside the timed region */
            for(int k=0;k<25;k++) g_buf[k]=0x0123456789ABCDEFULL*(k+1);
            reset_rc();
            uint64_t t0=rdtsc_start();
            for(int b=0;b<BATCH;b++) perm(V[i].counter_init);
            uint64_t t1=rdtsc_end();
            sample[i][r]=(t1-t0)/BATCH;
        }
    }
    for(int i=0;i<NVAR;i++){
        if(!V[i].ok) continue;
        V[i].med=median_u64(sample[i],REPS);
        V[i].min=robust_min(sample[i],REPS,V[i].med);
        V[i].p10=pct_u64(sample[i],REPS,10.0);
        V[i].p90=pct_u64(sample[i],REPS,90.0);
    }

    uint64_t base=V[0].med;
    printf("\n  %-13s %7s %7s %7s %7s %6s %5s %5s %9s %8s  %s\n",
           "variant","median","min","p10","p90","triads","ops","mem","delta","cost","design choice removed");
    for(int i=0;i<NVAR;i++){
        if(!V[i].ok) continue;
        long d=(long)V[i].med-(long)base;
        printf("  %-13s %7"PRIu64" %7"PRIu64" %7"PRIu64" %7"PRIu64" %6d %5d %5d %+9ld %7.2f%%  %s\n",
               V[i].name,V[i].med,V[i].min,V[i].p10,V[i].p90,
               V[i].triads,V[i].ops,V[i].memops,d,100.0*d/(double)base,V[i].removed);
    }

    printf("\n  Ranked by what each choice is worth (largest saving first):\n");
    int idx[NVAR],n=0;
    for(int i=1;i<NVAR;i++) if(V[i].ok) idx[n++]=i;
    for(int a=1;a<n;a++){ int k=idx[a],b2=a-1;
        while(b2>=0 && V[idx[b2]].med<V[k].med){ idx[b2+1]=idx[b2]; b2--; } idx[b2+1]=k; }
    for(int a=0;a<n;a++){
        int i=idx[a]; long d=(long)V[i].med-(long)base;
        printf("    %d. %-13s worth %ld cycles/permutation (%.2f%%)  [%s]\n",
               a+1, V[i].name, d, 100.0*d/(double)base, V[i].removed);
    }

    printf("\n=== paper-parse ===\n");
    for(int i=0;i<NVAR;i++) if(V[i].ok)
        printf("abl/%s: median %"PRIu64" min %"PRIu64" p10 %"PRIu64" p90 %"PRIu64
               " triads %d ops %d mem %d\n",
               V[i].name,V[i].med,V[i].min,V[i].p10,V[i].p90,V[i].triads,V[i].ops,V[i].memops);

done:
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
