/*
 * bench_hybrid.c — Hybrid native mul + microcode MAC curve25519 square
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Strategy: per limb, do 2 products with native mul+add/adc,
 *  then feed the partial sum into vmwrite MAC for the 3rd product.
 *
 *  Per limb:
 *    mov rax, A1; mul B1    → rdx:rax = product1
 *    save lo/hi
 *    mov rax, A2; mul B2    → rdx:rax = product2
 *    add/adc to accumulate
 *    seed RAX:R8 with partial sum
 *    vmwrite for product3   → RAX:R8 += A3×B3
 *
 *  5 vmwrite calls (was 15). Native mul is 3 cycles on Goldmont
 *  (constant-time on in-order pipeline).
 *
 *  Expected: ~60-75 cycles
 *    10 native muls × ~3 cyc = ~30 cyc
 *    5 vmwrite × ~7 cyc      = ~35 cyc
 *    x86 overhead             = ~10 cyc
 *
 *  Build:  make PROG=bench_hybrid
 *  Run:    sudo taskset -c 0 ./bench_hybrid_static
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
 *  5-TRIAD 1-MAC HOOK (same as proven working)
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac(void) {
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


/* ══════════════════════════════════════════════════════════════════
 *  REFERENCE
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


/* ══════════════════════════════════════════════════════════════════
 *  HYBRID SQUARE — 2 native mul + 1 vmwrite MAC per limb
 *
 *  Products:
 *   c[0] = a0·a0  + d1·r4 + d2·r3
 *   c[1] = d0·a1  + r3·a3 + d2·r4
 *   c[2] = d0·a2  + a1·a1 + d3·r4
 *   c[3] = d0·a3  + d1·a2 + r4·a4
 *   c[4] = d0·a4  + d1·a3 + a2·a2
 *          ─────    ─────   ─────
 *          native1  native2  vmwrite MAC
 *
 *  Native mul clobbers rax and rdx. We save partial sums in
 *  r10:r11 (lo:hi), then seed RAX:R8 and vmwrite.
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_hybrid(const uint64_t *a, uint64_t *out) {
        uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
        uint64_t d0=2*a0,d1=2*a1,d2=2*a2,d3=2*a3;
        uint64_t r3=19*a3,r4=19*a4;

        uint64_t c_lo, c_hi, carry;

        /* ── c[0] = a0·a0 + d1·r4 + d2·r3 ─────────────────── */
        asm volatile(
                /* native mul 1: a0 * a0 */
                "mov rax, %[A1]\n\t"
                "mul %[B1]\n\t"         /* rdx:rax = a0*a0 */
                "mov r10, rax\n\t"      /* r10 = product1_lo */
                "mov r11, rdx\n\t"      /* r11 = product1_hi */
                /* native mul 2: d1 * r4 */
                "mov rax, %[A2]\n\t"
                "mul %[B2]\n\t"         /* rdx:rax = d1*r4 */
                "add r10, rax\n\t"      /* accumulate lo */
                "adc r11, rdx\n\t"      /* accumulate hi + carry */
                /* vmwrite MAC: d2 * r3 accumulated onto r10:r11 */
                "mov rax, r10\n\t"
                "mov r8, r11\n\t"
                "mov rcx, %[A3]\n\t"
                "mov rdx, %[B3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [A1] "r"(a0), [B1] "r"(a0),
                  [A2] "r"(d1), [B2] "r"(r4),
                  [A3] "r"(d2), [B3] "r"(r3)
                : "rax", "rcx", "rdx", "r8", "r10", "r11"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[0] = c_lo & MASK51;

        /* ── c[1] = d0·a1 + r3·a3 + d2·r4 ─────────────────── */
        asm volatile(
                "mov rax, %[A1]\n\t"
                "mul %[B1]\n\t"
                "mov r10, rax\n\t"
                "mov r11, rdx\n\t"
                "mov rax, %[A2]\n\t"
                "mul %[B2]\n\t"
                "add r10, rax\n\t"
                "adc r11, rdx\n\t"
                /* seed with carry from previous limb */
                "add r10, %[cin]\n\t"
                "adc r11, 0\n\t"
                "mov rax, r10\n\t"
                "mov r8, r11\n\t"
                "mov rcx, %[A3]\n\t"
                "mov rdx, %[B3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [A1] "r"(d0), [B1] "r"(a1),
                  [A2] "r"(r3), [B2] "r"(a3),
                  [A3] "r"(d2), [B3] "r"(r4),
                  [cin] "r"(carry)
                : "rax", "rcx", "rdx", "r8", "r10", "r11"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[1] = c_lo & MASK51;

        /* ── c[2] = d0·a2 + a1·a1 + d3·r4 ─────────────────── */
        asm volatile(
                "mov rax, %[A1]\n\t"
                "mul %[B1]\n\t"
                "mov r10, rax\n\t"
                "mov r11, rdx\n\t"
                "mov rax, %[A2]\n\t"
                "mul %[B2]\n\t"
                "add r10, rax\n\t"
                "adc r11, rdx\n\t"
                "add r10, %[cin]\n\t"
                "adc r11, 0\n\t"
                "mov rax, r10\n\t"
                "mov r8, r11\n\t"
                "mov rcx, %[A3]\n\t"
                "mov rdx, %[B3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [A1] "r"(d0), [B1] "r"(a2),
                  [A2] "r"(a1), [B2] "r"(a1),
                  [A3] "r"(d3), [B3] "r"(r4),
                  [cin] "r"(carry)
                : "rax", "rcx", "rdx", "r8", "r10", "r11"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[2] = c_lo & MASK51;

        /* ── c[3] = d0·a3 + d1·a2 + r4·a4 ─────────────────── */
        asm volatile(
                "mov rax, %[A1]\n\t"
                "mul %[B1]\n\t"
                "mov r10, rax\n\t"
                "mov r11, rdx\n\t"
                "mov rax, %[A2]\n\t"
                "mul %[B2]\n\t"
                "add r10, rax\n\t"
                "adc r11, rdx\n\t"
                "add r10, %[cin]\n\t"
                "adc r11, 0\n\t"
                "mov rax, r10\n\t"
                "mov r8, r11\n\t"
                "mov rcx, %[A3]\n\t"
                "mov rdx, %[B3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [A1] "r"(d0), [B1] "r"(a3),
                  [A2] "r"(d1), [B2] "r"(a2),
                  [A3] "r"(r4), [B3] "r"(a4),
                  [cin] "r"(carry)
                : "rax", "rcx", "rdx", "r8", "r10", "r11"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[3] = c_lo & MASK51;

        /* ── c[4] = d0·a4 + d1·a3 + a2·a2 ─────────────────── */
        asm volatile(
                "mov rax, %[A1]\n\t"
                "mul %[B1]\n\t"
                "mov r10, rax\n\t"
                "mov r11, rdx\n\t"
                "mov rax, %[A2]\n\t"
                "mul %[B2]\n\t"
                "add r10, rax\n\t"
                "adc r11, rdx\n\t"
                "add r10, %[cin]\n\t"
                "adc r11, 0\n\t"
                "mov rax, r10\n\t"
                "mov r8, r11\n\t"
                "mov rcx, %[A3]\n\t"
                "mov rdx, %[B3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [A1] "r"(d0), [B1] "r"(a4),
                  [A2] "r"(d1), [B2] "r"(a3),
                  [A3] "r"(a2), [B3] "r"(a2),
                  [cin] "r"(carry)
                : "rax", "rcx", "rdx", "r8", "r10", "r11"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[4] = c_lo & MASK51;

        /* Final reduction */
        out[0] += carry * 19;
        carry = out[0] >> 51;
        out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  PURE 1-MAC (15 vmwrite, for comparison)
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_1mac(const uint64_t *a, uint64_t *out) {
        uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
        uint64_t d0=2*a0,d1=2*a1,d2=2*a2,d3=2*a3;
        uint64_t r3=19*a3,r4=19*a4;
        uint64_t c_lo, c_hi, carry;
#define MAC1(xA,yA) "mov rcx,%[" xA "]\n\t" "mov rdx,%[" yA "]\n\t" "vmwrite rcx,rdx\n\t"

        asm volatile("xor rax,rax\n\t" "xor r8,r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[x1]"r"(a0),[y1]"r"(a0),[x2]"r"(d1),[y2]"r"(r4),[x3]"r"(d2),[y3]"r"(r3)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[0]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a1),[x2]"r"(r3),[y2]"r"(a3),[x3]"r"(d2),[y3]"r"(r4)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[1]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a2),[x2]"r"(a1),[y2]"r"(a1),[x3]"r"(d3),[y3]"r"(r4)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[2]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a3),[x2]"r"(d1),[y2]"r"(a2),[x3]"r"(r4),[y3]"r"(a4)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[3]=c_lo&MASK51;

        asm volatile("mov rax,%[cin]\n\t" "xor r8,r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo],rax\n\t" "mov %[hi],r8\n\t"
                :[lo]"=r"(c_lo),[hi]"=r"(c_hi)
                :[cin]"r"(carry),[x1]"r"(d0),[y1]"r"(a4),[x2]"r"(d1),[y2]"r"(a3),[x3]"r"(a2),[y3]"r"(a2)
                :"rax","rcx","rdx","r8");
        carry=(c_hi<<13)|(c_lo>>51); out[4]=c_lo&MASK51;
#undef MAC1
        out[0]+=carry*19; carry=out[0]>>51; out[0]&=MASK51; out[1]+=carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  HELPERS
 * ══════════════════════════════════════════════════════════════════ */
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
static inline uint64_t tsc_start(void) {
        uint32_t lo,hi;
        asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx");
        return ((uint64_t)hi<<32)|lo;
}
static inline uint64_t tsc_end(void) {
        uint32_t lo,hi;
        asm volatile("rdtscp\n\tmov %0,eax\n\tmov %1,edx\n\tcpuid"
                :"=r"(lo),"=r"(hi)::"rax","rbx","rcx","rdx");
        return ((uint64_t)hi<<32)|lo;
}
static void bench(const char *name, sq_fn fn, const uint64_t *input) {
        uint64_t timings[NUM_BATCHES],out[5];
        volatile uint64_t sink=0; (void)sink;
        for(int i=0;i<WARMUP_BATCHES;i++) for(int j=0;j<OPS_PER_BATCH;j++) fn(input,out);
        for(int b=0;b<NUM_BATCHES;b++){
                uint64_t t0=tsc_start();
                for(int j=0;j<OPS_PER_BATCH;j++) fn(input,out);
                uint64_t t1=tsc_end();
                timings[b]=t1-t0; sink=out[0];
        }
        qsort(timings,NUM_BATCHES,sizeof(uint64_t),cmp_u64);
        printf("  %-12s  median: %4"PRIu64" cyc/op   "
               "[p10: %4"PRIu64",  p90: %4"PRIu64"]\n",
               name, timings[NUM_BATCHES/2]/OPS_PER_BATCH,
               timings[NUM_BATCHES/10]/OPS_PER_BATCH,
               timings[NUM_BATCHES*9/10]/OPS_PER_BATCH);
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  Hybrid Native mul + Microcode MAC — Curve25519 Square\n");
        printf("==========================================================\n\n");

        uint64_t input[5] = {
                0x00062D608F25D51AULL,0x000412A4B4F6592AULL,
                0x00075B7171A4B31DULL,0x0001FF60527118FEULL,
                0x000216936D3CD6E5ULL
        };
        uint64_t ref[5];
        fe_sq_ref(input, ref);

        printf("Installing 5-triad MAC hook...\n\n");
        install_mac();

        /* ── Hybrid correctness ───────────────────────────────── */
        uint64_t out[5];
        fe_sq_hybrid(input, out);
        printf("  Hybrid correctness: ");
        if (fe_equal(ref, out)) {
                printf("PASS\n");
        } else {
                printf("FAIL\n");
                print_limbs("ref:", ref);
                print_limbs("hybrid:", out);
                return 1;
        }

        /* Iterated */
        {
                uint64_t r[5]={1,0,0,0,0},m[5]={1,0,0,0,0},tmp[5];
                for(int i=0;i<1000;i++){
                        fe_sq_ref(r,tmp);memcpy(r,tmp,40);
                        fe_sq_hybrid(m,tmp);memcpy(m,tmp,40);
                }
                printf("  Iterated (1000x): %s\n",fe_equal(r,m)?"PASS":"FAIL");
                if(!fe_equal(r,m)) return 1;
        }

        /* 1-MAC correctness */
        fe_sq_1mac(input, out);
        printf("  1-MAC correctness:  %s\n", fe_equal(ref,out)?"PASS":"FAIL");

        /* ── Benchmark ────────────────────────────────────────── */
        printf("\n");
        bench("ref (C)", fe_sq_ref, input);
        bench("hybrid", fe_sq_hybrid, input);
        bench("1mac", fe_sq_1mac, input);

        printf("\n==========================================================\n");
        printf("  Summary\n");
        printf("==========================================================\n");
        printf("  ref:     15 native mul + add/adc\n");
        printf("  hybrid:  10 native mul + 5 vmwrite MAC\n");
        printf("  1mac:    15 vmwrite MAC (pure microcode)\n");
        printf("\n");
        printf("  The hybrid trades 10 vmwrite redirects (~50 cyc)\n");
        printf("  for 10 native mul+add/adc (~30 cyc).\n");
        printf("  Net: ~20 cyc faster than pure microcode.\n");
        printf("\n");
        printf("  For CryptOpt integration: vmwrite is a new opcode\n");
        printf("  in the search space. CryptOpt can choose per-product\n");
        printf("  whether to use native mul or vmwrite MAC, optimizing\n");
        printf("  the schedule over the hybrid instruction mix.\n\n");

        return 0;
}
