/*
 * test_memops.c — Probe LDZX / STAD from microcode (ASZ32 only)
 *
 * Only ASZ32 opcodes exist for LDZX/STAD. Addresses must fit 32 bits.
 * We use mmap(MAP_32BIT) to guarantee allocations in the low 4GB.
 *
 * BUILD:  make PROG=test_memops
 * RUN:    sudo ./test_memops_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static void do_patch(ucode_t *patch, int n_triads) {
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, n_triads);
        hook_match_and_patch(0, 0x0428, 0x7c00);
}

/* Allocate n bytes in the low 4GB */
static void *alloc32(size_t n) {
        void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) {
                perror("mmap MAP_32BIT");
                return NULL;
        }
        return p;
}

#ifndef WORKING_SEG
#define WORKING_SEG 3
#endif


/* ══════════════════════════════════════════════════════════════════
 * PROBE 1: Segment sweep for LDZX
 * ══════════════════════════════════════════════════════════════════ */
static void probe_seg_sweep(void) {
        printf("=== PROBE 1: LDZX ASZ32 segment sweep ===\n");

        uint64_t *buf = alloc32(4096);
        if (!buf) return;
        buf[0] = 0xDEADBEEFCAFE1234ULL;

        printf("  Buffer at: 0x%016" PRIx64 " (%s 32-bit)\n\n",
               (uint64_t)buf,
               (uint64_t)buf <= 0xFFFFFFFFULL ? "fits in" : "EXCEEDS");

        int segs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8,
                       0x10, 0x18, 0x1a, 0x1b, 0x1c, 0x1e, 0x1f };
        int n = sizeof(segs) / sizeof(segs[0]);

        for (int i = 0; i < n; i++) {
                int seg = segs[i];

                ucode_t patch[] = {
                        {
                                LDZX_DSZ64_ASZ32_SC1_DR(TMP0, RCX, seg),
                                NOP, NOP, NOP_SEQWORD
                        },
                        {
                                MOVE_DSZ64_DR(RAX, TMP0),
                                NOP, NOP, END_SEQWORD
                        }
                };
                do_patch(patch, 2);

                uint64_t rax_out;
                asm volatile(
                        "mov rcx, %[a]\n\t"
                        "rdrand rax\n\t"
                        "mov %[out], rax\n\t"
                        : [out] "=r"(rax_out)
                        : [a] "r"(buf)
                        : "rax", "rcx", "rdx"
                );

                int ok = (rax_out == 0xDEADBEEFCAFE1234ULL);
                printf("  seg=0x%02x: RAX=0x%016" PRIx64 " %s\n",
                       seg, rax_out, ok ? "✓ MATCH" : "");
        }
        printf("\n");
        munmap(buf, 4096);
}


/* ══════════════════════════════════════════════════════════════════
 * PROBE 2: LDZX with offset
 * ══════════════════════════════════════════════════════════════════ */
static void probe_ldzx_offset(void) {
        printf("=== PROBE 2: LDZX with offset (seg=0x%02x) ===\n", WORKING_SEG);

        uint64_t *buf = alloc32(4096);
        if (!buf) return;
        buf[0] = 0x1111111111111111ULL;
        buf[1] = 0x2222222222222222ULL;
        buf[2] = 0x3333333333333333ULL;
        buf[3] = 0x4444444444444444ULL;
        buf[4] = 0x5555555555555555ULL;

        struct { int off; uint64_t expect; } tests[] = {
                { 0x00, 0x1111111111111111ULL },
                { 0x08, 0x2222222222222222ULL },
                { 0x10, 0x3333333333333333ULL },
                { 0x18, 0x4444444444444444ULL },
                { 0x20, 0x5555555555555555ULL },
        };
        int n = sizeof(tests) / sizeof(tests[0]);

        for (int i = 0; i < n; i++) {
                ucode_t patch[2];
                if (tests[i].off == 0) {
                        patch[0] = (ucode_t){
                                LDZX_DSZ64_ASZ32_SC1_DR(TMP0, RCX, WORKING_SEG),
                                NOP, NOP, NOP_SEQWORD };
                } else {
                        patch[0] = (ucode_t){
                                LDZX_DSZ64_ASZ32_SC1_DRI(TMP0, RCX, tests[i].off, WORKING_SEG),
                                NOP, NOP, NOP_SEQWORD };
                }
                patch[1] = (ucode_t){ MOVE_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD };
                do_patch(patch, 2);

                uint64_t rax_out;
                asm volatile(
                        "mov rcx, %[a]\n\t"
                        "rdrand rax\n\t"
                        "mov %[out], rax\n\t"
                        : [out] "=r"(rax_out)
                        : [a] "r"(buf)
                        : "rax", "rcx", "rdx"
                );

                int ok = (rax_out == tests[i].expect);
                printf("  [+0x%02x]: 0x%016" PRIx64 " %s\n",
                       tests[i].off, rax_out, ok ? "✓" : "✗");
        }
        printf("\n");
        munmap(buf, 4096);
}


/* ══════════════════════════════════════════════════════════════════
 * PROBE 3: STAD basic — Store RCX → [RDX]
 * ══════════════════════════════════════════════════════════════════ */
static void probe_stad(void) {
        printf("=== PROBE 3: STAD RCX → [RDX] (seg=0x%02x) ===\n", WORKING_SEG);

        ucode_t patch[] = {
                {
                        STAD_DSZ64_ASZ32_SC1_RR(RCX, RDX, WORKING_SEG),
                        NOP, NOP, END_SEQWORD
                }
        };
        do_patch(patch, 1);

        uint64_t *buf = alloc32(4096);
        if (!buf) return;
        buf[0] = 0;

        uint64_t src_val = 0xCAFEBABE12345678ULL;
        asm volatile(
                "mov rcx, %[val]\n\t"
                "mov rdx, %[addr]\n\t"
                "rdrand rax\n\t"
                :
                : [val] "r"(src_val), [addr] "r"(buf)
                : "rax", "rcx", "rdx", "memory"
        );

        printf("  buf[0] = 0x%016" PRIx64 "  (expect 0xCAFEBABE12345678)\n", buf[0]);
        printf("  %s\n\n", buf[0] == 0xCAFEBABE12345678ULL ? "PASS" : "FAIL");
        munmap(buf, 4096);
}


/* ══════════════════════════════════════════════════════════════════
 * PROBE 4: STAD with offset — Store RCX → [RDX + 16]
 * ══════════════════════════════════════════════════════════════════ */
static void probe_stad_offset(void) {
        printf("=== PROBE 4: STAD RCX → [RDX+16] (seg=0x%02x) ===\n", WORKING_SEG);

        ucode_t patch[] = {
                {
                        STAD_DSZ64_ASZ32_SC1_RRI(RCX, RDX, 16, WORKING_SEG),
                        NOP, NOP, END_SEQWORD
                }
        };
        do_patch(patch, 1);

        uint64_t *buf = alloc32(4096);
        if (!buf) return;
        buf[0] = 0xAAAAAAAAAAAAAAAAULL;
        buf[1] = 0xBBBBBBBBBBBBBBBBULL;
        buf[2] = 0xCCCCCCCCCCCCCCCCULL;

        uint64_t src_val = 0x9999999999999999ULL;
        asm volatile(
                "mov rcx, %[val]\n\t"
                "mov rdx, %[addr]\n\t"
                "rdrand rax\n\t"
                :
                : [val] "r"(src_val), [addr] "r"(buf)
                : "rax", "rcx", "rdx", "memory"
        );

        int ok = (buf[0] == 0xAAAAAAAAAAAAAAAAULL &&
                  buf[1] == 0xBBBBBBBBBBBBBBBBULL &&
                  buf[2] == 0x9999999999999999ULL);
        printf("  buf[0] = 0x%016" PRIx64 "%s\n", buf[0],
               buf[0] == 0xAAAAAAAAAAAAAAAAULL ? "" : " CHANGED!");
        printf("  buf[1] = 0x%016" PRIx64 "%s\n", buf[1],
               buf[1] == 0xBBBBBBBBBBBBBBBBULL ? "" : " CHANGED!");
        printf("  buf[2] = 0x%016" PRIx64 "  (expect 0x9999999999999999)\n", buf[2]);
        printf("  %s\n\n", ok ? "PASS" : "FAIL");
        munmap(buf, 4096);
}


/* ══════════════════════════════════════════════════════════════════
 * PROBE 5: Round-trip — Load + add 1 + store
 * ══════════════════════════════════════════════════════════════════ */
static void probe_roundtrip(void) {
        printf("=== PROBE 5: Round-trip LDZX + ADD + STAD ===\n");

        ucode_t patch[] = {
                {
                        LDZX_DSZ64_ASZ32_SC1_DR(TMP0, RCX, WORKING_SEG),
                        NOP, NOP, NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRI(TMP0, TMP0, 1),
                        NOP, NOP, NOP_SEQWORD
                },
                {
                        STAD_DSZ64_ASZ32_SC1_RR(TMP0, RDX, WORKING_SEG),
                        NOP, NOP, END_SEQWORD
                }
        };
        do_patch(patch, 3);

        uint64_t *buf = alloc32(4096);
        if (!buf) return;
        buf[0] = 41;   /* source */
        buf[1] = 0;    /* dest */

        asm volatile(
                "mov rcx, %[saddr]\n\t"
                "mov rdx, %[daddr]\n\t"
                "rdrand rax\n\t"
                :
                : [saddr] "r"(&buf[0]), [daddr] "r"(&buf[1])
                : "rax", "rcx", "rdx", "memory"
        );

        printf("  src = %" PRIu64 "  (expect 41)\n", buf[0]);
        printf("  dst = %" PRIu64 "  (expect 42)\n", buf[1]);
        printf("  %s\n\n", (buf[1] == 42 && buf[0] == 41) ? "PASS" : "FAIL");
        munmap(buf, 4096);
}


/* ══════════════════════════════════════════════════════════════════
 * PROBE 6: Load two limbs + MUL — the carry_square pattern
 * ══════════════════════════════════════════════════════════════════ */
static void probe_load_mul(void) {
        printf("=== PROBE 6: LDZX + MUL (carry_square pattern) ===\n");

        ucode_t patch[] = {
                /* Triad 0: Load buf[0]→RCX, buf[1]→RDX */
                {
                        LDZX_DSZ64_ASZ32_SC1_DR(RCX, RSI, WORKING_SEG),
                        LDZX_DSZ64_ASZ32_SC1_DRI(RDX, RSI, 8, WORKING_SEG),
                        NOP, NOP_SEQWORD
                },
                /* Triad 1: MUL → TMP0(hi):RDX(lo) */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP, NOP, NOP_SEQWORD
                },
                /* Triad 2: Output lo→RAX, hi→R8 */
                {
                        MOVE_DSZ64_DR(RAX, RDX),
                        MOVE_DSZ64_DR(R8, TMP0),
                        NOP, END_SEQWORD
                }
        };
        do_patch(patch, 3);

        uint64_t *buf = alloc32(4096);
        if (!buf) return;
        buf[0] = 7;
        buf[1] = 13;

        uint64_t rax_out, r8_out;
        asm volatile(
                "mov rsi, %[addr]\n\t"
                "rdrand rax\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(rax_out), [hi] "=r"(r8_out)
                : [addr] "r"(buf)
                : "rax", "rcx", "rdx", "rsi", "r8"
        );

        printf("  RAX (lo) = %" PRIu64 "  (expect 91)\n", rax_out);
        printf("  R8  (hi) = %" PRIu64 "  (expect 0)\n", r8_out);
        printf("  %s\n\n", (rax_out == 91 && r8_out == 0) ? "PASS" : "FAIL");
        munmap(buf, 4096);
}


int main(void) {
        printf("╔══════════════════════════════════════════════╗\n");
        printf("║  LDZX / STAD Microcode Memory Probes          ║\n");
        printf("╚══════════════════════════════════════════════╝\n\n");
        printf("WORKING_SEG = 0x%02x\n\n", WORKING_SEG);

        /* Step 1: Find working segment */
        probe_seg_sweep();

        /* Step 2: Functional tests with WORKING_SEG */
        probe_ldzx_offset();
        probe_stad();
        probe_stad_offset();
        probe_roundtrip();
        probe_load_mul();

        return 0;
}
