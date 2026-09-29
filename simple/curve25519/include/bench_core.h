/*
 * bench_core.h — which physical core a curve25519 harness pins itself to.
 *
 * Every harness calls assign_to_core() (a plain sched_setaffinity) in main, so
 * `taskset -c N` on the command line is NOT enough: the process re-pins itself
 * and overrides it. This makes the core selectable at run time instead.
 *
 *   default                      core 0 (the core every pre-2026-09 result used)
 *   BENCH_CORE=1 ./harness       core 1
 *
 * patch_ucode() and hook_match_and_patch() act on whichever core executes
 * them, so installing and firing on another core is symmetric -- as long as
 * both happen on the SAME core, which is why assign_to_core() runs before the
 * patch is installed. Mirrors keccak/bench_core.h.
 */
#ifndef C25519_BENCH_CORE_H
#define C25519_BENCH_CORE_H

#include <stdlib.h>

static inline int bench_core(void)
{
    const char *e = getenv("BENCH_CORE");
    if (!e || !*e) return 0;
    return (int)strtol(e, NULL, 10);
}

/* Pin to BENCH_CORE and print where we actually landed, so every log carries
 * its own proof of the core it was measured on (paper_eval.sh checks it).
 * Needs misc.h's assign_to_core, so include this after misc.h. */
#ifdef MISC_H_
#include <sched.h>
#include <stdio.h>
static inline void bench_pin(void)
{
    int c = bench_core();
    assign_to_core(c);
    printf("PINNED core=%d sched_getcpu=%d\n", c, sched_getcpu());
}
#endif

#endif /* C25519_BENCH_CORE_H */
