/*
 * dump_slots.c — Dump all 32 match-and-patch slots with seqword bank analysis
 *
 * For each active slot, decodes:
 *   - Source (intercepted) ucode address
 *   - Destination (patch) ucode address
 *   - Seqword bank and start address the patch occupies
 *   - How many seqword entries it likely uses (estimated from uop region size)
 *
 * Goal: identify which slots conflict with the U7d7c region's seqword bank
 * so we know which ones to clear to make room for 55 triads.
 *
 * Build: make PROG=dump_slots
 * Run:   sudo taskset -c 0 ./dump_slots_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ldat.h"
#include "../../include/misc.h"

#define NUM_SLOTS 32

/* Decode a match-and-patch entry value.
 * Format: (dst << 16) | src_addr | enable_bit
 *   dst = patch_addr / 2
 *   src_addr = intercepted ucode addr (even)
 *   enable_bit = bit 0
 */
static void decode_entry(uint64_t val, uint64_t *src, uint64_t *patch_addr, int *enabled) {
    *enabled   = val & 1;
    *src       = val & 0xFFFE;          /* bits [15:1] << 1, mask out enable */
    *patch_addr = ((val >> 16) & 0xFFFF) * 2;
}

/* Compute seqword bank and start address for a patch RAM ucode address */
static void seqword_info(uint64_t uaddr, int *bank, int *seq_start) {
    uint64_t base = uaddr - 0x7c00;
    *bank      = base % 4;
    *seq_start = (base / 4) % 0x80;
}

/* Estimate triad count for a patch at patch_addr by scanning uop slots
 * until we hit a non-patched region or END_SEQWORD.
 * We read seqwords looking for END (uend bit pattern).
 */
static int estimate_triad_count(uint64_t patch_addr) {
    int bank, seq_start;
    seqword_info(patch_addr, &bank, &seq_start);

    int count = 0;
    int max_scan = 0x80 - seq_start;  /* max before wrap */
    if (max_scan > 64) max_scan = 64; /* safety cap */

    for (int i = 0; i < max_scan; i++) {
        uint64_t seq = ms_rw_seq_read(seq_start + i);
        if (seq == 0) break;  /* empty/unpatched */
        count++;
        /* Check for END_SEQWORD pattern: UEND bits set */
        if (seq & (3ULL << 24)) break;  /* UEND0/UEND1 bits */
    }
    return count;
}

int main(void) {
    assign_to_core(0);

    printf("=== Match-and-Patch Slot Dump with Seqword Bank Analysis ===\n\n");

    /* Target region analysis */
    int target_bank, target_seq;
    seqword_info(0x7d7c, &target_bank, &target_seq);
    printf("Target region U7d7c: bank=%d, seq_start=0x%02x\n", target_bank, target_seq);
    printf("  Max triads before wrap: 0x80 - 0x%02x = %d\n",
           target_seq, 0x80 - target_seq);
    printf("\n");

    printf("%-5s  %-8s  %-10s  %-8s  %-6s  %-10s  %-8s  %s\n",
           "Slot", "Enabled", "Src(hook)", "Dst(patch)", "Bank", "SeqStart", "~Triads", "Conflicts?");
    printf("-----  --------  ----------  ----------  ------  ----------  --------  ----------\n");

    int total_conflict = 0;
    int conflict_slots[NUM_SLOTS];
    int num_conflicts = 0;

    for (int i = 0; i < NUM_SLOTS; i++) {
        uint64_t raw = ms_match_n_patch_read(i);

        uint64_t src, patch_addr;
        int enabled;
        decode_entry(raw, &src, &patch_addr, &enabled);

        if (!enabled) {
            printf("%-5d  %-8s  —\n", i, "no");
            continue;
        }

        int bank, seq_start;
        int triads = 0;
        const char *conflict = "";

        if (patch_addr >= 0x7c00 && patch_addr < 0x7e00) {
            seqword_info(patch_addr, &bank, &seq_start);
            triads = estimate_triad_count(patch_addr);

            if (bank == target_bank) {
                conflict = "<-- CONFLICT";
                conflict_slots[num_conflicts++] = i;
                total_conflict += triads;
            }
        } else {
            /* Patch points outside patch RAM (e.g., slot 14: U209c -> U28d8,
             * slot 31: U58ba -> U017a). No patch RAM seqword used. */
            bank = -1;
            seq_start = 0;
            conflict = "(ROM redir)";
        }

        if (bank >= 0) {
            printf("%-5d  %-8s  U%04" PRIx64 "      U%04" PRIx64 "      %-6d  0x%02x        %-8d  %s\n",
                   i, "YES", src, patch_addr, bank, seq_start, triads, conflict);
        } else {
            printf("%-5d  %-8s  U%04" PRIx64 "      U%04" PRIx64 "      %-6s  %-10s  %-8s  %s\n",
                   i, "YES", src, patch_addr, "n/a", "n/a", "n/a", conflict);
        }
    }

    printf("\n=== Summary ===\n");
    printf("Target bank (U7d7c): %d\n", target_bank);
    printf("Conflicting slots: %d\n", num_conflicts);
    printf("Estimated seqword entries consumed by conflicts: %d\n", total_conflict);
    printf("Theoretical max at U7d7c: %d triads\n", 0x80 - target_seq);
    printf("Estimated available after conflicts: %d triads\n", 0x80 - target_seq - total_conflict);
    printf("Need: 55 triads\n");

    if (num_conflicts > 0) {
        printf("\n=== Conflicting slots (candidates to clear) ===\n");
        for (int c = 0; c < num_conflicts; c++) {
            int s = conflict_slots[c];
            uint64_t raw = ms_match_n_patch_read(s);
            uint64_t src, patch_addr;
            int enabled;
            decode_entry(raw, &src, &patch_addr, &enabled);

            int bank, seq_start;
            seqword_info(patch_addr, &bank, &seq_start);
            int triads = estimate_triad_count(patch_addr);

            printf("  Slot %2d: hooks U%04" PRIx64 " -> U%04" PRIx64
                   "  (bank %d, seq 0x%02x, ~%d triads)\n",
                   s, src, patch_addr, bank, seq_start, triads);
            printf("           Clear with: ldat_array_write(0x6a0, 3, 0, 0, %d, 0);\n", s);
        }

        printf("\nClearing all %d conflicting slots would free ~%d seqword entries in bank %d\n",
               num_conflicts, total_conflict, target_bank);
        printf("New estimated capacity at U7d7c: %d triads\n",
               0x80 - target_seq + total_conflict);
    }

    /* Also dump all 4 seqword banks occupancy */
    printf("\n=== Seqword Bank Occupancy (non-zero entries) ===\n");
    for (int bank = 0; bank < 4; bank++) {
        int occupied = 0;
        for (int a = 0; a < 0x80; a++) {
            uint64_t seq = ms_rw_seq_read(bank * 0x80 + a);
            if (seq != 0) occupied++;
        }
        printf("  Bank %d: %d/128 entries occupied%s\n",
               bank, occupied, bank == target_bank ? "  <-- target" : "");
    }

    return 0;
}
