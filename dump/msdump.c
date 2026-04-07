// Build:
//   # If libmicro is C (most builds):
//   gcc -O2 -Wall -I/path/to/lib-micro/include \
//       msdump.c \
//       -L/path/to/lib-micro/build -Wl,-rpath,/path/to/lib-micro/build \
//       -lmicro -o msdump
//
//   # If libmicro is C++ (symbols mangled): rename to .cpp and use g++
//   g++ -O2 -Wall -I/path/to/lib-micro/include \
//       msdump.cpp \
//       -L/path/to/lib-micro/build -Wl,-rpath,/path/to/lib-micro/build \
//       -lmicro -o msdump
//
// Run:
//   sudo ./msdump --array 0                # writes ms_array0.txt (default range 0x0..0x7e00)
//   sudo ./msdump --array 0 --start 0x0 --end 0x7e00 --out mydump.txt
//   sudo ./msdump --array 0 --probe        # just checks access

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

// lib-micro headers
#include "dump.h"
#include "udbg.h"
#include "patch.h"
#include "ldat.h"   // ldat_array_read()

typedef uint64_t u64;
#define PORT_MS 0x6a0ULL
#define W48(x) ((unsigned long)((x) & 0xFFFFFFFFFFFFULL))

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --array <0..4> [--start <hex>] [--end <hex>] [--step <dec>] [--out <file>] [--probe]\n"
        "Arrays:\n"
        "  0 = RO uops (ROM)         typical 0x0000..0x7e00\n"
        "  1 = RO seqwords           typical 0x0000..0x8000\n"
        "  2 = RW seqwords (window)  typical 0x0000..0x0100\n"
        "  3 = match & patch table   typical 0x0000..0x0020\n"
        "  4 = RW uops (window)      typical 0x7c00..0x7e00\n", prog);
}

// ---- SIGILL guard (clean error on locked systems) ----
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
        ok = 0;
    }
    sigaction(SIGILL, &old, NULL);
    return ok;
}

// ---- dumper (to FILE*, with 48-bit masking) ----
static void ms_array_dump_to_file(FILE *out, u64 array_sel, u64 fast_addr, u64 size, u64 step) {
    if (step == 0) step = 4;
    for (; fast_addr < size; fast_addr += step) {
        u64 v0 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 0);
        u64 v1 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 1);
        u64 v2 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 2);
        u64 v3 = ldat_array_read(PORT_MS, array_sel, 0, 0, fast_addr + 3);
        // Two spaces after ':' to match common tooling
        fprintf(out, "%04lx:  %012lx %012lx %012lx %012lx\n",
                (unsigned long)fast_addr,
                W48(v0), W48(v1), W48(v2), W48(v3));
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
    u64 start = 0x0, end = 0, step = 4;
    int do_probe_only = 0;
    const char *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--array") && i + 1 < argc) {
            array = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--start") && i + 1 < argc) {
            if (parse_hex(argv[++i], &start)) { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--end") && i + 1 < argc) {
            if (parse_hex(argv[++i], &end))   { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--step") && i + 1 < argc) {
            if (parse_dec(argv[++i], &step))  { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--out") && i + 1 < arg) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--probe")) {
            do_probe_only = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]); usage(argv[0]); return 1;
        }
    }

    if (array < 0 || array > 4) { fprintf(stderr, "Error: --array must be 0..4\n"); return 1; }

    // Default ranges
    if (end == 0) {
        switch (array) {
            case 0: end = 0x7e00; break;
            case 1: end = 0x8000; break;
            case 2: end = 0x0100; break;
            case 3: end = 0x0020; break;
            case 4: end = 0x7e00; break;
        }
    }
    if (start >= end) {
        fprintf(stderr, "Error: start (0x%llx) must be < end (0x%llx)\n",
                (unsigned long long)start, (unsigned long long)end);
        return 1;
    }

    // Access probe
    if (!probe_ms_access(array, start)) {
        fprintf(stderr,
            "Microsequencer access not available (UDBG/LDAT produced SIGILL).\n"
            "Likely not red-unlocked or running under a hypervisor.\n");
        return 2;
    }
    if (do_probe_only) {
        fprintf(stderr, "Probe succeeded: UDBG/LDAT reads are permitted.\n");
        return 0;
    }

    // Choose default output file if not provided
    char default_name[64];
    if (!out_path) {
        snprintf(default_name, sizeof(default_name), "ms_array%d.txt", array);
        out_path = default_name;
    }

    FILE *out = fopen(out_path, "w");
    if (!out) { perror("fopen(out)"); return 1; }

    // Header exactly like you showed: "array 00:"
    fprintf(out, "array %02d:\n", array);

    ms_array_dump_to_file(out, (u64)array, start, end, step);
    fclose(out);

    fprintf(stderr, "Wrote %s\n", out_path);
    return 0;
}
