/*
 * asm_op_keccak_scall.c — Keccak-f[1600] as 24 SEPARATE vmwrite calls (one
 * round each), not an in-microcode loop. Phase 4 fix.
 *
 * The round-count sweep showed the in-microcode backward branch SERIALIZES
 * iterations (~84 cyc/round), while separate vmwrites OVERLAP in the OoO engine
 * (~53 cyc/round incl I/O). So we issue 24 separate vmwrites from C; each runs
 * one round on g_keccak_buf, reading RC[round] from a buffer slot.
 *
 * Buffer (g_keccak_buf[KECCAK_SC_BUFLEN]), base RCX = &buf[KECCAK_SC_BASE_LANE]:
 *   [0..24] state   [25..29] D scratch   [30] RC slot (C writes RC[round]).
 *
 * Build: make PROG=asm_op_keccak_scall
 * Run:   sudo taskset -c 0 ./asm_op_keccak_scall_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#include "keccak_scround.h"   /* KECCAK_SC_TRIADS, SC_BUFLEN, SC_BASE_LANE, SC_RC_LANE */
static uint64_t g_keccak_buf[KECCAK_SC_BUFLEN];

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
static inline uint64_t rol64(uint64_t v,int n){n&=63;return n?((v<<n)|(v>>(64-n))):v;}
static void keccak_round_ref(uint64_t s[25], uint64_t RC){
    uint64_t C[5],D[5],B[25];
    for(int x=0;x<5;x++)C[x]=s[x]^s[x+5]^s[x+10]^s[x+15]^s[x+20];
    for(int x=0;x<5;x++)D[x]=C[(x+4)%5]^rol64(C[(x+1)%5],1);
    for(int y=0;y<5;y++)for(int x=0;x<5;x++)B[y+5*((2*x+3*y)%5)]=rol64(s[x+5*y]^D[x],RHO[x+5*y]);
    for(int y=0;y<5;y++)for(int x=0;x<5;x++)s[x+5*y]=B[x+5*y]^(~B[(x+1)%5+5*y]&B[(x+2)%5+5*y]);
    s[0]^=RC;
}
static void keccak_perm_ref(uint64_t s[25]){ for(int r=0;r<24;r++) keccak_round_ref(s,KECCAK_RC[r]); }

static void install_scround_patch(void){
    ucode_t patch[] = {
        #include "keccak_scround_body.h"
    };
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c00, patch, KECCAK_SC_TRIADS);
    printf("scround patch installed: %d triads at U7c00\n", KECCAK_SC_TRIADS);
}

/* One round: RC passed in RDX (NO C-side buffer store — that would serialize
 * consecutive vmwrites). The microcode saves RDX to the RC slot internally.
 * base = &buf[SC_BASE_LANE] in RCX. */
static inline void scround(uint64_t rc){
    register uint64_t *_b  asm("rcx") = &g_keccak_buf[KECCAK_SC_BASE_LANE];
    register uint64_t _rc  asm("rdx") = rc;
    asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_b) : "r"(_rc)
        : "rax","rbx","rdi","rsi","rbp",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");
}

/* Full permutation = 24 separate rounds. */
static void keccak_perm_scall(void){
    for (int r=0;r<24;r++) scround(KECCAK_RC[r]);
}

static int check(const char *label, uint64_t in[25]){
    uint64_t ref[25]; memcpy(ref,in,sizeof(ref)); keccak_perm_ref(ref);
    memcpy(g_keccak_buf,in,25*8); keccak_perm_scall();
    int fails=0;
    for(int i=0;i<25;i++) if(g_keccak_buf[i]!=ref[i]){
        if(fails<4) printf("  [%2d] ucode=%016" PRIx64 " ref=%016" PRIx64 " ***\n",i,g_keccak_buf[i],ref[i]);
        fails++;
    }
    printf("%-18s %s (%d/25)\n", label, fails?"FAIL":"PASS", 25-fails);
    return fails;
}
static int verify(void){
    int f=0;
    uint64_t z[25]={0}; f+=check("zero-state",z);
    uint64_t zr[25]={0}; keccak_perm_ref(zr);
    printf("  KAT lane0=%016" PRIx64 " (expect F1258F7940E1DDE7) %s\n",
           zr[0], zr[0]==0xF1258F7940E1DDE7ULL?"OK":"REF-WRONG");
    uint64_t a[25]; for(int i=0;i<25;i++)a[i]=0x0123456789ABCDEFULL*(i+1)^0xDEADBEEFCAFEBABEULL;
    f+=check("nontrivial",a);
    uint64_t seed=0x999;
    for(int t=0;t<3;t++){uint64_t r[25];for(int i=0;i<25;i++){seed=seed*6364136223846793005ULL+1;r[i]=seed;}char l[24];snprintf(l,sizeof(l),"random[%d]",t);f+=check(l,r);}
    return f;
}

static inline uint64_t rdtsc_start(void){uint32_t lo,hi;asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");return((uint64_t)hi<<32)|lo;}
static inline uint64_t rdtsc_end(void){uint32_t lo,hi;asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");return((uint64_t)hi<<32)|lo;}
#define BATCH 1000
#define REPS  100
static void bench(void){
    for(int i=0;i<25;i++)g_keccak_buf[i]=0x0123456789ABCDEFULL*(i+1);
    uint64_t min=UINT64_MAX,sum=0;
    for(int r=0;r<REPS;r++){
        uint64_t t0=rdtsc_start();
        for(int i=0;i<BATCH;i++) keccak_perm_scall();
        uint64_t t1=rdtsc_end();
        uint64_t dt=t1-t0; sum+=dt; if(dt<min)min=dt;
    }
    printf("\n--- separate-call permutation (24 vmwrites, %d triads/round) ---\n", KECCAK_SC_TRIADS);
    printf("min/perm %5" PRIu64 "  avg/perm %5" PRIu64 " cycles  (%" PRIu64 " cyc/round)\n",
           min/BATCH, sum/REPS/BATCH, (min/BATCH)/24);
    printf("baseline x86_64_asm: 939 cyc.  looped microcode: 2056.  %s\n",
           min/BATCH<939?"*** WIN ***":"(loss, but vs looped)");
}

int main(void){
    printf("=== asm_op_keccak_scall: 24 separate vmwrites ===\n\n");
    printf("g_keccak_buf @ %p (below 4GB: %s)\n",(void*)g_keccak_buf,
           (uint64_t)g_keccak_buf<0x100000000ULL?"YES":"NO");
    if((uint64_t)g_keccak_buf>=0x100000000ULL){printf("FATAL >4GB\n");return 1;}
    assign_to_core(0);
    init_match_and_patch(); do_fix_IN_patch();
    install_scround_patch();
    int fails=verify();
    if(!fails) bench();
    init_match_and_patch(); do_fix_IN_patch();
    printf(fails?"\nFAILED.\n":"\nOK.\n");
    return fails?1:0;
}
