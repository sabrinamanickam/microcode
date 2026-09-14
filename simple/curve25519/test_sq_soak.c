/*
 * test_sq_soak.c — long-soak validation for the five-accumulator fe_sq.
 *
 * WHY THIS EXISTS. sq_patch_5acc is correct (ucode_sim: 0/3000) and matched
 * fiat-crypto on every path where a result could be checked, yet it hard-reset
 * the machine four times. The decisive datum from that investigation: a run
 * that SURVIVES is one sample of a nondeterministic failure, so a single pass
 * proves nothing and bisecting by call shape cannot converge.
 *
 * test_sq_fire.c already covers the first-fire question -- does installing and
 * firing it once kill the box -- and its log shows the last attempt survived
 * install and five isolated firings, then died at stage 08, the RFC 7748 gate.
 * That is the full ladder: thousands of firings, back to back, interleaved
 * with fe_mul. So the failure needs SUSTAINED firing, which is exactly what
 * this harness escalates through.
 *
 * PROTOCOL
 *   - One mode per process invocation (probe_seg.c pattern). A crash costs one
 *     mode, not the whole escalation, and the mode is named on the command line
 *     so the reboot notes say what was running.
 *   - Every stage boundary and every heartbeat is fsync'd BEFORE the firings it
 *     announces, so after a hard reset the last line in test_sq_soak.log names
 *     the mode and the firing count that killed it.
 *   - Heartbeats are SPARSE (default every 10k firings). Writing between every
 *     firing would break up the back-to-back pattern that appears to be what
 *     provokes the fault, and would measure something else entirely.
 *   - Correctness is checked against fiat-crypto periodically rather than every
 *     firing, for the same reason.
 *
 * MODES
 *   smoke          5 targeted inputs, one firing each (same as test_sq_fire)
 *   soak    <n>    n separate fe_sq_ucode calls, memory round trip between
 *   chain   <n>    fe_sq_ucode_n(out,a,n): n firings back to back, state kept
 *                  in arch regs, no memory round trip. This is the shape that
 *                  killed the machine in the 2026-09-12 session.
 *   mixed   <n>    n iterations alternating fe_mul_ucode / fe_sq_ucode, the
 *                  hook-switching pattern a ladder step actually issues.
 *   rfc            the RFC 7748 gate (full ladder, ~2,561 firings per vector)
 *   ladder  <n>    n full X25519 scalar multiplications
 *
 * ESCALATION (stop at the first mode that resets the box):
 *   smoke -> soak 1000 -> chain 1000 -> mixed 1000 -> soak 100000
 *         -> chain 100000 -> mixed 100000 -> rfc -> ladder 200
 *
 * Build: make PROG=test_sq_soak EXTRA_CPPFLAGS="-DSQ_MASK_R8 -DENABLE_SQ_5ACC"
 * Run:   sudo taskset -c 0 ./test_sq_soak_static <mode> [n]
 *
 * Build WITHOUT the two defines to soak the shipped serial fe_sq instead --
 * that is the control, and it should survive every mode.
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

static int g_log = -1;
static void stage(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) { (void)!write(g_log, buf, n); fsync(g_log); }
    (void)!write(1, buf, n);            /* unbuffered: stdio can lose a crash */
}

#define HEARTBEAT 10000

/* Pseudo-random 51-bit-ish limbs; deterministic so a crashing input is
 * reproducible from the firing count in the log. */
static uint64_t rng_s = 0x243F6A8885A308D3ULL;
static uint64_t rng(void) {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return rng_s;
}
static void rand_fe(uint64_t a[5]) {
    for (int i = 0; i < 5; i++) a[i] = rng() & ((1ULL << 53) - 1);
}

static int check(const uint64_t a[5], const uint64_t got[5]) {
    uint64_t want[5], ra[5], rb[5];
    fe_sq_fiat(a, want);
    fe_reduce(ra, got); fe_reduce(rb, want);
    return memcmp(ra, rb, 40) != 0;
}

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        printf("needs root: sudo taskset -c 0 ./test_sq_soak_static <mode> [n]\n");
        return 1;
    }
    const char *mode = argc > 1 ? argv[1] : "smoke";
    long N = argc > 2 ? atol(argv[2]) : 1000;

    g_log = open("test_sq_soak.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    stage("00 mode=%s n=%ld  fe_sq variant=%s", mode, N,
#ifdef ENABLE_SQ_5ACC
          "five-accumulator (41 triads, same-register MULs eliminated)"
#else
          "shipped serial (42 triads) -- CONTROL RUN"
#endif
         );
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    stage("01 helpers done, microcode state is stock");
    stage("02 about to patch_ucode + hook");
    install_field_patches();
    stage("03 patches installed and hooked -- install did NOT crash");

    int bad = 0;
    uint64_t a[5], b[5], got[5];

    if (!strcmp(mode, "smoke")) {
        static const uint64_t cases[][5] = {
            { 1, 0, 0, 0, 0 },
            { 2, 3, 5, 7, 11 },
            { MASK51, MASK51, MASK51, MASK51, MASK51 },
            { MASK51 - 18, MASK51, MASK51, MASK51, MASK51 },
            { (1ULL<<53)-1, (1ULL<<53)-1, (1ULL<<53)-1, (1ULL<<53)-1, (1ULL<<53)-1 },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            memcpy(a, cases[i], 40);
            stage("04.%u about to FIRE, a=%016lx %016lx %016lx %016lx %016lx",
                  i, a[0], a[1], a[2], a[3], a[4]);
            fe_sq_ucode(a, got);
            bad += check(a, got);
            stage("05.%u survived%s", i, bad ? " (MISMATCH)" : "");
        }
    } else if (!strcmp(mode, "soak")) {
        stage("04 about to fire %ld SEPARATE fe_sq calls (heartbeat every %d)", N, HEARTBEAT);
        for (long i = 0; i < N; i++) {
            if (i % HEARTBEAT == 0) stage("04.hb %ld / %ld fired so far", i, N);
            rand_fe(a);
            fe_sq_ucode(a, got);
            if (i % 1000 == 0) bad += check(a, got);
        }
        stage("05 all %ld separate firings survived; %d mismatches", N, bad);
    } else if (!strcmp(mode, "chain")) {
        stage("04 about to fire fe_sq_ucode_n back-to-back, %ld deep -- THIS IS THE"
              " SHAPE THAT KILLED THE MACHINE on 2026-09-12", N);
        rand_fe(a);
        for (long i = 0; i < N; i += 100) {
            long k = (N - i) < 100 ? (N - i) : 100;
            if (i % HEARTBEAT == 0) stage("04.hb %ld / %ld fired so far", i, N);
            fe_sq_ucode_n(got, a, (int)k);
            memcpy(a, got, 40);
        }
        stage("05 chain of %ld back-to-back firings survived", N);
    } else if (!strcmp(mode, "mixed")) {
        stage("04 about to fire %ld alternating fe_mul/fe_sq pairs (hook switching)", N);
        rand_fe(a); rand_fe(b);
        for (long i = 0; i < N; i++) {
            if (i % HEARTBEAT == 0) stage("04.hb %ld / %ld pairs fired so far", i, N);
            fe_mul_ucode(a, b, got);
            memcpy(a, got, 40);
            fe_sq_ucode(a, got);
            memcpy(a, got, 40);
        }
        stage("05 %ld alternating pairs survived", N);
    } else if (!strcmp(mode, "rfc")) {
        stage("04 about to run the RFC 7748 gate -- the previous attempt died HERE"
              " (test_sq_fire.log ended at stage 08)");
        bad += test_rfc7748();
        stage("05 RFC 7748 gate returned, bad=%d", bad);
    } else if (!strcmp(mode, "ladder")) {
        uint8_t sc[32], pt[32], out[32];
        hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
        hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
        stage("04 about to run %ld full X25519 (~%ld firings)", N, N * 2561);
        for (long i = 0; i < N; i++) {
            if (i % 10 == 0) stage("04.hb %ld / %ld scalarmults", i, N);
            x25519(out, sc, pt);
            if (memcmp_hex(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32))
                { bad++; stage("04.%ld WRONG RESULT", i); break; }
        }
        stage("05 %ld scalarmults survived; %d wrong", N, bad);
    } else {
        stage("XX unknown mode '%s'", mode);
        bad = 1;
    }

    init_match_and_patch();
    do_fix_IN_patch();
    stage("06 microcode state restored; verdict %s", bad ? "FAIL" : "PASS");
    if (g_log >= 0) close(g_log);
    return bad ? 1 : 0;
}
