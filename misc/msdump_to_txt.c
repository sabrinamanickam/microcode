// Build: gcc -O2 -Wall -Ilibmicro/include -Llibmicro/build -lmicro -o msdump_to_txt msdump_to_txt.c
// Run:   sudo ./msdump_to_txt ms_array0.txt ms_array1.txt

#include <stdio.h>
#include <stdint.h>
#include "../include/ldat.h"
#include "../include/patch.h"
#include "../include/dump.h"
#include "../source/patch.c"
#include "../source/dump.c"
#include "../source/ldat.c"

 // lib-micro API: ms_array_*_read, or ms_ro_*_read

typedef uint64_t u64;

// Read 48-bit "words" returned in u64 (low bits used). Print 12 hex digits each.
static u64 rd_ms0(u64 addr){ return ms_ro_code_read(addr); } // array 0 (RO uops)
static u64 rd_ms1(u64 addr){ return ms_ro_seq_read(addr); }  // array 1 (RO seqwords)

static void dump_array(FILE *f, u64 (*rd)(u64), u64 start, u64 end) {
    for (u64 a = start; a < end; a += 4) {
        u64 v0 = rd(a+0), v1 = rd(a+1), v2 = rd(a+2), v3 = rd(a+3);
        // match uCodeDisasm’s expected layout: "0000:  0123456789ab  ...  "
        fprintf(f, "%04lx:  %012lx %012lx %012lx %012lx\n",
                (unsigned long)a,
                (unsigned long)v0, (unsigned long)v1,
                (unsigned long)v2, (unsigned long)v3);
    }
}

int main(int argc, char **argv){
    const char *out0 = (argc > 1) ? argv[1] : "ms_array0.txt";
    const char *out1 = (argc > 2) ? argv[2] : "ms_array1.txt";

    FILE *f0 = fopen(out0, "w"); if(!f0){ perror("ms_array0.txt"); return 1; }
    FILE *f1 = fopen(out1, "w"); if(!f1){ perror("ms_array1.txt"); return 1; }

    // Goldmont ROM spans the usual ranges; safe caps shown in lib-micro docs.
    dump_array(f0, rd_ms0, 0x0000, 0x7e00);   // RO uops (array 0)
    dump_array(f1, rd_ms1, 0x0000, 0x8000);   // RO seqwords (array 1)

    fclose(f0); fclose(f1);
    return 0;
}

