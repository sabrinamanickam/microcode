/*
 * probe_loopcost.c — Decompose the looped Keccak's 85 cyc/round. Measure the
 * cost of the loop control ALONE (backward UJMPCC + counter, no round body) by
 * running an empty counted loop for various iteration counts N. Slope = per-
 * iteration branch+counter cost; tells us whether the round cost is the branch
 * or the round body (the 25 D-loads).
 *
 * Pure register (counter in TMP0), no memory. vmwrite hook. test_q loop pattern:
 *   loop_top: ADD counter+=1 ; {XOR chk=counter^N (sets ZF), NOP, UJMPCC CONDNZ back}
 *
 * Build: make PROG=probe_loopcost ; Run: sudo taskset -c 0 ./probe_loopcost_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION 0x7c00
#define T(n) (REGION + (n)*4)

static uint64_t fire(void){
    uint64_t res;
    asm volatile("xor eax,eax\n\t vmwrite rcx, rcx\n\t" : "=a"(res) :: "rcx","rdx","memory","cc");
    return res;
}
/* Loop body has BODY_T NOP triads (avoid the tight-loop hazard that crashed a
 * 1-triad backward loop). per-iter cost = branch+counter + BODY_T triads. */
#define BODY_T 4
static uint64_t run_loop(int N){
    ucode_t p[16];
    int n=0;
    p[n++]=(ucode_t){ ZEROEXT_DSZ32_DI(TMP0, 0), NOP, NOP, NOP_SEQWORD };  /* T0 init */
    int loop_top = n;                                                      /* T1 */
    p[n++]=(ucode_t){ ADD_DSZ64_DRI(TMP0, TMP0, 1), NOP, NOP, NOP_SEQWORD };
    for (int i=0;i<BODY_T;i++) p[n++]=(ucode_t){ NOP, NOP, NOP, NOP_SEQWORD };
    p[n++]=(ucode_t){ XOR_DSZ64_DRI(TMP1, TMP0, N), NOP,
                      UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP1, T(loop_top)), NOP_SEQWORD };
    p[n++]=(ucode_t){ ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD };
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, REGION);
    patch_ucode(REGION, p, n);
    return fire();
}

static inline uint64_t rdtsc_start(void){uint32_t lo,hi;asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");return((uint64_t)hi<<32)|lo;}
static inline uint64_t rdtsc_end(void){uint32_t lo,hi;asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");return((uint64_t)hi<<32)|lo;}
#define BATCH 1000
#define REPS  100

int main(void){
    printf("=== probe_loopcost: empty counted-loop cost per iteration ===\n\n");
    assign_to_core(0);

    int Ns[]={1,2,4,8,16,32,64};
    for (size_t k=0;k<sizeof(Ns)/sizeof(Ns[0]);k++){
        int N=Ns[k];
        uint64_t chk=run_loop(N);
        if (chk != (uint64_t)N){ printf("  N=%d: loop ran wrong (RAX=%" PRIu64 "), skip timing\n",N,chk); continue; }
        uint64_t min=UINT64_MAX;
        for (int r=0;r<REPS;r++){
            uint64_t t0=rdtsc_start();
            for (int i=0;i<BATCH;i++) fire();
            uint64_t t1=rdtsc_end();
            uint64_t dt=t1-t0; if(dt<min)min=dt;
        }
        printf("  N=%2d iters: %5" PRIu64 " cyc  (%5.2f cyc/iter)\n",
               N, min/BATCH, (double)(min/BATCH)/N);
    }
    printf("\n(slope between rows = per-iteration backward-branch+counter cost)\n");
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
