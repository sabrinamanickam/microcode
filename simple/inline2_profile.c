/* inline2_profile.c — per-op cost profiler for the inline2 ladder.
 *
 * Compiles the entire benchmark with -DINLINE2_PROFILE, which swaps in the
 * profiling main() at the bottom of full_curve25519_inline2.c. Nothing here is
 * duplicated — same patches, same FE_* macros, same ladder_step/fe_invert.
 *
 * Build: make PROG=inline2_profile CFLAGS="-O3 -march=native -mtune=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./inline2_profile_static
 */
#define INLINE2_PROFILE
#include "full_curve25519_inline2.c"
