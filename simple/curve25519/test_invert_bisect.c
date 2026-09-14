/*
 * test_invert_bisect.c — which of fe_invert_ucode's 30 calls kills the box?
 *
 * WHERE WE ARE. The new five-accumulator fe_sq patch is CORRECT: it has
 * matched fiat-crypto on every path tested, including inputs up to 2^53-1.
 * It is also survivable in every ingredient of fe_invert_ucode, tested
 * individually and logged:
 *
 *     fe_sq_ucode standalone, 5 firings          survived, correct
 *     FE_SQ macro, memory to memory              survived, correct
 *     FE_SQ_FROM_REGS                            survived, correct
 *     fe_mul and fe_sq in ONE asm block          survived, correct
 *     fe_sq_ucode_n at n = 1,2,3,4,5             survived, correct
 *     fe_sq_ucode_n at n = 10,20,50,100          survived
 *
 * That is ~195 consecutive firings without incident. Then
 * fe_invert_ucode -- which is nothing but those same calls in sequence --
 * killed the machine. Three hard resets so far.
 *
 * So the fault is NOT a particular call path. Two hypotheses remain and
 * this separates them:
 *
 *   A. a specific CALL in fe_invert, most likely one of the shapes not
 *      covered above: fe_sq_ucode ALIASED (out == in, `fe_sq_ucode(t,t)`),
 *      fe_sq_ucode_n ALIASED (`fe_sq_ucode_n(t3,t3,10)`), or
 *      fe_mul_ucode, whose C-wrapper form has not been fired once in any
 *      of these tests -- only its FE_MUL macro form has.
 *
 *   B. something CUMULATIVE. fe_invert is ~260 firings on top of the ~195
 *      already done, and nothing here has run that many in one process.
 *
 * This is fe_invert_ucode reproduced call for call with a fsynced stage
 * line around every one, plus a running firing count. If it dies at a
 * named call, that is hypothesis A and the call names the shape. If it
 * dies at a similar TOTAL COUNT regardless of which call it is on, that is
 * hypothesis B, and the count is the finding.
 *
 * THIS WILL PROBABLY CRASH. It is a bisect, not a fix. Do not run it
 * unless the information is worth a reboot.
 *
 * Build: make PROG=test_invert_bisect
 * Run:   sudo taskset -c 0 ./test_invert_bisect_static
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

#define SQ(o, i)      do { stage("  fires=%ld  about to fe_sq_ucode(%s -> %s)%s",       \
                                 g_fires, #i, #o, (void*)(i)==(void*)(o) ? "  ALIASED" : ""); \
                           fe_sq_ucode(i, o); g_fires++;                                \
                           stage("  fires=%ld  ok", g_fires); } while (0)
#define SQN(o, i, n)  do { stage("  fires=%ld  about to fe_sq_ucode_n(%s -> %s, n=%d)%s", \
                                 g_fires, #i, #o, n, (void*)(i)==(void*)(o) ? "  ALIASED" : ""); \
                           fe_sq_ucode_n(o, i, n); g_fires += (n);                      \
                           stage("  fires=%ld  ok", g_fires); } while (0)
#define MUL(o, x, y)  do { stage("  fires=%ld  about to fe_mul_ucode(%s,%s -> %s)%s",   \
                                 g_fires, #x, #y, #o,                                   \
                                 ((void*)(x)==(void*)(o)||(void*)(y)==(void*)(o)) ? "  ALIASED" : ""); \
                           fe_mul_ucode(x, y, o); g_fires++;                            \
                           stage("  fires=%ld  ok", g_fires); } while (0)

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    g_log = open("test_invert_bisect.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    sync();

    stage("00 helpers + install");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    stage("01 installed");

    fe z, z2, z9, z11, t, t0, t1, t2, t3;
    for (int i = 0; i < 5; i++) z[i] = (0x123456789ABCDULL * (i + 7)) & MASK51;

    /* fe_invert_ucode, call for call, each one logged. */
    stage("02 entering the fe_invert_ucode sequence");
    SQ(z2, z);
    SQ(t, z2);
    SQ(t, t);                       /* first ALIASED fe_sq_ucode */
    MUL(z9, z, t);                  /* first fe_mul_ucode -- C wrapper, never yet fired here */
    MUL(z11, z9, z2);
    SQ(t, z11);
    MUL(t0, z9, t);
    SQN(t1, t0, 5);   MUL(t1, t0, t1);
    SQN(t2, t1, 10);  MUL(t2, t1, t2);
    SQN(t3, t2, 20);  MUL(t3, t2, t3);
    SQN(t3, t3, 10);  MUL(t1, t1, t3);      /* first ALIASED fe_sq_ucode_n */
    SQN(t2, t1, 50);  MUL(t2, t1, t2);
    SQN(t3, t2, 100); MUL(t3, t2, t3);
    SQN(t3, t3, 50);  MUL(t1, t1, t3);
    SQN(t1, t1, 5);
    stage("03 sequence complete, %ld firings, nothing crashed", g_fires);

    init_match_and_patch(); do_fix_IN_patch();
    stage("04 restored");
    if (g_log >= 0) close(g_log);
    return 0;
}
