#include <stdio.h>
#include <stdint.h>
#include "../../include/dump.h"
#include "../../include/ldat.h"
#include "../../include/patch.h" // path may be lib-dependent; adjust if needed

// Forward decl from source/dump.c (or include the header if one exists)
void ms_array_dump(uint64_t array_sel, uint64_t fast_addr, uint64_t size);

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <array_idx>\n", argv[0]);
        fprintf(stderr, "Arrays: 0=RO code, 1=RO seq, 2=RW seq, 3=MnP, 4=RW code\n");
        return 1;
    }
    uint64_t a = strtoull(argv[1], NULL, 0);

    switch (a) {
        case 0: ms_array_dump(0, 0, 0x7e0*4); break;  // 0..0x7dff triads → 0..0x7e00 addralign
        case 1: ms_array_dump(1, 0, 0x8000);  break;
        case 2: ms_array_dump(2, 0, 0x80);    break;
        case 3: ms_array_dump(3, 0, 0x20);    break;
        case 4: ms_array_dump(4, 0, 0x200);   break;
        default: fprintf(stderr, "Bad array idx\n"); return 2;
    }
    return 0;
}

