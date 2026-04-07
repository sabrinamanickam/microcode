// mSDump: dump microsequencer arrays using lib-micro
// Build:
//   gcc -O2 -Wall -I/path/to/lib-micro/include \
//       -L/path/to/lib-micro/build -Wl,-rpath,/path/to/lib-micro/build \
//       -lmicro -o msdump msdump.c
// Run (examples):
//   sudo ./msdump --array 0 --start 0x0    --end 0x7e00   > ms_array0.txt
//   sudo ./msdump --array 1 --start 0x0    --end 0x8000   > ms_array1.txt
// Notes:
//   • Requires a red-unlocked Intel CPU (UDBG/LDAT active) on bare metal.
//   • If UDBG is disabled, this program will print a clear error and exit.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

// lib-micro headers (from the repo)
#include "dump.h"
#include "udbg.h"
#include "patch.h"
#include "ldat.h"   // for ldat_array_read()

typedef uint64_t u64;
#define PORT_MS 0x6a0ULL
#define W48(x) ((unsigned long)((x) & 0xFFFFFFFFFFFFULL))

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --array <0..4> [--start <hex>] [--end <hex>] [--step <dec>] [--probe]\n"
        "Arrays:\n"
        "  0 = RO uops (ROM)         typical 0x0000..0x7e00\n"
        "  1 = RO seqwords           typical 0x0000..0x8000\n"
        "  2 = RW seqwords (window)  typical 0x0000..0x0100\n"
        "  3 = match & patch table   typical 0x0000..0x0020\n"
        "  4 = RW uops (window)      typical 0x7c00..0x7e00\n"
        "Examples:\n"
        "  %s --array 0 --start 0x0    --end 0x7e00\n"
        "  %s --array 1 --start 0x0    --end 0x8000\n"
        "  %s --array 4 --start 0x7c00 --end 0x7e00\n",
        prog, prog, prog, prog);
}

// ----- SIGILL guard so we fail gracefully -----
static sigjmp_buf jmp_env;
static void on_sigill(int sig) { (void)sig; siglongjmp(jmp_env, 1); }

static int probe_ms_access(int array, u64 addr) {
    struct sigaction sa = {0}, old = {0};
    sa.sa_handler = on_sigill;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old);

    int ok = 1;
    if (sigsetjmp(jmp_env, 1) == 0) {
        (void) ldat_array_read(PORT_MS, (u64)array, 0, 0, addr);
    } else {
        ok = 0;  // hit SIGILL
    }
    sigaction(SIGILL, &old, NULL);
    return ok;
}

// ----- Your dumper (with 48-bit masking, and optional step) -----
static void ms_array_dump_loop(u64 array_sel, u64 fast_addr, u64 size, u64 step) {
    if (step == 0) step = 4;
    for (; fast_addr < size; fast_addr += step) {
        u64 val0 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 0);
        u64 val1 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 1);
        u64 val2 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 2);
        u64 val3 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 3);
        // Two spaces after ':' to match common disassembler-friendly formatting
        printf("%04lx:  %012lx %012lx %012lx %012lx\n",
               (unsigned long)fast_addr,
               W48(val0), W48(val1), W48(val2), W48(val3));
    }
}

static int parse_hex(const char *s, u64 *out) {
    char *end = NULL; errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || end == s || *end != '\0') return -1;
    *out = (u64)v; return 0;
}

static int parse_dec(const char *s, u64 *out) {
    char *end = NULL; errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0') return -1;
    *out = (u64)v; return 0;
}

int main(int argc, char **argv) {
    int array = -1;
    u64 start = 0x0;
    u64 end   = 0;          // 0 => choose defaults
    u64 step  = 4;
    int do_probe_only = 0;

    // Args
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--array") && i + 1 < argc) {
            array = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--start") && i + 1 < argc) {
            if (parse_hex(argv[++i], &start)) { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--end") && i + 1 < argc) {
            if (parse_hex(argv[++i], &end))   { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--step") && i + 1 < argc){
            if (parse_dec(argv[++i], &step))  { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--probe")) {
            do_probe_only = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    if (array < 0 || array > 4) {
        fprintf(stderr, "Error: --array must be 0..4\n");
        usage(argv[0]); return 1;
    }

    // Defaults for 'end' if not provided
    if (end == 0) {
        switch (array) {
            case 0: end = 0x7e00; break; // RO uops
            case 1: end = 0x8000; break; // RO seqwords
            case 2: end = 0x0100; break; // RW seqwords window
            case 3: end = 0x0020; break; // match & patch entries
            case 4: end = 0x7e00; break; // RW uops (maps peculiarly)
        }
    }

    if (start >= end) {
        fprintf(stderr, "Error: start (0x%llx) must be < end (0x%llx)\n",
                (unsigned long long)start, (unsigned long long)end);
        return 1;
    }

    // Probe access once so we can error out cleanly on non-red-unlocked systems.
    if (!probe_ms_access(array, start)) {
        fprintf(stderr,
            "Microsequencer access not available (UDBG/LDAT produced SIGILL).\n"
            "This typically means the CPU is not red-unlocked or you are in a VM.\n");
        return 2;
    }
    if (do_probe_only) {
        fprintf(stderr, "Probe succeeded: UDBG/LDAT reads are permitted.\n");
        return 0;
    }

    fprintf(stderr, "Dumping ms_array %d from 0x%lx to 0x%lx (step %lu)\n",
            array, (unsigned long)start, (unsigned long)end, (unsigned long)step);

    ms_array_dump_loop((u64)array, start, end, step);
    return 0;
}
