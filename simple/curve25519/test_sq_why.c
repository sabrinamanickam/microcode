/*
 * test_sq_why.c — separate the two surviving theories for the fe_sq reset.
 *
 * Established over three boots, all fsync-logged: the five-accumulator
 * fe_sq patch is CORRECT (matches fiat-crypto standalone, through both
 * macros, mixed with fe_mul in one asm block, and looped to n=100), and it
 * resets the machine from inside fe_invert_ucode, which is nothing but
 * those calls in sequence. Dead by evidence: RCX at the vmread trigger,
 * multiply clustering, the two patches coexisting, looped firing.
 *
 * Two theories left. This runs them in order, cheapest first, so one boot
 * decides:
 *
 *   PHASE 1  CUMULATIVE. ~195 firings preceded the fatal one and nothing
 *            has ever run more in a single process. This fires 2000 using
 *            ONLY the already-cleared shape, logging a running count. If it
 *            dies, the count is the finding and it is the first evidence of
 *            state accumulating across firings.
 *
 *   PHASE 2  SHAPE. fe_invert is the only caller that ALIASES its operands:
 *            fe_sq_ucode(t, t) and fe_sq_ucode_n(t3, t3, n). Neither has
 *            ever been fired. The wrappers load every input before firing
 *            and store after, so aliasing SHOULD be safe -- which is
 *            exactly why it is worth testing rather than assuming.
 *
 *   PHASE 3  the fe_invert call sequence itself, call by call, to confirm
 *            whichever of the two it turned out to be.
 *
 * Production is untouched: this is the only build that carries the parked
 * patch, via -DENABLE_SQ_5ACC -DSQ_MASK_R8.
 *
 * THIS WILL PROBABLY CRASH. That is the point; it crashes somewhere that
 * means something.
 *
 * Build: make PROG=test_sq_why
 * Run:   sudo taskset -c 0 ./test_sq_why_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

static int g_log = -1;
static long g_fires;
static void stage(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) { (void)!write(g_log, buf, n); fsync(g_log); }
    (void)!write(1, buf, n);
}
static int wrong(const char *what, const uint64_t *in, const uint64_t *got) {
    uint64_t want[5], ra[5], rb[5];
    fe_sq_fiat(in, want); fe_reduce(ra, got); fe_reduce(rb, want);
    int bad = memcmp(ra, rb, 40) != 0;
    stage("      %s: %s", what, bad ? "WRONG vs fiat" : "matches fiat");
    return bad;
}

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    g_log = open("test_sq_why.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    sync();

    stage("00 installing (mul_patch + parked five-accumulator fe_sq)");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    stage("01 installed");

    fe a, o, t;
    for (int i = 0; i < 5; i++) a[i] = (0x123456789ABCDULL * (i + 7)) & MASK51;
    int bad = 0;

    /* ---------------- PHASE 1: is it cumulative? ---------------- */
    stage("02 PHASE 1: 2000 firings via the cleared shape, 100 at a time");
    for (int k = 0; k < 20; k++) {
        fe_sq_ucode_n(o, a, 100);
        g_fires += 100;
        stage("   fires=%ld ok", g_fires);
    }
    stage("03 PHASE 1 SURVIVED %ld firings -- NOT cumulative", g_fires);

    /* ---------------- PHASE 2: is it the aliased shape? ---------------- */
    stage("04 PHASE 2: aliased operands, never fired before");
    memcpy(t, a, 40);
    stage("   fires=%ld about to fe_sq_ucode(t, t)  ALIASED", g_fires);
    fe_sq_ucode(t, t); g_fires++;
    stage("   fires=%ld survived", g_fires);
    bad += wrong("aliased fe_sq_ucode", a, t);

    memcpy(t, a, 40);
    stage("   fires=%ld about to fe_sq_ucode_n(t, t, 10)  ALIASED", g_fires);
    fe_sq_ucode_n(t, t, 10); g_fires += 10;
    stage("   fires=%ld survived", g_fires);

    memcpy(t, a, 40);
    stage("   fires=%ld about to fe_sq_ucode_n(t, t, 100)  ALIASED", g_fires);
    fe_sq_ucode_n(t, t, 100); g_fires += 100;
    stage("   fires=%ld survived", g_fires);
    stage("05 PHASE 2 SURVIVED -- aliasing is not it either");

    /* ---------------- PHASE 3: the real sequence ---------------- */
    stage("06 PHASE 3: fe_invert_ucode, call by call");
    {
        fe z2, z9, z11, tt, t0, t1, t2, t3;
        #define SQ(dst, s)     do { stage("   fires=%ld sq %s->%s", g_fires, #s, #dst); \
                                    fe_sq_ucode(s, dst); g_fires++; } while (0)
        #define SQN(dst, s, n) do { stage("   fires=%ld sqn %s->%s n=%d", g_fires, #s, #dst, n); \
                                    fe_sq_ucode_n(dst, s, n); g_fires += n; } while (0)
        #define MU(dst, x, y)  do { stage("   fires=%ld mul %s,%s->%s", g_fires, #x, #y, #dst); \
                                    fe_mul_ucode(x, y, dst); g_fires++; } while (0)
        SQ(z2, a); SQ(tt, z2); SQ(tt, tt); MU(z9, a, tt); MU(z11, z9, z2);
        SQ(tt, z11); MU(t0, z9, tt);
        SQN(t1, t0, 5);   MU(t1, t0, t1);
        SQN(t2, t1, 10);  MU(t2, t1, t2);
        SQN(t3, t2, 20);  MU(t3, t2, t3);
        SQN(t3, t3, 10);  MU(t1, t1, t3);
        SQN(t2, t1, 50);  MU(t2, t1, t2);
        SQN(t3, t2, 100); MU(t3, t2, t3);
        SQN(t3, t3, 50);  MU(t1, t1, t3);
        SQN(t1, t1, 5);
    }
    stage("07 PHASE 3 SURVIVED, %ld total firings", g_fires);

    init_match_and_patch(); do_fix_IN_patch();
    stage("08 restored; %d wrong results", bad);
    if (g_log >= 0) close(g_log);
    return bad ? 1 : 0;
}
