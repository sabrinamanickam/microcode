/* inline2_profile.c — build wrapper for the per-op profiler that lives inside
 * full_curve25519_inline2.c under -DINLINE2_PROFILE.
 *
 * Exists so the profiler uses the SAME patches, the SAME FE_* macros and the
 * SAME ladder as the benchmark binary; nothing here is a second copy.
 *
 * Build: make PROG=inline2_profile EXTRA_CPPFLAGS=-DINLINE2_PROFILE
 * Run:   sudo taskset -c 0 ./inline2_profile_static     (pin the core first) */
#define INLINE2_PROFILE
#include "full_curve25519_inline2.c"
