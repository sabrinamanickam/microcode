/*
 * test_intradata.c — Can slot 1 read a value slot 0 writes in same triad?
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Flag-based carry is dead (END_SEQWORD restores EFLAGS).
 *  Bit-manipulation carry chain is 6 triads.
 *  Can we compress to 5 by merging the last two triads?
 *
 *  Current:
 *    T4: SHR TMP5, TMP5, 63    (extract carry bit)
 *    T5: ADD R8, R8, TMP5      (fold into acc_hi)   | END
 *
 *  Proposed:
 *    T4: SHR TMP5, TMP5, 63 | ADD R8, R8, TMP5 | END
 *
 *  Question: does ADD in slot 1 see the value SHR wrote to TMP5
 *  in slot 0 of the same triad? Or does it read the stale value?
 *
 *  Test approach:
 *    RAX = test value with known bit 63
 *    Hook: SHR TMP5, RAX, 63 → TMP5 should be 0 or 1
 *           ADD R8, R8, TMP5  → R8 += (0 or 1)
 *           Return R8 in RAX
 *
 *  Also test the full 5-triad MAC with curve25519 square.
 *
 *  Build:  make PROG=test_intradata
 *  Run:    sudo taskset -c 0 ./test_intradata_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL
#define OPS_PER_BATCH   1000
#define NUM_BATCHES     200
#define WARMUP_BATCHES  20


/* ══════════════════════════════════════════════════════════════════
 *  Test A: SHR + ADD in same triad (intra-triad forwarding)
 *
 *  T0: SHR TMP5, RAX, 63  |  ADD R8, R8, TMP5  |  NOP
 *  T1: ZEROEXT RAX, R8    |  END
 *
 *  Input:  RAX = value, R8 = 0
 *  Output: RAX = bit63(value)
 * ══════════════════════════════════════════════════════════════════ */
static void install_test_merged(void) {
        ucode_t patch[] = {
                {
                        SHR_DSZ64_DRI(TMP5, RAX, 63),
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, R8),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Test B: SHR and ADD in separate triads (known to work, baseline) */
static void install_test_split(void) {
        ucode_t patch[] = {
                {
                        SHR_DSZ64_DRI(TMP5, RAX, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, R8),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 3);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t invoke(uint64_t rax_val) {
        uint64_t result;
        asm volatile(
                "mov rax, %[a]\n\t"
                "xor r8, r8\n\t"
                "xor rcx, rcx\n\t"
                "xor rdx, rdx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(result)
                : [a] "r"(rax_val)
                : "rax", "rcx", "rdx", "r8"
        );
        return result;
}

static void run_forwarding_test(const char *name) {
        struct { uint64_t val; uint64_t expect; const char *desc; } cases[] = {
                { 0,                         0, "0 → bit63=0" },
                { 1,                         0, "1 → bit63=0" },
                { 0x7FFFFFFFFFFFFFFFULL,     0, "0x7F..F → bit63=0" },
                { 0x8000000000000000ULL,     1, "0x80..0 → bit63=1" },
                { 0xFFFFFFFFFFFFFFFFULL,     1, "0xFF..F → bit63=1" },
                { 0xC000000000000000ULL,     1, "0xC0..0 → bit63=1" },
        };
        int n = sizeof(cases) / sizeof(cases[0]);
        int pass = 0;

        printf("--- %s ---\n", name);
        for (int i = 0; i < n; i++) {
                uint64_t got = invoke(cases[i].val);
                int ok = (got == cases[i].expect);
                pass += ok;
                printf("  %-25s  expect:%" PRIu64 "  got:%" PRIu64 "  %s\n",
                       cases[i].desc, cases[i].expect, got,
                       ok ? "PASS" : "** FAIL **");
        }
        printf("  Result: %d / %d\n\n", pass, n);
}


/* ══════════════════════════════════════════════════════════════════
 *  5-TRIAD MAC (if forwarding works)
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac5t(void) {
        ucode_t patch[] = {
                /* T0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: ADD + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T2: propagated overflow + acc_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: merge carry bits + extract */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: extract carry + fold + DONE (merged) */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 5);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* 6-triad MAC (reference) */
static void install_mac6t(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 6);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  HELPERS
 * ══════════════════════════════════════════════════════════════════ */
static void fe_sq_ref(const uint64_t *a, uint64_t *out) {
        uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
        uint64_t d0=2*a0,d1=2*a1,d2=2*a2,d3=2*a3;
        uint64_t r3=19*a3,r4=19*a4;
        __uint128_t c0=(__uint128_t)a0*a0+(__uint128_t)d1*r4+(__uint128_t)d2*r3;
        __uint128_t c1=(__uint128_t)d0*a1+(__uint128_t)r3*a3+(__uint128_t)d2*r4;
        __uint128_t c2=(__uint128_t)d0*a2+(__uint128_t)a1*a1+(__uint128_t)d3*r4;
        __uint128_t c3=(__uint128_t)d0*a3+(__uint128_t)d1*a2+(__uint128_t)r4*a4;
        __uint128_t c4=(__uint128_t)d0*a4+(__uint128_t)d1*a3+(__uint128_t)a2*a2;
        uint64_t carry;
        carry=(uint64_t)(c0>>51); out[0]=(uint64_t)c0&MASK51;
        c1+=carry; carry=(uint64_t)(c1>>51); out[1]=(uint64_t)c1&MASK51;
        c2+=carry; carry=(uint64_t)(c2>>51); out[2]=(uint64_t)c2&MASK51;
        c3+=carry; carry=(uint64_t)(c3>>51); out[3]=(uint64_t)c3&MASK51;
        c4+=carry; carry=(uint64_t)(c4>>51); out[4]=(uint64_t)c4&MASK51;
        out[0]+=carry*19; carry=out[0]>>51; out[0]&=MASK51; out[1]+=carry;
}

__attribute__((noinline))
static void fe_sq_mac(const uint64_t *a, uint64_t *out) {
        uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
        uint64_t d0=2*a0,d1=2*a1,d2=2*a2,d3=2*a3;
        uint64_t r3=19*a3,r4=19*a4;
        uint64_t c_lo,c_hi,carry;
#define MAC(xA,yA) "mov rcx,%[" xA "]\n\t" "mov rdx,%[" yA "]\n\t" "vmwrite rcx,rdx\n\t"

        asm volatile("xor rax,rax\n\t" "xor r8,r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[x1]"r"(a0),[y1]"r"(a0),[x2]"r"(d1),[y2]"r"(r4),[x3]"r"(d2),[y3]"r"(r3)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[0]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a1),[x2]"r"(r3),[y2]"r"(a3),[x3]"r"(d2),[y3]"r"(r4)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[1]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a2),[x2]"r"(a1),[y2]"r"(a1),[x3]"r"(d3),[y3]"r"(r4)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[2]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a3),[x2]"r"(d1),[y2]"r"(a2),[x3]"r"(r4),[y3]"r"(a4)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[3]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a4),[x2]"r"(d1),[y2]"r"(a3),[x3]"r"(a2),[y3]"r"(a2)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[4]=c_lo&MASK51;
#undef MAC
        out[0]+=carry*19; carry=out[0]>>51; out[0]&=MASK51; out[1]+=carry;
}

static void fe_normalize(const uint64_t *in, uint64_t *out) {
        uint64_t t[5],c; memcpy(t,in,40);
        for(int p=0;p<2;p++){
                c=t[0]>>51;t[0]&=MASK51;t[1]+=c;c=t[1]>>51;t[1]&=MASK51;
                t[2]+=c;c=t[2]>>51;t[2]&=MASK51;t[3]+=c;c=t[3]>>51;t[3]&=MASK51;
                t[4]+=c;c=t[4]>>51;t[4]&=MASK51;t[0]+=c*19;
        }
        memcpy(out,t,40);
}

static int fe_equal(const uint64_t *a, const uint64_t *b) {
        uint64_t na[5],nb[5]; fe_normalize(a,na); fe_normalize(b,nb);
        return memcmp(na,nb,40)==0;
}

static void print_limbs(const char *label, const uint64_t *v) {
        printf("  %-8s [%016"PRIx64", %016"PRIx64", %016"PRIx64",\n"
               "           %016"PRIx64", %016"PRIx64"]\n",
               label,v[0],v[1],v[2],v[3],v[4]);
}

typedef void (*sq_fn)(const uint64_t *, uint64_t *);

static int cmp_u64(const void *a, const void *b) {
        uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b;
        return (x>y)-(x<y);
}

static void bench(const char *name, sq_fn fn, const uint64_t *input) {
        uint64_t timings[NUM_BATCHES],out[5];
        volatile uint64_t sink=0; (void)sink;
        for(int i=0;i<WARMUP_BATCHES;i++) for(int j=0;j<OPS_PER_BATCH;j++) fn(input,out);
        for(int b=0;b<NUM_BATCHES;b++){
                uint64_t t0=({uint32_t lo,hi; asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx"); ((uint64_t)hi<<32)|lo;});
                for(int j=0;j<OPS_PER_BATCH;j++) fn(input,out);
                uint64_t t1=({uint32_t lo,hi; asm volatile("rdtscp\n\tmov %0,eax\n\tmov %1,edx\n\tcpuid":"=r"(lo),"=r"(hi)::"rax","rbx","rcx","rdx"); ((uint64_t)hi<<32)|lo;});
                timings[b]=t1-t0; sink=out[0];
        }
        qsort(timings,NUM_BATCHES,sizeof(uint64_t),cmp_u64);
        printf("  %-12s  median: %4"PRIu64" cyc/op   [p10: %4"PRIu64",  p90: %4"PRIu64"]\n",
               name, timings[NUM_BATCHES/2]/OPS_PER_BATCH,
               timings[NUM_BATCHES/10]/OPS_PER_BATCH,
               timings[NUM_BATCHES*9/10]/OPS_PER_BATCH);
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  Intra-Triad Data Forwarding + 5-Triad MAC Test\n");
        printf("==========================================================\n\n");

        uint64_t input[5] = {
                0x00062D608F25D51AULL,0x000412A4B4F6592AULL,
                0x00075B7171A4B31DULL,0x0001FF60527118FEULL,
                0x000216936D3CD6E5ULL
        };
        uint64_t ref[5];
        fe_sq_ref(input, ref);

        /* ── Part 1: Data forwarding test ─────────────────────── */

        printf("Part 1: Intra-triad data forwarding\n");
        printf("  SHR writes TMP5, ADD reads TMP5 in same triad\n\n");

        printf("Baseline (split triads):\n");
        install_test_split();
        run_forwarding_test("Split (2 triads)");

        printf("Merged (same triad):\n");
        install_test_merged();
        run_forwarding_test("Merged (1 triad)");

        /* ── Part 2: 5-triad MAC if forwarding works ──────────── */

        printf("==========================================================\n");
        printf("Part 2: 5-triad MAC (curve25519 square)\n\n");

        printf("Installing 5-triad MAC...\n");
        install_mac5t();

        uint64_t out[5];
        fe_sq_mac(input, out);
        printf("  Correctness: ");
        if (fe_equal(ref,out)) {
                printf("PASS\n");
        } else {
                printf("FAIL\n");
                print_limbs("ref:", ref);
                print_limbs("mac5t:", out);
                printf("\n  Forwarding probably doesn't work.\n");
                printf("  Falling back to 6-triad MAC.\n\n");

                /* Still bench the 6-triad for reference */
                install_mac6t();
                fe_sq_mac(input, out);
                printf("  6-triad correctness: %s\n\n", fe_equal(ref,out)?"PASS":"FAIL");
                bench("ref (C)", fe_sq_ref, input);
                bench("mac6t", fe_sq_mac, input);
                return 1;
        }

        /* Iterated */
        {
                uint64_t r[5]={1,0,0,0,0},m[5]={1,0,0,0,0},tmp[5];
                for(int i=0;i<1000;i++){
                        fe_sq_ref(r,tmp);memcpy(r,tmp,40);
                        fe_sq_mac(m,tmp);memcpy(m,tmp,40);
                }
                printf("  Iterated (1000x): %s\n",fe_equal(r,m)?"PASS":"FAIL");
                if(!fe_equal(r,m)) return 1;
        }

        printf("\n");
        bench("ref (C)", fe_sq_ref, input);
        bench("mac5t", fe_sq_mac, input);

        /* 6-triad for comparison */
        printf("\nRe-installing 6-triad MAC...\n\n");
        install_mac6t();
        fe_sq_mac(input, out);
        printf("  Correctness: %s\n\n", fe_equal(ref,out)?"PASS":"FAIL");
        bench("ref (C)", fe_sq_ref, input);
        bench("mac6t", fe_sq_mac, input);

        printf("\n==========================================================\n");
        printf("  mac6t: 15 x 6 = 90 triads\n");
        printf("  mac5t: 15 x 5 = 75 triads (if forwarding works)\n");
        printf("==========================================================\n\n");

        return 0;
}
