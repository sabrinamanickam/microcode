#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"
#include "../../include/udbg.h"

u64  ms_array_read (u64 array_sel, u64 bank_sel, u64 dword_idx, u64 fast_addr);
void ms_array_write(u64 array_sel, u64 bank_sel, u64 dword_idx, u64 fast_addr, u64 val);

#define SEQW_GOTO(addr)  ( SEQ_GOTO2(addr) | SEQ_NOSYNC )
#define UOP_ADDR(t, s)   ( ((u64)(t) * 4) + (s) )
#define SEQW_ARRAY       2
#define PATCH_ADDR       0x7c4c
#define HOOK_ENTRY       0x0428

#define SERIALIZE() do { \
    u32 _a,_b,_c,_d; \
    asm volatile("mfence\n\tcpuid\n\tmfence" \
        : "=a"(_a),"=b"(_b),"=c"(_c),"=d"(_d) : "a"(0) : "memory"); \
} while(0)

void yolo(void) {
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP1,0xface), MOVE_DSZ64_DI(TMP1,0xbead), NOP, NOP_SEQWORD },
        { NOP, MOVE_DSZ64_DI(RBX,0xbead), MOVE_DSZ64_DR(RAX,TMP1), END_SEQWORD }
    };
    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(PATCH_ADDR, p, ARRAY_SZ(p));
    hook_match_and_patch(0, HOOK_ENTRY, PATCH_ADDR);
}

static inline u64 tsc_start(void) {
    u32 lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo),"=d"(hi) : "a"(0) : "rbx","rcx");
    return ((u64)hi<<32)|lo;
}
static inline u64 tsc_end(void) {
    u32 lo, hi;
    asm volatile("rdtscp" : "=a"(lo),"=d"(hi) :: "rcx");
    return ((u64)hi<<32)|lo;
}

/*
 * TEST: CRC_SEQ hypothesis
 *
 * All confirmed seqwords in the array have bits 28-29 set:
 *   NOP_SEQWORD raw = 0x018000c0, stored as 0x300000c0
 *   END_SEQWORD raw = 0x030000f2, stored as 0x030000f2 (already has bits 28-29)
 *   Our SEQW_GOTO   = 0x01fc4c80, stored as 0x01fc4c80 (bits 28-29 zero)
 *
 * The sequencer may require valid CRC parity in bits 28-29 before
 * accepting a seqword. CRC_SEQ() computes and inserts those bits.
 *
 * This test writes CRC_SEQ(SEQW_GOTO(PATCH_ADDR)) and fires rdrand.
 * No CRBUS writes — safe to run.
 *
 * Three variants tried in sequence:
 *   A: raw SEQW_GOTO (what we had before — known to not work)
 *   B: CRC_SEQ(SEQW_GOTO) — parity bits added
 *   C: SEQ_GOTO1 instead of SEQ_GOTO2 — executes only uop0+uop1 before jump
 *      (in case uop2 of the original rdrand triad causes issues)
 */
void test_seqword_variants(void) {
    printf("=== seqword variant test ===\n");

    ucode_t payload[] = {
        { MOVE_DSZ64_DI(TMP1,0xface), MOVE_DSZ64_DI(TMP1,0xbead), NOP, NOP_SEQWORD },
        { NOP, MOVE_DSZ64_DI(RBX,0xbead), MOVE_DSZ64_DR(RAX,TMP1), END_SEQWORD }
    };
    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(PATCH_ADDR, payload, ARRAY_SZ(payload));
    hook_match_and_patch(0, 0xFFFE, PATCH_ADDR);

    u64 sw_fa   = UOP_ADDR(HOOK_ENTRY, 3);
    u64 orig_sw = ms_array_read(SEQW_ARRAY, 0, 0, sw_fa);
    printf("  orig seqw = 0x%016" PRIx64 "\n\n", orig_sw);

    u64 variants[] = {
        SEQW_GOTO(PATCH_ADDR),                          /* A: raw */
        CRC_SEQ(SEQW_GOTO(PATCH_ADDR)),                 /* B: with CRC */
        SEQ_GOTO1(PATCH_ADDR) | SEQ_NOSYNC,             /* C: GOTO1 raw */
        CRC_SEQ(SEQ_GOTO1(PATCH_ADDR) | SEQ_NOSYNC),   /* D: GOTO1 with CRC */
        SEQ_GOTO0(PATCH_ADDR) | SEQ_NOSYNC,             /* E: GOTO0 raw */
        CRC_SEQ(SEQ_GOTO0(PATCH_ADDR) | SEQ_NOSYNC),   /* F: GOTO0 with CRC */
    };
    const char *labels[] = { "A:GOTO2 raw", "B:GOTO2+CRC",
                              "C:GOTO1 raw", "D:GOTO1+CRC",
                              "E:GOTO0 raw", "F:GOTO0+CRC" };

    for (int i = 0; i < 6; i++) {
        ms_array_write(SEQW_ARRAY, 0, 0, sw_fa, variants[i]);
        u64 rb = ms_array_read(SEQW_ARRAY, 0, 0, sw_fa);
        SERIALIZE();

        uint64_t rax;
        asm volatile("rdrand %0\n\t" : "=a"(rax));

        printf("  %s  wrote=0x%016" PRIx64 "  rb=0x%016" PRIx64
               "  rdrand=%lx  %s\n",
               labels[i], variants[i], rb, rax,
               rax == 0xbead ? "PASS" : "fail");
    }

    /* Restore */
    ms_array_write(SEQW_ARRAY, 0, 0, sw_fa, orig_sw);
    printf("\n  restored to 0x%016" PRIx64 "\n",
           ms_array_read(SEQW_ARRAY, 0, 0, sw_fa));
}

int main(void) {
    uint64_t rax;

    yolo();
    asm volatile("rdrand %0\n\t" : "=a"(rax));
    printf("yolo: a=%lx\n", rax);
    u64 t0 = tsc_start();
    for (int i = 0; i < 1000; i++)
        asm volatile("rdrand %0\n\t" : "=a"(rax));
    printf("yolo: cycles/call=%" PRIu64 "\n\n", (tsc_end() - t0) / 1000);

    test_seqword_variants();

    return 0;
}
