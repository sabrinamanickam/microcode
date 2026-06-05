/* inline3_profile.c — per-op profiler for the inline3 ladder (output-reg-
 * resident z3 chain). Same mechanism as inline2_profile.c: reuses the
 * INLINE2_PROFILE main at the bottom of full_curve25519_inline3.c.
 *
 * Build: make PROG=inline3_profile CFLAGS="-O3 -march=native -mtune=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./inline3_profile_static
 */
#define INLINE2_PROFILE
#include "full_curve25519_inline3.c"
