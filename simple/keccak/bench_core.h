/*
 * bench_core.h — which physical core a Keccak harness pins itself to.
 *
 * Every harness calls assign_to_core() (a plain sched_setaffinity) in main, so
 * `taskset -c N` on the command line is NOT enough: the process re-pins itself
 * and overrides it. This makes the core selectable at run time instead.
 *
 *   default                      core 0 (what every published number was taken on)
 *   BENCH_CORE=1 ./harness       core 1
 *
 * Nothing else in the patching path is core-specific: patch_ucode() and
 * hook_match_and_patch() act on whichever core executes them, so installing and
 * firing from another core is symmetric — as long as both happen on the SAME
 * core, which is why assign_to_core() runs before the patch is installed.
 */
#ifndef KECCAK_BENCH_CORE_H
#define KECCAK_BENCH_CORE_H

#include <stdlib.h>

static inline int bench_core(void)
{
    const char *e = getenv("BENCH_CORE");
    if (!e || !*e) return 0;
    return (int)strtol(e, NULL, 10);
}

#endif /* KECCAK_BENCH_CORE_H */
